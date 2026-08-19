#include "batcan/bridge.hpp"

#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <diagnostic_msgs/msg/key_value.hpp>
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
#include <iomanip>
#include <sstream>
#include <set>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "batcan/models.hpp"

namespace batcan {
namespace {

std::uint32_t wireId(std::uint32_t id, bool extended) {
  return id | (extended ? CAN_EFF_FLAG : 0U);
}

std::uint32_t idMask(const ResponseConfig &response) {
  const auto mask = response.id_mask == 0
                        ? (response.extended ? CAN_EFF_MASK : CAN_SFF_MASK)
                        : response.id_mask;
  return mask | CAN_EFF_FLAG | CAN_RTR_FLAG;
}

void addValue(diagnostic_msgs::msg::DiagnosticStatus &status,
              const std::string &key, double value) {
  diagnostic_msgs::msg::KeyValue item;
  item.key = key;
  std::ostringstream stream;
  stream << std::setprecision(12) << value;
  item.value = stream.str();
  status.values.push_back(std::move(item));
}

void addText(diagnostic_msgs::msg::DiagnosticStatus &status,
             const std::string &key, const std::string &value) {
  diagnostic_msgs::msg::KeyValue item;
  item.key = key;
  item.value = value;
  status.values.push_back(std::move(item));
}

void addOptionalValue(diagnostic_msgs::msg::DiagnosticStatus &status,
                      const std::string &key,
                      const std::optional<double> &value) {
  if (value.has_value()) {
    addValue(status, key, *value);
  }
}

std::string bytesToHex(const std::vector<std::uint8_t> &bytes) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0) {
      stream << ' ';
    }
    stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return stream.str();
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
    : Node("batcan"), config_(std::move(config)), auto_config_(config_) {
  if (config_.auto_detect) {
    candidates_.reserve(config_.auto_profiles.size());
    for (const auto &selector : config_.auto_profiles) {
      auto candidate = loadModel(selector);
      if (config_.interface_override) {
        candidate.can.interface = config_.can.interface;
      }
      if (config_.bitrate_override) {
        candidate.can.bitrate = config_.can.bitrate;
      }
      candidate.ros = config_.ros;
      candidates_.push_back(std::move(candidate));
    }
    if (candidates_.empty()) {
      throw std::runtime_error("automatic BMS detection has no candidates");
    }
    config_.can.interface = candidates_.front().can.interface;
    config_.can.bitrate = candidates_.front().can.bitrate;
  }
  publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
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
  sample_.clear();
  if (auto_config_.auto_detect &&
      active_candidate_ == static_cast<std::size_t>(-1)) {
    (void)detectProfile(sample_);
    publish(sample_);
    return;
  }
  if (openCanSocket()) {
    for (const auto &query : config_.can.queries) {
      collectQuery(query, sample_);
      if (can_fd_ < 0) {
        break;
      }
    }
  }
  if (auto_config_.auto_detect) {
    if (sample_.present) {
      missed_cycles_ = 0;
    } else if (++missed_cycles_ >= 3U) {
      RCLCPP_WARN(get_logger(),
                  "lost BMS profile %s; returning to automatic detection",
                  config_.model.c_str());
      closeCanSocket();
      active_candidate_ = static_cast<std::size_t>(-1);
      config_ = auto_config_;
      missed_cycles_ = 0;
    }
  }
  publish(sample_);
}

bool BatteryBridge::detectProfile(BatterySample &sample) {
  closeCanSocket();
  std::size_t match = static_cast<std::size_t>(-1);
  BatterySample matched_sample;
  for (std::size_t index = 0; index < candidates_.size(); ++index) {
    config_ = candidates_[index];
    BatterySample probe;
    if (!openCanSocket()) {
      continue;
    }
    // Stop at the first valid protocol response; this also handles profiles
    // whose first query is quiet while a later query is actively answered.
    for (const auto &query : config_.can.queries) {
      collectQuery(query, probe);
      if (can_fd_ < 0) {
        break;
      }
      if (probe.present) {
        break;
      }
    }
    if (!probe.present) {
      closeCanSocket();
      continue;
    }
    if (match != static_cast<std::size_t>(-1)) {
      RCLCPP_ERROR(get_logger(),
                   "automatic BMS detection is ambiguous between %s and %s",
                   candidates_[match].model.c_str(),
                   candidates_[index].model.c_str());
      closeCanSocket();
      config_ = auto_config_;
      return false;
    }
    match = index;
    matched_sample = std::move(probe);
    closeCanSocket();
  }
  if (match != static_cast<std::size_t>(-1)) {
    active_candidate_ = match;
    config_ = candidates_[match];
    missed_cycles_ = 0;
    sample = std::move(matched_sample);
    RCLCPP_INFO(get_logger(), "detected BMS profile %s (%s)",
                config_.model.c_str(), config_.bms_model.c_str());
    return true;
  }
  closeCanSocket();
  config_ = auto_config_;
  return false;
}

void BatteryBridge::collectQuery(const QueryConfig &query,
                                 BatterySample &sample) {
  if (query.send_request) {
    can_frame request{};
    request.can_id = wireId(query.request_id, query.extended);
    if (query.request_remote) {
      request.can_id |= CAN_RTR_FLAG;
    } else {
      request.can_dlc = static_cast<__u8>(query.request_data.size());
      std::copy(query.request_data.begin(), query.request_data.end(),
                request.data);
    }
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
        const auto data_length = std::min<std::size_t>(frame.can_dlc, 8);
        if (!validateResponse(response, data, data_length)) {
          RCLCPP_WARN(get_logger(), "discarded response 0x%X with invalid CRC",
                      response.id);
          break;
        }
        applyResponse(response, data, data_length, sample);
        if (!response.collect) {
          pending.erase(index);
        }
      } catch (const std::exception &error) {
        RCLCPP_WARN(get_logger(), "decode response 0x%X failed: %s",
                    response.id, error.what());
      }
      break;
    }
  }
}

void BatteryBridge::publish(const BatterySample &sample) {
  diagnostic_msgs::msg::DiagnosticArray details;
  details.header.stamp = now();
  details.header.frame_id = config_.ros.frame_id;
  diagnostic_msgs::msg::DiagnosticStatus status;
  status.level = sample.present ? diagnostic_msgs::msg::DiagnosticStatus::OK
                                : diagnostic_msgs::msg::DiagnosticStatus::WARN;
  status.name = "batcan/" + config_.model + "/summary";
  status.hardware_id = config_.bms_model;
  status.message = sample.present ? "BMS data received" : "No BMS data received";
  addText(status, "profile", config_.model);
  addText(status, "profile_id", config_.model_id);
  addText(status, "profile_mode", auto_config_.auto_detect ? "auto" : "manual");
  addOptionalValue(status, "voltage", sample.voltage);
  addOptionalValue(status, "current", sample.current);
  addOptionalValue(status, "temperature", sample.temperature);
  addOptionalValue(status, "percentage", sample.percentage);
  addOptionalValue(status, "charge", sample.charge);
  addOptionalValue(status, "capacity", sample.capacity);
  addOptionalValue(status, "design_capacity", sample.design_capacity);
  if (sample.power_supply_status.has_value()) {
    addValue(status, "power_supply_status", *sample.power_supply_status);
  }
  if (sample.power_supply_health.has_value()) {
    addValue(status, "power_supply_health", *sample.power_supply_health);
  }
  if (sample.power_supply_technology.has_value()) {
    addValue(status, "power_supply_technology",
             *sample.power_supply_technology);
  }
  details.status.push_back(std::move(status));
  std::set<std::string> response_names;
  for (const auto &[response_name, metrics] : sample.response_metrics) {
    (void)metrics;
    response_names.insert(response_name);
  }
  for (const auto &[response_name, raw_frames] : sample.response_raw_frames) {
    (void)raw_frames;
    response_names.insert(response_name);
  }
  for (const auto &response_name : response_names) {
    diagnostic_msgs::msg::DiagnosticStatus response_status;
    response_status.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
    response_status.name = "batcan/" + config_.model + "/" + response_name;
    response_status.hardware_id = config_.bms_model;
    response_status.message = "Decoded response " + response_name;
    const auto metrics = sample.response_metrics.find(response_name);
    if (metrics != sample.response_metrics.end()) {
      for (const auto &[name, value] : metrics->second) {
        addValue(response_status, name, value);
      }
    }
    const auto raw = sample.response_raw_frames.find(response_name);
    if (raw != sample.response_raw_frames.end()) {
      for (const auto &[name, data] : raw->second) {
        diagnostic_msgs::msg::KeyValue item;
        item.key = "raw." + name;
        item.value = bytesToHex(data);
        response_status.values.push_back(std::move(item));
      }
    }
    details.status.push_back(std::move(response_status));
  }
  publisher_->publish(details);
}

}  // namespace batcan
