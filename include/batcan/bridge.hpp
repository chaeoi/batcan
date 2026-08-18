#pragma once

#include <rclcpp/rclcpp.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>

#include "batcan/config.hpp"
#include "batcan/protocol.hpp"

namespace batcan {

class BatteryBridge final : public rclcpp::Node {
 public:
  explicit BatteryBridge(Config config);
  ~BatteryBridge() override;

 private:
  bool openCanSocket();
  void closeCanSocket();
  void collectAndPublish();
  void collectQuery(const QueryConfig &query, BatterySample &sample);
  void publish(const BatterySample &sample);

  Config config_;
  BatterySample sample_;
  int can_fd_ = -1;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr
      publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
};

}  // namespace batcan
