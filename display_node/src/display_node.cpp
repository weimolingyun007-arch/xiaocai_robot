#include "display_node/display_node.hpp"

#include <chrono>
#include <cstdint>

namespace xiaocai_robot {

DisplayNode::DisplayNode()
    : Node("display_node"), display_config_(),
      log_manager_("display_node", display_config_.Logging()),
      display_window_(
          std::make_unique<DisplayWindow>(display_config_.AssetDirectory())) {
  ShowRobotState(common::RobotState::Idle);
  InitSubscribers();
  InitTimers();
}

void DisplayNode::InitSubscribers() {
  int initialized_count = 0;
  robot_state_subscriber_ = create_subscription<xiaocai_msgs::msg::RobotState>(
      "/xiaocai_robot/state", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::RobotState::SharedPtr message) {
        OnRobotStateCallback(*message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void DisplayNode::OnRobotStateCallback(
    const xiaocai_msgs::msg::RobotState &message) {
  if (message.state > static_cast<std::uint8_t>(common::RobotState::Muted)) {
    LOG_WARN("Ignoring unknown robot state value {}", message.state);
    return;
  }
  ShowRobotState(static_cast<common::RobotState>(message.state));
}

void DisplayNode::ShowRobotState(common::RobotState state) {
  const char *image_name = nullptr;
  switch (state) {
  case common::RobotState::Idle:
    image_name = "idle";
    break;
  case common::RobotState::Listening:
    image_name = "listening";
    break;
  case common::RobotState::Thinking:
    image_name = "thinking";
    break;
  case common::RobotState::Speaking:
    image_name = "speaking";
    break;
  case common::RobotState::Quiet:
    image_name = "quiet";
    break;
  case common::RobotState::Muted:
    image_name = "muted";
    break;
  }
  if (image_name)
    display_window_->ShowImage(image_name);
}

void DisplayNode::ProcessDisplayEvents() { display_window_->ProcessEvents(); }

void DisplayNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::milliseconds(30),
                             [this] { ProcessDisplayEvents(); });
}

} // namespace xiaocai_robot
