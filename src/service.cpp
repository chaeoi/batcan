#include "batcan/service.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "batcan/config.hpp"

namespace batcan {
namespace {

constexpr const char *kInstallDirectory = "/opt/batcan";
constexpr const char *kInstalledBinary = "/opt/batcan/batcan";
constexpr const char *kInstalledConfig = "/opt/batcan/config.yml";
constexpr const char *kUnitPath =
    "/etc/systemd/system/batcan.service";
constexpr const char *kUpdateScriptPath = "/opt/batcan/update.sh";
constexpr const char *kLogDirectory = "/var/log/batcan";
constexpr const char *kPrivateLogDirectory = "/var/log/private/batcan";
constexpr const char *kROSSetup = "/opt/ros/humble/setup.bash";
constexpr auto kInitialUpdateDelay = std::chrono::seconds(30);
constexpr auto kUpdateInterval = std::chrono::hours(24);

void requireRoot() {
  if (::geteuid() != 0) {
    throw std::runtime_error(
        "service changes require root; run with sudo");
  }
}

void writeFileAtomic(const std::string &path, const std::string &content,
                     mode_t mode) {
  const auto temporary = path + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot write " + temporary);
    }
    stream << content;
    stream.flush();
    if (!stream) {
      throw std::runtime_error("cannot flush " + temporary);
    }
  }
  if (::chmod(temporary.c_str(), mode) != 0) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("cannot set permissions on " + temporary);
  }
  std::filesystem::rename(temporary, path);
}

int runCommand(const std::vector<std::string> &arguments,
               bool allow_failure = false) {
  const auto child = ::fork();
  if (child < 0) {
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    std::vector<char *> values;
    values.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) {
      values.push_back(const_cast<char *>(argument.c_str()));
    }
    values.push_back(nullptr);
    ::execvp(values[0], values.data());
    _exit(127);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    throw std::runtime_error("waitpid failed");
  }
  const auto code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  if (code != 0 && !allow_failure) {
    throw std::runtime_error(arguments.front() + " exited with status " +
                             std::to_string(code));
  }
  return code;
}

std::string serviceUnit() {
  std::string unit =
      "[Unit]\n"
      "Description=CAN battery to ROS2 bridge\n"
      "After=network-online.target\n"
      "Wants=network-online.target\n\n"
      "[Service]\n"
      "Type=simple\n";
  unit += "ExecStart=/bin/bash -lc 'source ";
  unit += kROSSetup;
  unit += " && exec /opt/batcan/batcan run --config ";
  unit += kInstalledConfig;
  unit +=
      "'\nRestart=always\nRestartSec=3\n"
      "Environment=ROS_LOCALHOST_ONLY=1\n"
      "Environment=ROS_LOG_DIR=/var/log/batcan/ros\n"
      "LogsDirectory=batcan\nLogsDirectoryMode=0750\n"
      "RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_CAN AF_INET AF_INET6\n\n"
      "[Install]\nWantedBy=multi-user.target\n";
  return unit;
}

std::string updateScript() {
  return R"BATCAN(#!/bin/bash
set -eu

binary=/opt/batcan/batcan
exec 9>/run/lock/batcan-update.lock
flock -n 9 || exit 0
temporary_directory="$(mktemp -d /tmp/batcan-update.XXXXXX)"
cleanup() {
  rm -rf "$temporary_directory"
}
trap cleanup EXIT

case "$(uname -m)" in
  aarch64|arm64) asset=batcan-linux-arm64 ;;
  x86_64|amd64) asset=batcan-linux-amd64 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

base_url=https://gitwarp.canghai.org/github.com/chaeoi/batcan/releases/latest/download
proxy_base_url=https://github.com/chaeoi/batcan/releases/latest/download
cache_buster="$(date +%s)-$$"
download_release() {
  local name="$1"
  local destination="$2"
  for base in "$base_url" "$proxy_base_url"; do
    if curl --connect-timeout 8 --max-time 20 --fail --location \
      --silent --show-error --retry 1 \
      "$base/$name?batcan_cache=$cache_buster" -o "$destination"; then
      return 0
    fi
    rm -f "$destination"
  done
  echo "cannot download release asset: $name" >&2
  return 1
}
download_release "$asset" "$temporary_directory/batcan"
download_release SHA256SUMS "$temporary_directory/SHA256SUMS"

expected_hash="$(awk -v asset="$asset" '$2 == asset {print $1; exit}' \
  "$temporary_directory/SHA256SUMS")"
if [ -z "$expected_hash" ]; then
  echo "release checksum is missing for $asset" >&2
  exit 1
fi
actual_hash="$(sha256sum "$temporary_directory/batcan" | awk '{print $1}')"
if [ "$expected_hash" != "$actual_hash" ]; then
  echo "release checksum mismatch" >&2
  exit 1
fi

new_version="$(chmod +x "$temporary_directory/batcan"; \
  "$temporary_directory/batcan" --version)"
current_hash=""
if [ -f "$binary" ]; then
  current_hash="$(sha256sum "$binary" | awk '{print $1}')"
fi
if [ "$current_hash" = "$expected_hash" ]; then
  echo "batcan is up to date: $new_version"
  exit 0
fi

install -m 0755 "$temporary_directory/batcan" "$binary.new"
mv -f "$binary.new" "$binary"
echo "updated batcan to $new_version"
if systemctl is-active --quiet batcan.service; then
  systemctl restart batcan.service
fi
)BATCAN";
}

void installUpdateScript() {
  writeFileAtomic(kUpdateScriptPath, updateScript(), 0755);
}

void installService(const std::vector<std::string> &arguments,
                    const std::string &executable_path) {
  requireRoot();
  bool force_config = false;
  for (const auto &argument : arguments) {
    if (argument == "--force-config") {
      force_config = true;
    } else {
      throw std::runtime_error("unknown service install option: " + argument);
    }
  }

  std::filesystem::create_directories(kInstallDirectory);
  bool valid_config = false;
  if (!force_config && std::filesystem::exists(kInstalledConfig)) {
    try {
      (void)loadConfig(kInstalledConfig);
      valid_config = true;
    } catch (const std::exception &) {
      writeFileAtomic(kInstalledConfig, defaultConfig(), 0644);
    }
  } else {
    writeFileAtomic(kInstalledConfig, defaultConfig(), 0644);
  }

  const auto temporary_binary =
      std::string(kInstalledBinary) + ".tmp." + std::to_string(::getpid());
  if (std::filesystem::weakly_canonical(executable_path) !=
      std::filesystem::weakly_canonical(kInstalledBinary)) {
    std::filesystem::copy_file(
        executable_path, temporary_binary,
        std::filesystem::copy_options::overwrite_existing);
    ::chmod(temporary_binary.c_str(), 0755);
    std::filesystem::rename(temporary_binary, kInstalledBinary);
  }
  writeFileAtomic(kUnitPath, serviceUnit(), 0644);
  installUpdateScript();
  runCommand({"systemctl", "daemon-reload"});
  if (!valid_config) {
    runCommand({"systemctl", "disable", "--now", "batcan.service"}, true);
    std::cout << "installed batcan.service; select one profile in "
              << kInstalledConfig << " and start it with: systemctl enable "
              << "--now batcan\n";
    return;
  }
  runCommand({"systemctl", "enable", "batcan.service"});
  runCommand({"systemctl", "restart", "batcan.service"});
  std::cout << "installed and started batcan.service with config "
            << kInstalledConfig << '\n';
}

void uninstallService(const std::vector<std::string> &arguments) {
  requireRoot();
  if (!arguments.empty()) {
    throw std::runtime_error("service uninstall takes no options");
  }
  runCommand({"systemctl", "disable", "--now", "batcan.service"},
             true);
  std::filesystem::remove(kUnitPath);
  std::filesystem::remove(kUpdateScriptPath);
  runCommand({"systemctl", "daemon-reload"});
  runCommand({"systemctl", "reset-failed", "batcan.service"}, true);
  std::filesystem::remove_all(kLogDirectory);
  std::filesystem::remove_all(kPrivateLogDirectory);
  std::cout << "uninstalled batcan.service; preserved " << kInstallDirectory
            << '\n';
}

}  // namespace

int serviceCommand(const std::vector<std::string> &arguments,
                   const std::string &executable_path) {
  if (arguments.empty()) {
    throw std::runtime_error(
        "service requires install, update, uninstall or status");
  }
  const std::vector<std::string> options(arguments.begin() + 1,
                                         arguments.end());
  if (arguments.front() == "install") {
    installService(options, executable_path);
    return 0;
  }
  if (arguments.front() == "uninstall") {
    uninstallService(options);
    return 0;
  }
  if (arguments.front() == "update") {
    requireRoot();
    if (!options.empty()) {
      throw std::runtime_error("service update takes no options");
    }
    installUpdateScript();
    return runCommand({kUpdateScriptPath});
  }
  if (arguments.front() == "status") {
    if (!options.empty()) {
      throw std::runtime_error("service status takes no options");
    }
    return runCommand({"systemctl", "status", "batcan.service"},
                      true);
  }
  throw std::runtime_error("unknown service command: " + arguments.front());
}

void automaticUpdateLoop(std::atomic_bool &stopping,
                         std::condition_variable &wake,
                         std::mutex &wake_mutex) {
  std::unique_lock lock(wake_mutex);
  if (wake.wait_for(lock, kInitialUpdateDelay,
                    [&stopping]() { return stopping.load(); })) {
    return;
  }
  while (!stopping.load()) {
    lock.unlock();
    try {
      std::cerr << "batcan automatic update check started\n";
      const auto result = runCommand({kUpdateScriptPath}, true);
      if (result != 0) {
        std::cerr << "batcan automatic update exited with status "
                  << result << '\n';
      }
    } catch (const std::exception &error) {
      std::cerr << "batcan automatic update failed: " << error.what()
                << '\n';
    }
    lock.lock();
    if (wake.wait_for(lock, kUpdateInterval,
                      [&stopping]() { return stopping.load(); })) {
      return;
    }
  }
}

}  // namespace batcan
