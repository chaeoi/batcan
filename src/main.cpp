#include <limits.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "batcan/bridge.hpp"
#include "batcan/config.hpp"
#include "batcan/service.hpp"

#ifndef BATCAN_VERSION
#define BATCAN_VERSION "dev"
#endif

namespace {

std::string executablePath() {
  std::vector<char> buffer(PATH_MAX + 1);
  const auto length = ::readlink("/proc/self/exe", buffer.data(), PATH_MAX);
  if (length < 0) {
    throw std::runtime_error("cannot resolve /proc/self/exe");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(length));
}

void usage(std::ostream &stream) {
  stream << "Usage:\n"
         << "  batcan run [--config PATH]\n"
         << "  batcan --check-config [--config PATH]\n"
         << "  batcan service install [--force-config]\n"
         << "  batcan service uninstall\n"
         << "  batcan service status\n"
         << "  batcan --version\n";
}

}  // namespace

int main(int argc, char **argv) {
  try {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    if (!arguments.empty() && arguments.front() == "service") {
      return batcan::serviceCommand(
          std::vector<std::string>(arguments.begin() + 1, arguments.end()),
          executablePath());
    }
    if (!arguments.empty() && arguments.front() == "--version") {
      std::cout << BATCAN_VERSION << '\n';
      return 0;
    }
    if (!arguments.empty() &&
        (arguments.front() == "--help" || arguments.front() == "-h")) {
      usage(std::cout);
      return 0;
    }

    bool check_config = false;
    std::string config_path = "/opt/batcan/config.yml";
    std::size_t index = 0;
    if (!arguments.empty() && arguments.front() == "run") {
      index = 1;
    }
    for (; index < arguments.size(); ++index) {
      if (arguments[index] == "--config" && index + 1 < arguments.size()) {
        config_path = arguments[++index];
      } else if (arguments[index] == "--check-config") {
        check_config = true;
      } else {
        throw std::runtime_error("unknown option: " + arguments[index]);
      }
    }
    const auto config = batcan::loadConfig(config_path);
    if (check_config) {
      std::cout << "config is valid\n";
      return 0;
    }

    ::setenv("ROS_LOCALHOST_ONLY", config.ros.localhost_only ? "1" : "0", 1);
    const auto domain = std::to_string(config.ros.domain_id);
    ::setenv("ROS_DOMAIN_ID", domain.c_str(), 1);
    rclcpp::init(0, nullptr);
    rclcpp::spin(std::make_shared<batcan::BatteryBridge>(config));
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "batcan: " << error.what() << '\n';
    return 1;
  }
}
