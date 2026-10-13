#pragma once

#include "common/log_manager.hpp"
#include "defination/robot_state.hpp"
#include "display_node/display_config.hpp"
#include "display_node/display_window.hpp"
#include "xiaocai_msgs/msg/robot_state.hpp"

#include <rclcpp/rclcpp.hpp>

#include <memory>

namespace xiaocai_robot {

class DisplayNode : public rclcpp::Node {
public:
  DisplayNode();

private:
  // ROS subscription initialization.
  void InitSubscribers();

  // ROS subscription callbacks.
  void OnRobotStateCallback(const xiaocai_msgs::msg::RobotState &message);

  // Display state and X11 event handling.
  void ShowRobotState(common::RobotState state);
  void ProcessDisplayEvents();

  // Timer initialization.
  void InitTimers();

  DisplayConfig display_config_;
  LogManager log_manager_;
  std::unique_ptr<DisplayWindow> display_window_;
  rclcpp::Subscription<xiaocai_msgs::msg::RobotState>::SharedPtr
      robot_state_subscriber_;
  rclcpp::TimerBase::SharedPtr timer_;
};

} // namespace xiaocai_robot
