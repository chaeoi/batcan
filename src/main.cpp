#include <limits.h>
#include <unistd.h>

#include <cstdlib>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <rclcpp/rclcpp.hpp>

#include "batcan/bridge.hpp"
#include "batcan/config.hpp"
#include "batcan/service.hpp"

namespace {

// Keep the transport profile in memory; ROS_LOCALHOST_ONLY=1 would add SHM
// back in Humble, so the profile restricts UDP to loopback itself.
void configureLocalTransport() {
  constexpr char profile[] = R"ROS_PROFILE(<?xml version="1.0" encoding="UTF-8"?>
<profiles xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <transport_descriptors>
    <transport_descriptor>
      <transport_id>udp_transport</transport_id>
      <type>UDPv4</type>
      <interfaceWhiteList><address>127.0.0.1</address></interfaceWhiteList>
      <maxInitialPeersRange>100</maxInitialPeersRange>
    </transport_descriptor>
  </transport_descriptors>
  <participant profile_name="monitor_udp" is_default_profile="true">
    <rtps>
      <builtin>
        <initialPeersList>
          <locator><udpv4><address>127.0.0.1</address></udpv4></locator>
        </initialPeersList>
      </builtin>
      <userTransports><transport_id>udp_transport</transport_id></userTransports>
      <useBuiltinTransports>false</useBuiltinTransports>
    </rtps>
  </participant>
</profiles>
)ROS_PROFILE";
  auto *factory = eprosima::fastdds::dds::DomainParticipantFactory::get_instance();
  if (factory->load_XML_profiles_string(profile, sizeof(profile) - 1) !=
      ReturnCode_t::RETCODE_OK) {
    throw std::runtime_error("cannot load ROS2 transport profile");
  }
  // The XML owns loopback restriction; Humble's localhost switch adds SHM.
  if (::setenv("ROS_LOCALHOST_ONLY", "0", 1) != 0) {
    throw std::runtime_error("cannot configure ROS2 transport");
  }
}

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
         << "  batcan --print-default-config\n"
         << "  batcan service install [--force-config]\n"
         << "  batcan service update\n"
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
    if (arguments.size() == 1 && arguments.front() == "--print-default-config") {
      std::cout << batcan::defaultConfig();
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

    configureLocalTransport();
    const auto domain = std::to_string(config.ros.domain_id);
    ::setenv("ROS_DOMAIN_ID", domain.c_str(), 1);
    rclcpp::init(0, nullptr);
    auto node = std::make_shared<batcan::BatteryBridge>(config);
    std::atomic_bool stopping = false;
    std::condition_variable update_wake;
    std::mutex update_mutex;
    const auto installed_binary = std::filesystem::weakly_canonical(
        "/opt/batcan/batcan");
    const auto running_binary = std::filesystem::weakly_canonical(
        executablePath());
    const bool auto_update = running_binary == installed_binary;
    std::thread update_thread;
    if (auto_update) {
      update_thread = std::thread(batcan::checkUpdateAfterStartup,
                                  std::ref(stopping), std::ref(update_wake),
                                  std::ref(update_mutex));
    }
    rclcpp::spin(node);
    {
      std::lock_guard lock(update_mutex);
      stopping.store(true);
    }
    update_wake.notify_one();
    if (update_thread.joinable()) {
      update_thread.join();
    }
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "batcan: " << error.what() << '\n';
    return 1;
  }
}
