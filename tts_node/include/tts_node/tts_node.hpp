#pragma once
#include "common/log_manager.hpp"
#include "tts_node/tts_config.hpp"
#include "common/common.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include <c-api.h>
#include <rclcpp/rclcpp.hpp>
#include <filesystem>

class TtsNode:public rclcpp::Node {
public:
  TtsNode();
  ~TtsNode()override;
private:
  void InitPublishers();
  void InitSubscribers();
  void OnTextCallback(const std_msgs::msg::String::SharedPtr message);
  void Synthesize(std::string text);
  TtsConfig tts_config_;
  xc::LogManager log_manager_;
  const SherpaOnnxOfflineTts *tts_=nullptr;std::string model_,lexicon_,tokens_,fsts_;int speaker_=2,id_=0;
  rclcpp::Publisher<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_;
};
