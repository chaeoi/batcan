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

constexpr const char *kInstalledBinary = "/usr/local/bin/batcan";
constexpr const char *kConfigDirectory = "/etc/batcan";
constexpr const char *kInstalledConfig = "/etc/batcan/config.yml";
constexpr const char *kUnitPath =
    "/etc/systemd/system/batcan.service";
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
         " && exec /usr/local/bin/batcan run --config " + kInstalledConfig +
         "'\nRestart=always\nRestartSec=3\n"
         "Environment=ROS_LOCALHOST_ONLY=1\n"
         "Environment=ROS_LOG_DIR=/var/log/batcan/ros\n"
         "LogsDirectory=batcan\nLogsDirectoryMode=0750\n"
         "AmbientCapabilities=CAP_NET_RAW\n"
         "CapabilityBoundingSet=CAP_NET_RAW\n"
         "NoNewPrivileges=true\n"
         "ProtectSystem=strict\nProtectHome=read-only\n"
         "ReadOnlyPaths=/etc/batcan/config.yml\n"
         "PrivateTmp=true\nProtectKernelTunables=true\n"
         "ProtectControlGroups=true\nRestrictSUIDSGID=true\n"
         "RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_CAN AF_INET AF_INET6\n\n"
         "[Install]\nWantedBy=multi-user.target\n";
}

void installService(const std::vector<std::string> &arguments,
                    const std::string &executable_path) {
  requireRoot();
  std::string robot_model;
  bool force_config = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--robot-model" && index + 1 < arguments.size()) {
      robot_model = arguments[++index];
    } else if (arguments[index] == "--force-config") {
      force_config = true;
    } else {
      throw std::runtime_error("unknown service install option: " +
                               arguments[index]);
    }
  }

  std::filesystem::create_directories(kConfigDirectory);
  if (!robot_model.empty()) {
    profileForModel(robot_model);
    if (std::filesystem::exists(kInstalledConfig) && !force_config) {
      const auto existing = loadConfig(kInstalledConfig);
      if (existing.robot_model != robot_model) {
        throw std::runtime_error(
            "installed config selects " + existing.robot_model +
            "; use --force-config to replace it");
      }
    } else {
      writeFileAtomic(kInstalledConfig, defaultConfig(robot_model), 0644);
    }
  } else if (!std::filesystem::exists(kInstalledConfig)) {
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
  runCommand({"systemctl", "daemon-reload"});
  runCommand({"systemctl", "enable", "batcan.service"});
  try {
    loadConfig(kInstalledConfig);
  } catch (const std::exception &) {
    std::cout << "installed batcan.service; set robot_model in "
              << kInstalledConfig
              << " and start it with: systemctl start batcan\n";
    return;
  }
  runCommand({"systemctl", "restart", "batcan.service"});
  std::cout << "installed and started batcan.service with config "
            << kInstalledConfig << '\n';
}

void uninstallService(const std::vector<std::string> &arguments) {
  requireRoot();
  bool purge = false;
  for (const auto &argument : arguments) {
    if (argument == "--purge") {
      purge = true;
    } else {
      throw std::runtime_error("unknown service uninstall option: " +
                               argument);
    }
  }
  runCommand({"systemctl", "disable", "--now", "batcan.service"},
             true);
  std::filesystem::remove(kUnitPath);
  runCommand({"systemctl", "daemon-reload"});
  runCommand({"systemctl", "reset-failed", "batcan.service"}, true);
  if (purge) {
    std::filesystem::remove(kInstalledConfig);
    std::filesystem::remove(kConfigDirectory);
    std::filesystem::remove(kInstalledBinary);
  }
  std::cout << "uninstalled batcan.service"
            << (purge ? " and purged its files" : " (config preserved)")
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
