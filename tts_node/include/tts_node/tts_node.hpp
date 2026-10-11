#pragma once
#include "common/common.hpp"
#include "common/log_manager.hpp"
#include "tts_node/tts_config.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include <c-api.h>
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

  // TTS subscription callback.
  void OnTextCallback(const std_msgs::msg::String::SharedPtr message);
  TtsConfig tts_config_;
  LogManager log_manager_;
  const SherpaOnnxOfflineTts *tts_;
  std::string model_, lexicon_, tokens_, fsts_;
  int speaker_, id_;
  rclcpp::Publisher<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_publisher_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr text_subscriber_;
};

} // namespace xiaocai_robot
