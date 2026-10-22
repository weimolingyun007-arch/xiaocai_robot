#pragma once
#include "common/common.hpp"
#include "common/log_manager.hpp"
#include "common/node_readiness_publisher.hpp"
#include "tts_node/tts_config.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include "xiaocai_msgs/msg/audio_playback_result.hpp"
#include "xiaocai_msgs/msg/tts_request.hpp"
#include "xiaocai_msgs/msg/tts_result.hpp"
#include <c-api.h>
#include <cstdint>
#include <map>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <string>

namespace xiaocai_robot {

class TtsNode : public rclcpp::Node {
public:
  TtsNode();
  ~TtsNode() override;

private:
  // ROS communication initialization.
  void InitPublishers();
  void InitSubscribers();

  // ROS message publishing.
  void PublishResult(std::uint64_t request_id, bool success);

  // TTS subscription callbacks.
  void OnTextCallback(const std_msgs::msg::String::SharedPtr message);
  void OnTtsRequestCallback(const xiaocai_msgs::msg::TtsRequest &message);
  void
  OnAudioResultCallback(const xiaocai_msgs::msg::AudioPlaybackResult &message);

  // Speech generation and playback tracking.
  void GenerateSpeech(const std::string &text, std::uint64_t request_id);

  TtsConfig tts_config_;
  LogManager log_manager_;
  common::NodeReadinessPublisher readiness_publisher_;
  const SherpaOnnxOfflineTts *tts_;
  std::string model_, lexicon_, tokens_, fsts_;
  int speaker_, id_;
  rclcpp::Publisher<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::TtsResult>::SharedPtr result_publisher_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr text_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::TtsRequest>::SharedPtr
      request_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlaybackResult>::SharedPtr
      result_subscriber_;
  std::map<std::string, std::uint64_t> pending_;
};

} // namespace xiaocai_robot
