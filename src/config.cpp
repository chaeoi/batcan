#include "batcan/config.hpp"

#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "batcan/models.hpp"

namespace batcan {
namespace {

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string removeComment(const std::string &line) {
  bool single_quoted = false;
  bool double_quoted = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char value = line[index];
    if (double_quoted && value == '\\' && !escaped) {
      escaped = true;
      continue;
    }
    if (value == '\'' && !double_quoted && !escaped) {
      single_quoted = !single_quoted;
    } else if (value == '"' && !single_quoted && !escaped) {
      double_quoted = !double_quoted;
    } else if (value == '#' && !single_quoted && !double_quoted &&
               (index == 0 || line[index - 1] == ' ' ||
                line[index - 1] == '\t')) {
      return line.substr(0, index);
    }
    escaped = false;
  }
  return line;
}

std::vector<std::string> splitProfiles(std::string value) {
  value = trim(value);
  if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
    value = trim(value.substr(1, value.size() - 2));
  }
  std::vector<std::string> profiles;
  std::set<std::string> unique;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto separator = value.find(',', start);
    auto item = trim(value.substr(start, separator - start));
    if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
      item = item.substr(1, item.size() - 2);
    }
    if (!item.empty()) {
      if (!unique.insert(item).second) {
        throw std::runtime_error(
            "profile must contain unique non-empty selectors");
      }
      profiles.push_back(std::move(item));
    }
    if (separator == std::string::npos) {
      break;
    }
    start = separator + 1;
  }
  return profiles;
}

}  // namespace

Config loadConfig(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot read config " + path);
  }
  std::string auto_mode;
  std::vector<std::string> profile_list;
  std::set<std::string> unique_profiles;
  std::string interface;
  std::string line;
  std::size_t line_number = 0;
  bool in_profile_sequence = false;

  while (std::getline(input, line)) {
    ++line_number;
    line = trim(removeComment(line));
    if (line.empty()) {
      continue;
    }

    // Support YAML sequence items under profile:, e.g. "- UUID"
    if (line.front() == '-') {
      if (!in_profile_sequence) {
        throw std::runtime_error("config line " + std::to_string(line_number) +
                                 ": unexpected sequence item");
      }
      auto item = trim(line.substr(1));
      if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
        item = item.substr(1, item.size() - 2);
      }
      if (item.empty()) {
        throw std::runtime_error("config line " + std::to_string(line_number) +
                                 ": empty profile item");
      }
      if (!unique_profiles.insert(item).second) {
        throw std::runtime_error("profile must contain unique selectors");
      }
      profile_list.push_back(std::move(item));
      continue;
    }

    in_profile_sequence = false;

    const auto separator = line.find(':');
    if (separator == std::string::npos) {
      throw std::runtime_error("config line " + std::to_string(line_number) +
                               " is invalid");
    }
    const auto key = trim(line.substr(0, separator));
    auto value = trim(line.substr(separator + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }

    if (key == "auto") {
      if (!auto_mode.empty()) {
        throw std::runtime_error("config only accepts one auto field");
      }
      if (value != "enable" && value != "disable") {
        throw std::runtime_error("auto must be enable or disable");
      }
      auto_mode = value;
    } else if (key == "profile") {
      if (!profile_list.empty()) {
        throw std::runtime_error("config only accepts one profile field");
      }
      if (value.empty()) {
        // Multi-line list follows
        in_profile_sequence = true;
      } else {
        // Single value or comma-separated list
        auto parsed = splitProfiles(value);
        for (auto &item : parsed) {
          if (!unique_profiles.insert(item).second) {
            throw std::runtime_error("profile must contain unique selectors");
          }
          profile_list.push_back(std::move(item));
        }
      }
    } else if (key == "interface") {
      if (!interface.empty()) {
        throw std::runtime_error("config only accepts one interface field");
      }
      interface = value;
    } else {
      throw std::runtime_error(
          "config only accepts auto, profile and interface fields");
    }
  }

  if (auto_mode.empty()) {
    throw std::runtime_error("auto is required (enable or disable)");
  }
  if (profile_list.empty()) {
    throw std::runtime_error("profile is required");
  }
  if (interface.empty()) {
    throw std::runtime_error("interface is required");
  }

  const bool auto_detect = auto_mode == "enable";
  if (!auto_detect) {
    if (profile_list.size() != 1) {
      throw std::runtime_error(
          "profile must contain exactly one UUID when auto is disabled");
    }
  }

  const std::regex uuid(
      "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  for (const auto &selector : profile_list) {
    if (!std::regex_match(selector, uuid)) {
      throw std::runtime_error(
          "profile must use canonical BMS UUIDs: " + selector);
    }
    (void)loadModel(selector);
  }

  auto config = loadModel(profile_list.front());
  config.auto_detect = auto_detect;
  config.auto_profiles = profile_list;
  if (auto_detect) {
    config.model = "auto";
    config.model_id = "auto";
    config.bms_model = "BMS auto-detection";
  }

  if (!std::regex_match(interface,
                        std::regex("^[A-Za-z0-9_-]{1,64}$"))) {
    throw std::runtime_error("interface is invalid");
  }
  config.can.interface = interface;
  config.interface_override = true;
  return config;
}

std::string defaultConfig() {
  std::string config =
      "# 1. 自动探测模式: enable (开启) 或 disable (关闭)\n"
      "auto: enable\n\n"
      "# 2. BMS 协议 UUID:\n"
      "#    - auto 模式开启时: 支持 1 个或多个 UUID，删除不用的配置即可\n"
      "#    - auto 模式关闭时: 严格只允许有且仅有 1 个 UUID\n"
      "profile:\n";
  for (const auto &model : supportedModels()) {
    config += "  - " + model.id + " # " + model.profile + ": " +
              model.bms_model + "\n";
  }
  config += "\n# 3. SocketCAN 接口名称 (必填项)\n"
            "interface: can5\n";
  return config;
}

}  // namespace batcan
