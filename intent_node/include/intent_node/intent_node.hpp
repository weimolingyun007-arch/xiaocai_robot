#pragma once

#include "common/log_manager.hpp"
#include "intent_node/intent_config.hpp"
#include "intent_node/intent_rules.hpp"
#include "xiaocai_msgs/msg/intent.hpp"
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/music_command.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <cstdint>
#include <string>

namespace xiaocai_robot {

class IntentNode : public rclcpp::Node {
public:
  IntentNode();

private:
  void InitPublishers();
  void InitSubscribers();
  void OnTextCallback(std_msgs::msg::String::SharedPtr message);

  IntentConfig intent_config_;
  LogManager log_manager_;
  IntentRules intent_rules_;
  std::uint64_t next_memory_request_id_ = 0;
  rclcpp::Publisher<xiaocai_msgs::msg::Intent>::SharedPtr intent_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicCommand>::SharedPtr
      music_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      volume_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryCommand>::SharedPtr
      memory_command_publisher_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr text_subscriber_;
};

} // namespace xiaocai_robot
