#include "batcan/bridge.hpp"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace batcan {
namespace {

std::uint32_t wireId(std::uint32_t id, bool extended) {
  return id | (extended ? CAN_EFF_FLAG : 0U);
}

std::uint32_t idMask(const ResponseConfig &response) {
  const auto mask = response.id_mask == 0
                        ? (response.extended ? CAN_EFF_MASK : CAN_SFF_MASK)
                        : response.id_mask;
  return mask | (response.extended ? CAN_EFF_FLAG : 0U);
}

double valueOrNaN(const std::optional<double> &value) {
  return value.value_or(std::numeric_limits<double>::quiet_NaN());
}

int runCommand(const std::vector<std::string> &arguments) {
  const auto child = ::fork();
  if (child < 0) {
    return -1;
  }
  if (child == 0) {
    std::vector<char *> values;
    values.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) {
      values.push_back(const_cast<char *>(argument.c_str()));
    }
    values.push_back(nullptr);
    ::execvp(values[0], values.data());
    _exit(127);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

bool configureCanInterface(const CanConfig &can) {
  (void)runCommand({"ip", "link", "set", can.interface, "down"});
  if (runCommand({"ip", "link", "set", can.interface, "type", "can",
                  "bitrate", std::to_string(can.bitrate)}) != 0) {
    return false;
  }
  return runCommand({"ip", "link", "set", can.interface, "up"}) == 0;
}

}  // namespace

BatteryBridge::BatteryBridge(Config config)
    : Node("batcan"), config_(std::move(config)) {
  publisher_ = create_publisher<sensor_msgs::msg::BatteryState>(
      config_.ros.topic, rclcpp::QoS(config_.ros.qos_depth));
  timer_ = create_wall_timer(
      std::chrono::milliseconds(config_.can.query_interval_ms),
      [this]() { collectAndPublish(); });
  collectAndPublish();
}

BatteryBridge::~BatteryBridge() { closeCanSocket(); }

bool BatteryBridge::openCanSocket() {
  if (can_fd_ >= 0) {
    return true;
  }
  if (!configureCanInterface(config_.can)) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                         "configure CAN interface %s at %d bit/s failed",
                         config_.can.interface.c_str(), config_.can.bitrate);
    return false;
  }

  can_fd_ = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
  if (can_fd_ < 0) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                         "open CAN socket failed: %s", std::strerror(errno));
    return false;
  }

  ifreq request{};
  std::strncpy(request.ifr_name, config_.can.interface.c_str(), IFNAMSIZ - 1);
  if (::ioctl(can_fd_, SIOCGIFINDEX, &request) < 0) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                         "CAN interface %s is unavailable: %s",
                         config_.can.interface.c_str(), std::strerror(errno));
    closeCanSocket();
    return false;
  }

  std::vector<can_filter> filters;
  for (const auto &query : config_.can.queries) {
    for (const auto &response : query.responses) {
      filters.push_back(
          can_filter{wireId(response.id, response.extended),
                     idMask(response)});
    }
  }
  if (::setsockopt(can_fd_, SOL_CAN_RAW, CAN_RAW_FILTER, filters.data(),
                   filters.size() * sizeof(can_filter)) < 0) {
    RCLCPP_WARN(get_logger(), "set CAN filters failed: %s",
                std::strerror(errno));
    closeCanSocket();
    return false;
  }

  sockaddr_can address{};
  address.can_family = AF_CAN;
  address.can_ifindex = request.ifr_ifindex;
  if (::bind(can_fd_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                         "bind %s failed: %s", config_.can.interface.c_str(),
                         std::strerror(errno));
    closeCanSocket();
    return false;
  }
  RCLCPP_INFO(get_logger(), "listening on CAN interface %s",
              config_.can.interface.c_str());
  return true;
}

void BatteryBridge::closeCanSocket() {
  if (can_fd_ >= 0) {
    ::close(can_fd_);
    can_fd_ = -1;
  }
}

void BatteryBridge::collectAndPublish() {
  if (openCanSocket()) {
    for (const auto &query : config_.can.queries) {
      collectQuery(query, sample_);
      if (can_fd_ < 0) {
        break;
      }
    }
  }
  publish(sample_);
}

void BatteryBridge::collectQuery(const QueryConfig &query,
                                 BatterySample &sample) {
  if (query.send_request) {
    can_frame request{};
    request.can_id = wireId(query.request_id, query.extended);
    request.can_dlc = static_cast<__u8>(query.request_data.size());
    std::copy(query.request_data.begin(), query.request_data.end(), request.data);
    if (::write(can_fd_, &request, sizeof(request)) != sizeof(request)) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                           "send CAN query %s failed: %s", query.name.c_str(),
                           std::strerror(errno));
      closeCanSocket();
      return;
    }
  }

  std::set<std::size_t> pending;
  for (std::size_t index = 0; index < query.responses.size(); ++index) {
    pending.insert(index);
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            config_.can.response_timeout_ms);
  while (!pending.empty()) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      break;
    }
    pollfd descriptor{can_fd_, POLLIN, 0};
    const auto poll_result =
        ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (poll_result == 0) {
      break;
    }
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      RCLCPP_WARN(get_logger(), "poll CAN failed: %s", std::strerror(errno));
      closeCanSocket();
      return;
    }

    can_frame frame{};
    if (::read(can_fd_, &frame, sizeof(frame)) != sizeof(frame)) {
      continue;
    }
    for (std::size_t index = 0; index < query.responses.size(); ++index) {
      if (pending.find(index) == pending.end()) {
        continue;
      }
      const auto &response = query.responses[index];
      const auto expected = wireId(response.id, response.extended);
      if ((frame.can_id & idMask(response)) !=
          (expected & idMask(response))) {
        continue;
      }
      std::array<std::uint8_t, 8> data{};
      std::copy_n(frame.data, std::min<std::size_t>(frame.can_dlc, 8),
                  data.begin());
      try {
        applyResponse(response, data, std::min<std::size_t>(frame.can_dlc, 8),
                      sample);
        pending.erase(index);
      } catch (const std::exception &error) {
        RCLCPP_WARN(get_logger(), "decode response 0x%X failed: %s",
                    response.id, error.what());
      }
      break;
    }
  }
}

void BatteryBridge::publish(const BatterySample &sample) {
  sensor_msgs::msg::BatteryState message;
  message.header.stamp = now();
  message.header.frame_id = config_.ros.frame_id;
  message.voltage = valueOrNaN(sample.voltage);
  message.current = valueOrNaN(sample.current);
  message.temperature = valueOrNaN(sample.temperature);
  message.percentage = valueOrNaN(sample.percentage);
  message.charge = valueOrNaN(sample.charge);
  message.capacity = valueOrNaN(sample.capacity);
  message.design_capacity = valueOrNaN(sample.design_capacity);
  message.power_supply_status = sample.power_supply_status.value_or(
      sensor_msgs::msg::BatteryState::POWER_SUPPLY_STATUS_UNKNOWN);
  message.power_supply_health = sample.power_supply_health.value_or(
      sensor_msgs::msg::BatteryState::POWER_SUPPLY_HEALTH_UNKNOWN);
  message.power_supply_technology = sample.power_supply_technology.value_or(
      sensor_msgs::msg::BatteryState::POWER_SUPPLY_TECHNOLOGY_UNKNOWN);
  message.present = sample.present;
  publisher_->publish(message);
}

}  // namespace batcan
