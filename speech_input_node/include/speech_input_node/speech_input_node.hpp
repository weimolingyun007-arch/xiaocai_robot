#pragma once
#include "common/log_manager.hpp"
#include "speech_input_node/speech_input_config.hpp"
#include <c-api.h>
#include <vosk_api.h>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <xiaocai_msgs/msg/audio_play.hpp>
#include <xiaocai_msgs/msg/music_state.hpp>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class SpeechInputNode : public rclcpp::Node {
public:
  SpeechInputNode();
  ~SpeechInputNode() override;
private:
  void InitPublishers();
  void InitSubscribers();
  void InitTimers();
#ifdef XIAOCAI_SPEECH_INPUT_TEST
  friend class SpeechInputTestPeer;
#endif
  struct Segment {
    std::vector<float> samples;
    bool was_armed;
    std::uint64_t generation;
  };
  struct Recognition {
    std::string text;
    bool was_armed;
    std::uint64_t generation;
  };
  void on_audio(const std_msgs::msg::UInt8MultiArray &m);
  void on_playing(bool playing);
  void collect_segments();
  void decode_loop();
  void drain_results();
  void handle_text(const std::string &heard, bool was_armed);
  void acknowledge_wake();
  void reset_capture();
  void enter_standby();
  void accept_request(const std::string &text);
  void return_to_wake_only();
  SpeechInputConfig speech_input_config_;
  xc::LogManager log_manager_;
  VoskModel *wake_model_=nullptr;
  VoskRecognizer *wake_recognizer_=nullptr;
  const SherpaOnnxOfflineRecognizer *asr_=nullptr;
  const SherpaOnnxVoiceActivityDetector *vad_=nullptr;
  std::vector<float> vad_pending_;
  std::deque<float> recent_samples_;
  int64_t samples_seen_=0,history_begin_=0;
  std::deque<Segment> segments_;
  std::deque<Recognition> results_;
  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::thread decoder_;
  bool stop_=false, playing_=false, music_playing_=false, wake_candidate_=false;
  bool request_in_flight_=false;
  std::uint64_t recognition_generation_=0;
  float input_gain_=6.f;
  double inactivity_timeout_sec_=120.0;
  double ignore_until_=0,armed_until_=0,last_wake_time_=0;
  std::string wake_ack_path_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr text_;
  rclcpp::Publisher<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr awake_;
  rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr mic_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr playing_sub_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicState>::SharedPtr music_sub_;
  rclcpp::TimerBase::SharedPtr result_timer_;
};
