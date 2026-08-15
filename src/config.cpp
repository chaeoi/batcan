#include "batcan/config.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>

namespace batcan {
namespace {

constexpr const char *kBatteryTopic = "/bms_can/battery_data";

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

FieldConfig field(std::string metric, std::size_t offset, std::size_t length,
                  std::string encoding = "uint", std::string endian = "big",
                  double scale = 1.0, double bias = 0.0) {
  return FieldConfig{std::move(metric), offset, length, std::move(encoding),
                     std::move(endian), scale, bias};
}

Config build2MProfile() {
  Config config;
  config.robot_model = "2m_v0.1.2";
  config.can.interface = "can5";
  config.can.query_interval_ms = 2000;
  config.can.response_timeout_ms = 800;
  config.can.queries = {{
      "xinxiangyang", 0x0400FF80U, true, {0, 0, 0, 0, 0, 0, 0, 0},
      {{0x04028001U,
        true,
        {field("voltage", 0, 2, "uint", "big", 0.1),
         field("current", 2, 2, "uint", "big", 0.1, -3000.0),
         field("percentage", 4, 2, "uint", "big", 0.001)}},
       {0x04038001U, true, {field("temperature", 4, 1, "uint", "big", 1.0, -40.0)}},
       {0x04078001U, true, {field("power_supply_status", 0, 1)}}},
  }};
  config.ros.topic = kBatteryTopic;
  config.ros.frame_id = "battery";
  config.ros.localhost_only = true;
  config.ros.domain_id = 0;
  config.ros.qos_depth = 10;
  return config;
}

const std::regex kModelName("^[A-Za-z0-9._-]{1,64}$");

}  // namespace

Config profileForModel(const std::string &robot_model) {
  if (!std::regex_match(robot_model, kModelName)) {
    throw std::runtime_error("robot_model is invalid");
  }
  if (robot_model == "2m_v0.1.2") {
    return build2MProfile();
  }
  throw std::runtime_error("unsupported robot_model " + robot_model +
                           "; install a release that supports this model");
}

Config loadConfig(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot read config " + path);
  }
  std::string model;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim(std::move(line));
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
      throw std::runtime_error("config line " + std::to_string(line_number) +
                               " must use robot_model: MODEL");
    }
    const auto key = trim(line.substr(0, separator));
    auto value = trim(line.substr(separator + 1));
    if (key != "robot_model" || !model.empty()) {
      throw std::runtime_error("config only accepts one robot_model field");
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    model = value;
  }
  if (model.empty()) {
    throw std::runtime_error("robot_model is required");
  }
  return profileForModel(model);
}

std::string defaultConfig(const std::string &robot_model) {
  return "# Select a robot model compiled into this release.\n"
         "# The model defines CAN interface, requests and response parsing.\n"
         "# The bridge always publishes sensor_msgs/msg/BatteryState on "
         "/bms_can/battery_data.\n"
         "robot_model: " + robot_model + "\n";
}

}  // namespace batcan
