#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace batcan {

int serviceCommand(const std::vector<std::string> &arguments,
                   const std::string &executable_path);

void automaticUpdateLoop(std::atomic_bool &stopping,
                         std::condition_variable &wake,
                         std::mutex &wake_mutex);

}  // namespace batcan
