#include "batcan/service.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
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
constexpr const char *kUpdateUnitPath =
    "/etc/systemd/system/batcan-update.service";
constexpr const char *kUpdateTimerPath =
    "/etc/systemd/system/batcan-update.timer";
constexpr const char *kLogDirectory = "/var/log/batcan";
constexpr const char *kPrivateLogDirectory = "/var/log/private/batcan";
constexpr const char *kServiceUser = "ubuntu";
constexpr const char *kROSSetup = "/opt/ros/humble/setup.bash";

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
  return "[Unit]\n"
         "Description=CAN battery to ROS2 bridge\n"
         "After=network-online.target\n"
         "Wants=network-online.target\n\n"
         "[Service]\n"
         "Type=simple\n"
         "User=" + std::string(kServiceUser) +
         "\nGroup=" + kServiceUser +
         "\nExecStart=/bin/bash -lc 'source " + kROSSetup +
         " && exec /opt/batcan/batcan run --config " + kInstalledConfig +
         "'\nRestart=always\nRestartSec=3\n"
         "Environment=ROS_LOCALHOST_ONLY=1\n"
         "Environment=ROS_LOG_DIR=/var/log/batcan/ros\n"
         "LogsDirectory=batcan\nLogsDirectoryMode=0750\n"
         "AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN\n"
         "CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN\n"
         "NoNewPrivileges=true\n"
         "ProtectSystem=strict\nProtectHome=read-only\n"
         "ReadOnlyPaths=/opt/batcan/config.yml\n"
         "PrivateTmp=true\nProtectKernelTunables=true\n"
         "ProtectControlGroups=true\nRestrictSUIDSGID=true\n"
         "RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_CAN AF_INET AF_INET6\n\n"
         "[Install]\nWantedBy=multi-user.target\n";
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

base_url=https://github.com/chaeoi/batcan/releases/latest/download
curl --fail --location --silent --show-error --retry 3 \
  "$base_url/$asset" -o "$temporary_directory/batcan"
curl --fail --location --silent --show-error --retry 3 \
  "$base_url/SHA256SUMS" -o "$temporary_directory/SHA256SUMS"

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

std::string updateUnit() {
  return "[Unit]\n"
         "Description=Update batcan from GitHub Releases\n"
         "After=network-online.target\n"
         "Wants=network-online.target\n\n"
         "[Service]\n"
         "Type=oneshot\n"
         "ExecStart=" + std::string(kUpdateScriptPath) + "\n";
}

std::string updateTimer() {
  return "[Unit]\n"
         "Description=Daily batcan update check\n\n"
         "[Timer]\n"
         "OnBootSec=10min\n"
         "OnUnitActiveSec=24h\n"
         "Persistent=true\n"
         "RandomizedDelaySec=1h\n\n"
         "[Install]\nWantedBy=timers.target\n";
}

void installUpdateArtifacts() {
  writeFileAtomic(kUpdateScriptPath, updateScript(), 0755);
  writeFileAtomic(kUpdateUnitPath, updateUnit(), 0644);
  writeFileAtomic(kUpdateTimerPath, updateTimer(), 0644);
  runCommand({"systemctl", "daemon-reload"});
  runCommand({"systemctl", "enable", "--now", "batcan-update.timer"});
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
  installUpdateArtifacts();
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
  runCommand({"systemctl", "disable", "--now", "batcan-update.timer"},
             true);
  std::filesystem::remove(kUnitPath);
  std::filesystem::remove(kUpdateUnitPath);
  std::filesystem::remove(kUpdateTimerPath);
  std::filesystem::remove(kUpdateScriptPath);
  runCommand({"systemctl", "daemon-reload"});
  runCommand({"systemctl", "reset-failed", "batcan.service"}, true);
  runCommand({"systemctl", "reset-failed", "batcan-update.service"}, true);
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
        "service requires install, uninstall or status");
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
    installUpdateArtifacts();
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

}  // namespace batcan
