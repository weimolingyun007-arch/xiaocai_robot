#pragma once

#include "common/log_manager.hpp"
#include "common/node_readiness_publisher.hpp"
#include "intent_node/intent_classifier.hpp"
#include "intent_node/intent_config.hpp"
#include "intent_node/intent_feedback_store.hpp"
#include "xiaocai_msgs/msg/dialogue_command.hpp"
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/music_command.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include "xiaocai_msgs/srv/get_volume.hpp"
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
  void InitClients();
  void InitServers();
  void RequestVolumeQuery();

  // ROS subscription and service callbacks.
  void OnTextCallback(std_msgs::msg::String::SharedPtr message);
  void OnSubmitIntentCorrectionCallback(
      const std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Request>
          request,
      std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Response>
          response);

  IntentConfig intent_config_;
  LogManager log_manager_;
  common::NodeReadinessPublisher readiness_publisher_;
  IntentClassifier intent_classifier_;
  IntentFeedbackStore intent_feedback_store_;
  std::uint64_t next_memory_request_id_;
  bool feedback_store_ready_;
  std::string last_intent_text_;
  std::string last_predicted_label_;
  double last_predicted_score_;
  std::chrono::steady_clock::time_point last_intent_at_;
  rclcpp::Publisher<xiaocai_msgs::msg::DialogueCommand>::SharedPtr
      dialogue_command_publisher_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr tts_text_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicCommand>::SharedPtr
      music_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      volume_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryCommand>::SharedPtr
      memory_command_publisher_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr text_subscriber_;
  rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedPtr volume_get_client_;
  rclcpp::TimerBase::SharedPtr volume_query_timeout_timer_;
  bool volume_query_pending_{false};
  std::uint64_t volume_query_generation_{0};
  rclcpp::Service<xiaocai_msgs::srv::SubmitIntentCorrection>::SharedPtr
      intent_correction_server_;
};

} // namespace xiaocai_robot
