#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
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
  std::map<std::uint8_t, std::uint8_t> value_map;
};

struct ResponseConfig {
  std::uint32_t id = 0;
  bool extended = true;
  std::vector<FieldConfig> fields;
  // Zero means an exact ID match; otherwise only the masked ID bits match.
  std::uint32_t id_mask = 0;
};

struct QueryConfig {
  std::string name;
  bool send_request = true;
  std::uint32_t request_id = 0;
  bool extended = true;
  std::vector<std::uint8_t> request_data;
  std::vector<ResponseConfig> responses;
};

struct CanConfig {
  std::string interface = "can5";
  int bitrate = 250000;
  int query_interval_ms = 2000;
  int response_timeout_ms = 800;
  std::vector<QueryConfig> queries;
};

struct RosConfig {
  std::string topic = "/batcan/data";
  std::string frame_id = "battery";
  bool localhost_only = true;
  int domain_id = 0;
  int qos_depth = 10;
};

struct Config {
  std::string model;
  std::string bms_model;
  CanConfig can;
  RosConfig ros;
};

Config loadConfig(const std::string &path);
std::string defaultConfig();

}  // namespace batcan
