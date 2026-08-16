#include "batcan/config.hpp"

#include <fstream>
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
                               " must use model: NAME");
    }
    const auto key = trim(line.substr(0, separator));
    auto value = trim(line.substr(separator + 1));
    if (key != "model" || !model.empty()) {
      throw std::runtime_error("config only accepts one model field");
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    model = value;
  }
  if (model.empty()) {
    throw std::runtime_error("model is required");
  }
  return loadModel(model);
}

std::string defaultConfig() {
  std::string config =
      "# Uncomment exactly one model line.\n";
  for (const auto &model : supportedModels()) {
    config += "# model: " + model.id + "\n";
  }
  return config;
}

}  // namespace batcan
