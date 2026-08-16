#pragma once

#include <string>
#include <vector>

#include "batcan/config.hpp"

namespace batcan {

struct ModelInfo {
  std::string id;
  std::string bms_model;
};

Config loadModel(const std::string &model);
std::vector<ModelInfo> supportedModels();

}  // namespace batcan
