#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace batcan {

struct FieldConfig {
  std::string metric;
  std::size_t offset = 0;
  std::size_t length = 0;
  std::string encoding = "uint";
  std::string endian = "big";
  double scale = 1.0;
  double bias = 0.0;
};

struct ResponseConfig {
  std::uint32_t id = 0;
  bool extended = true;
  std::vector<FieldConfig> fields;
};

struct QueryConfig {
  std::string name;
  std::uint32_t request_id = 0;
  bool extended = true;
  std::vector<std::uint8_t> request_data;
  std::vector<ResponseConfig> responses;
};

struct CanConfig {
  std::string interface = "can5";
  int query_interval_ms = 2000;
  int response_timeout_ms = 800;
  std::vector<QueryConfig> queries;
};

struct RosConfig {
  std::string topic = "/bms_can/battery_data";
  std::string frame_id = "battery";
  bool localhost_only = true;
  int domain_id = 0;
  int qos_depth = 10;
};

struct Config {
  std::string robot_model;
  CanConfig can;
  RosConfig ros;
};

Config loadConfig(const std::string &path);
Config profileForModel(const std::string &robot_model);
std::string defaultConfig(const std::string &robot_model = "");

}  // namespace batcan
