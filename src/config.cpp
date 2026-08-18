#include "batcan/config.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>

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

}  // namespace

Config loadConfig(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot read config " + path);
  }
  std::string profile;
  std::string interface;
  std::string bitrate;
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
                               " must use profile: NAME");
    }
    const auto key = trim(line.substr(0, separator));
    auto value = trim(line.substr(separator + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (key == "model" || key == "profile") {
      if (!profile.empty()) {
        throw std::runtime_error("config only accepts one profile field");
      }
      profile = value;
    } else if (key == "interface") {
      if (!interface.empty()) {
        throw std::runtime_error("config only accepts one interface field");
      }
      interface = value;
    } else if (key == "bitrate") {
      if (!bitrate.empty()) {
        throw std::runtime_error("config only accepts one bitrate field");
      }
      bitrate = value;
    } else {
      throw std::runtime_error(
          "config accepts profile, interface and bitrate fields");
    }
  }
  if (profile.empty()) {
    throw std::runtime_error("profile is required");
  }
  auto config = loadModel(profile);
  if (!interface.empty()) {
    if (!std::regex_match(interface,
                          std::regex("^[A-Za-z0-9_-]{1,64}$"))) {
      throw std::runtime_error("interface is invalid");
    }
    config.can.interface = interface;
  }
  if (!bitrate.empty()) {
    std::size_t length = 0;
    unsigned long value = 0;
    try {
      value = std::stoul(bitrate, &length, 10);
    } catch (const std::exception &) {
      throw std::runtime_error("bitrate must be a positive integer");
    }
    if (length != bitrate.size() || value == 0 || value > 10000000UL) {
      throw std::runtime_error("bitrate must be a positive integer");
    }
    config.can.bitrate = static_cast<int>(value);
  }
  return config;
}

std::string defaultConfig() {
  std::string config =
      "# Select one BMS profile and optionally override the CAN interface.\n";
  for (const auto &model : supportedModels()) {
    config += "# profile: " + model.id + "\n";
  }
  config += "# interface: can5\n";
  config += "# bitrate: 250000\n";
  return config;
}

}  // namespace batcan
