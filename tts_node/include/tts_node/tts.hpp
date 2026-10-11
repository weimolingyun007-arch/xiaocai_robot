#pragma once
#include "xiaocai_cpp/common.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include <c-api.h>
#include <rclcpp/rclcpp.hpp>
#include <filesystem>

class Tts:public rclcpp::Node {
public:
  Tts();
  ~Tts()override;
private:
  void synthesize(std::string text);
  const SherpaOnnxOfflineTts *tts_=nullptr;std::string model_,lexicon_,tokens_,fsts_;int speaker_=2,id_=0;
  rclcpp::Publisher<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_;
};
