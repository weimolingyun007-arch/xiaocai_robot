#pragma once

#include "common/log_manager.hpp"
#include "common/node_readiness_publisher.hpp"
#include "dialogue_node/dialogue_config.hpp"
#include "dialogue_node/dialogue_controller.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include "xiaocai_msgs/msg/conversation_state.hpp"
#include "xiaocai_msgs/msg/conversation_turn.hpp"
#include "xiaocai_msgs/msg/intent.hpp"
#include "xiaocai_msgs/msg/memory_result.hpp"
#include "xiaocai_msgs/msg/music_state.hpp"
#include "xiaocai_msgs/msg/reminder.hpp"
#include "xiaocai_msgs/msg/robot_state.hpp"
#include "xiaocai_msgs/msg/tts_request.hpp"
#include "xiaocai_msgs/msg/tts_result.hpp"
#include "xiaocai_msgs/srv/get_volume.hpp"

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/string.hpp>
#include <xiaocai_msgs/msg/memory_command.hpp>
#include <xiaocai_msgs/msg/music_command.hpp>
#include <xiaocai_msgs/msg/volume_command.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace xiaocai_robot {

class DialogueNode : public rclcpp::Node {
public:
  DialogueNode();
  ~DialogueNode() override;

private:
  // ROS communication and controller initialization.
  void InitPublishers();
  void InitSubscribers();
  void InitClients();
  void InitController();
  void InitTimers();

  // ROS message publishing.
  void PublishState(common::RobotState state);
  void PublishSpeech(const std::string &text, std::uint64_t request_id);
  void PublishConversation(common::ConversationPhase phase, bool expect_reply);
  void PublishMemoryCommand(const DialogueMemoryCommand &command);
  void PublishVolumeCommand(common::VolumeAction action, int percent);
  void PublishMusicCommand(common::MusicAction action, const std::string &query,
                           std::uint64_t request_id);

  // ROS service client requests.
  void ClientVolume(DialogueCallbacks::VolumeResponseCallback callback);

  // ROS subscription callbacks.
  void OnIntentCallback(const xiaocai_msgs::msg::Intent &message);
  void OnMemoryResultCallback(const xiaocai_msgs::msg::MemoryResult &message);
  void OnReminderDueCallback(const xiaocai_msgs::msg::Reminder &message);
  void OnAudioPlayingCallback(const std_msgs::msg::Bool &message);
  void OnSpeechAwakeCallback(const std_msgs::msg::Bool &message);
  void OnVolumeStateCallback(const std_msgs::msg::Int32 &message);
  void OnMusicStateCallback(const xiaocai_msgs::msg::MusicState &message);
  void OnAudioPlayCallback(const xiaocai_msgs::msg::AudioPlay &message);

  void OnConversationTurnCallback(
      const xiaocai_msgs::msg::ConversationTurn &message);
  void OnTtsResultCallback(const xiaocai_msgs::msg::TtsResult &message);

  DialogueConfig dialogue_config_;
  LogManager log_manager_;
  common::NodeReadinessPublisher readiness_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::TtsRequest>::SharedPtr tts_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::ConversationState>::SharedPtr
      conversation_publisher_;
  rclcpp::Subscription<xiaocai_msgs::msg::ConversationTurn>::SharedPtr
      turn_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::TtsResult>::SharedPtr
      tts_result_subscriber_;
  rclcpp::Publisher<xiaocai_msgs::msg::RobotState>::SharedPtr state_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryCommand>::SharedPtr
      memory_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      volume_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicCommand>::SharedPtr
      music_command_publisher_;
  rclcpp::Subscription<xiaocai_msgs::msg::Intent>::SharedPtr intent_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MemoryResult>::SharedPtr
      result_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::Reminder>::SharedPtr due_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr playing_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr awake_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr
      volume_state_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicState>::SharedPtr
      music_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlay>::SharedPtr
      interrupt_subscriber_;
  rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedPtr volume_get_client_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::unique_ptr<DialogueController> dialogue_controller_;
};

} // namespace xiaocai_robot
