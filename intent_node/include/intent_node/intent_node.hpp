#pragma once

#include "common/log_manager.hpp"
#include "intent_node/intent_classifier.hpp"
#include "intent_node/intent_config.hpp"
#include "intent_node/intent_feedback_store.hpp"
#include "xiaocai_msgs/msg/conversation_turn.hpp"
#include "xiaocai_msgs/msg/intent.hpp"
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/music_command.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include "xiaocai_msgs/srv/submit_intent_correction.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace xiaocai_robot {

class IntentNode : public rclcpp::Node {
public:
  IntentNode();

private:
  // ROS communication initialization.
  void InitPublishers();
  void InitSubscribers();
  void InitServers();

  // ROS subscription and service callbacks.
  void OnTextCallback(std_msgs::msg::String::SharedPtr message);
  void OnSubmitIntentCorrectionCallback(
      const std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Request>
          request,
      std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Response>
          response);

  IntentConfig intent_config_;
  LogManager log_manager_;
  IntentClassifier intent_classifier_;
  IntentFeedbackStore intent_feedback_store_;
  std::uint64_t next_memory_request_id_;
  bool feedback_store_ready_;
  std::string last_intent_text_;
  std::string last_predicted_label_;
  double last_predicted_score_;
  std::chrono::steady_clock::time_point last_intent_at_;
  rclcpp::Publisher<xiaocai_msgs::msg::Intent>::SharedPtr intent_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicCommand>::SharedPtr
      music_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      volume_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryCommand>::SharedPtr
      memory_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::ConversationTurn>::SharedPtr
      turn_publisher_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr text_subscriber_;
  rclcpp::Service<xiaocai_msgs::srv::SubmitIntentCorrection>::SharedPtr
      intent_correction_server_;
};

} // namespace xiaocai_robot
