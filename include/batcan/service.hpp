#pragma once

#include <string>
#include <vector>

namespace batcan {

int serviceCommand(const std::vector<std::string> &arguments,
                   const std::string &executable_path);

}  // namespace batcan
