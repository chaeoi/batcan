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
    if (item.empty() || !unique.insert(item).second) {
      throw std::runtime_error(
          "profiles must contain unique non-empty selectors");
    }
    profiles.push_back(std::move(item));
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
  std::string profile;
  std::string profiles;
  std::string interface;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim(removeComment(line));
    if (line.empty()) {
      continue;
    }
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
      throw std::runtime_error("config line " + std::to_string(line_number) +
                               " must use profile: UUID");
    }
    const auto key = trim(line.substr(0, separator));
    auto value = trim(line.substr(separator + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (key == "profile") {
      if (!profile.empty()) {
        throw std::runtime_error("config only accepts one profile field");
      }
      profile = value;
    } else if (key == "profiles") {
      if (!profiles.empty()) {
        throw std::runtime_error("config only accepts one profiles field");
      }
      profiles = value;
    } else if (key == "interface") {
      if (!interface.empty()) {
        throw std::runtime_error("config only accepts one interface field");
      }
      interface = value;
    } else {
      throw std::runtime_error(
          "config accepts profile, profiles and interface fields");
    }
  }
  if (profile.empty()) {
    throw std::runtime_error("profile is required");
  }
  const bool auto_detect = profile == "auto";
  if (!auto_detect && !profiles.empty()) {
    throw std::runtime_error("profiles is only valid with profile: auto");
  }
  std::vector<std::string> auto_profiles;
  if (auto_detect) {
    if (profiles.empty()) {
      throw std::runtime_error("profiles is required with profile: auto");
    }
    auto_profiles = splitProfiles(profiles);
    if (auto_profiles.empty()) {
      throw std::runtime_error("profiles must not be empty");
    }
    const std::regex uuid(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    for (const auto &selector : auto_profiles) {
      if (!std::regex_match(selector, uuid)) {
        throw std::runtime_error(
            "automatic profiles must use canonical BMS UUIDs");
      }
      (void)loadModel(selector);
    }
  }
  auto config = loadModel(auto_detect ? auto_profiles.front() : profile);
  config.auto_detect = auto_detect;
  config.auto_profiles = std::move(auto_profiles);
  if (auto_detect) {
    config.model = "auto";
    config.model_id = "auto";
    config.bms_model = "BMS auto-detection";
  }
  if (!interface.empty()) {
    if (!std::regex_match(interface,
                          std::regex("^[A-Za-z0-9_-]{1,64}$"))) {
      throw std::runtime_error("interface is invalid");
    }
    config.can.interface = interface;
    config.interface_override = true;
  }
  return config;
}

std::string defaultConfig() {
  std::string config =
      "# Select one BMS profile UUID and optionally override the CAN interface.\n";
  for (const auto &model : supportedModels()) {
    config += "# profile: " + model.id + " # " + model.profile + ": " +
              model.bms_model + ".\n";
  }
  config += "# profile: auto # Probe the candidate UUIDs below at startup.\n";
  config += "# profiles: UUID,UUID # Comma-separated candidate profile IDs.\n";
  config += "# interface: can5 # SocketCAN interface on this machine.\n";
  return config;
}

}  // namespace batcan
