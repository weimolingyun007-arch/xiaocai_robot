#pragma once

#include "common/log_manager.hpp"
#include "common/node_readiness_publisher.hpp"
#include "speech_input_node/speech_input_config.hpp"
#include "speech_input_node/speech_input_controller.hpp"

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <xiaocai_msgs/msg/audio_play.hpp>
#include <xiaocai_msgs/msg/conversation_state.hpp>
#include <xiaocai_msgs/msg/music_state.hpp>

#include <memory>
#include <string>

namespace xiaocai_robot {

class SpeechInputNode : public rclcpp::Node {
public:
  SpeechInputNode();

private:
  // ROS communication and controller initialization.
  void InitPublishers();
  void InitSubscribers();
  void InitController();
  void InitTimers();

  // ROS message publishing.
  void PublishText(const std::string &text);
  void PublishAudioPlay(const std::string &path);
  void PublishAwake(bool awake);

  // ROS subscription callbacks.
  void OnAudioCallback(const std_msgs::msg::UInt8MultiArray &message);
  void OnAudioPlayingCallback(bool playing);
  void OnMusicStateCallback(const xiaocai_msgs::msg::MusicState &message);

  void OnConversationStateCallback(
      const xiaocai_msgs::msg::ConversationState &message);

  SpeechInputConfig speech_input_config_;
  LogManager log_manager_;
  common::NodeReadinessPublisher readiness_publisher_;
  std::unique_ptr<SpeechInputController> speech_input_controller_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr text_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_publisher_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr awake_publisher_;
  rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr
      mic_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr playing_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicState>::SharedPtr
      music_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::ConversationState>::SharedPtr
      conversation_subscriber_;
  rclcpp::TimerBase::SharedPtr result_timer_;
  rclcpp::Time last_wake_at_;
};

} // namespace xiaocai_robot
