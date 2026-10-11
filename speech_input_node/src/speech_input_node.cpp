#include "speech_input_node/speech_input_node.hpp"

#include "defination/music_state.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace xiaocai_robot {

SpeechInputNode::SpeechInputNode()
    : Node("speech_input_node"), speech_input_config_(),
      log_manager_("speech_input_node", speech_input_config_.Logging()),
      speech_input_controller_() {
  InitPublishers();
  InitSubscribers();
  InitController();
  InitTimers();
}

void SpeechInputNode::InitPublishers() {
  int initialized_count = 0;
  text_publisher_ =
      create_publisher<std_msgs::msg::String>("/xiaocai_robot/speech/text", 10);
  ++initialized_count;
  awake_publisher_ =
      create_publisher<std_msgs::msg::Bool>("/xiaocai_robot/speech/awake", 10);
  ++initialized_count;
  play_publisher_ = create_publisher<xiaocai_msgs::msg::AudioPlay>(
      "/xiaocai_robot/audio/play", 10);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void SpeechInputNode::InitSubscribers() {
  int initialized_count = 0;
  mic_subscriber_ = create_subscription<std_msgs::msg::UInt8MultiArray>(
      "/xiaocai_robot/audio/mic", 10,
      [this](std_msgs::msg::UInt8MultiArray::SharedPtr message) {
        OnAudioCallback(*message);
      });
  ++initialized_count;
  playing_subscriber_ = create_subscription<std_msgs::msg::Bool>(
      "/xiaocai_robot/audio/playing", 10,
      [this](std_msgs::msg::Bool::SharedPtr message) {
        OnAudioPlayingCallback(message->data);
      });
  ++initialized_count;
  music_subscriber_ = create_subscription<xiaocai_msgs::msg::MusicState>(
      "/xiaocai_robot/music/state", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::MusicState::SharedPtr message) {
        OnMusicStateCallback(*message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void SpeechInputNode::InitController() {
  auto settings = speech_input_config_.Settings();
  settings.wake_model_path = declare_parameter<std::string>(
      "wake_model_path", settings.wake_model_path);
  settings.sense_model_path = declare_parameter<std::string>(
      "sense_model_path", settings.sense_model_path);
  settings.sense_tokens_path = declare_parameter<std::string>(
      "sense_tokens_path", settings.sense_tokens_path);
  settings.vad_model_path =
      declare_parameter<std::string>("vad_model_path", settings.vad_model_path);
  settings.wake_ack_path =
      declare_parameter<std::string>("wake_ack_path", settings.wake_ack_path);
  settings.input_gain =
      declare_parameter<double>("input_gain", settings.input_gain);
  if (settings.input_gain <= 0 || settings.input_gain > 30)
    throw std::runtime_error("input_gain must be in (0, 30]");
  settings.inactivity_timeout_sec = declare_parameter<double>(
      "inactivity_timeout_sec", settings.inactivity_timeout_sec);
  if (settings.inactivity_timeout_sec < 1.0)
    throw std::runtime_error("inactivity_timeout_sec must be at least 1");

  SpeechInputCallbacks callbacks;
  callbacks.publish_text = [this](const std::string &text) {
    PublishText(text);
  };
  callbacks.publish_audio_play = [this](const std::string &path) {
    PublishAudioPlay(path);
  };
  callbacks.publish_awake = [this](bool awake) { PublishAwake(awake); };
  speech_input_controller_ = std::make_unique<SpeechInputController>(
      std::move(settings), log_manager_, std::move(callbacks));
}

void SpeechInputNode::InitTimers() {
  result_timer_ = create_wall_timer(std::chrono::milliseconds(50), [this] {
    speech_input_controller_->DrainResults();
  });
}

void SpeechInputNode::PublishText(const std::string &text) {
  std_msgs::msg::String message;
  message.data = text;
  text_publisher_->publish(message);
}

void SpeechInputNode::PublishAudioPlay(const std::string &path) {
  xiaocai_msgs::msg::AudioPlay message;
  message.timestamp = now();
  message.path = path;
  play_publisher_->publish(message);
}

void SpeechInputNode::PublishAwake(bool awake) {
  std_msgs::msg::Bool message;
  message.data = awake;
  awake_publisher_->publish(message);
}

void SpeechInputNode::OnAudioCallback(
    const std_msgs::msg::UInt8MultiArray &message) {
  speech_input_controller_->HandleAudioData(message.data);
}

void SpeechInputNode::OnAudioPlayingCallback(bool playing) {
  speech_input_controller_->HandleAudioPlaying(playing);
}

void SpeechInputNode::OnMusicStateCallback(
    const xiaocai_msgs::msg::MusicState &message) {
  speech_input_controller_->HandleMusicState(
      static_cast<common::MusicState>(message.state));
}

} // namespace xiaocai_robot
