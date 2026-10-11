#pragma once

#include "common/log_manager.hpp"
#include "defination/music_state.hpp"
#include "speech_input_node/speech_input_config.hpp"

#include <c-api.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <vosk_api.h>

namespace xiaocai_robot {

struct SpeechInputCallbacks {
  std::function<void(const std::string &)> publish_text;
  std::function<void(const std::string &)> publish_audio_play;
  std::function<void(bool)> publish_awake;
};

struct SpeechRecognitionResult {
  std::string text;
  bool was_armed{false};
  std::uint64_t generation{0};
};

class SpeechInputController {
public:
  SpeechInputController(SpeechInputSettings settings, LogManager &log_manager,
                        SpeechInputCallbacks callbacks);
  ~SpeechInputController();

  SpeechInputController(const SpeechInputController &) = delete;
  SpeechInputController &operator=(const SpeechInputController &) = delete;

  void HandleAudioData(const std::vector<std::uint8_t> &audio_data);
  void HandleAudioPlaying(bool playing);
  void HandleMusicState(common::MusicState state);
  void DrainResults();

  std::uint64_t AcknowledgeWake();
  void HandleRecognitionResult(const SpeechRecognitionResult &result);

private:
  struct Segment {
    std::vector<float> samples;
    bool was_armed{false};
    std::uint64_t generation{0};
  };

  void CollectSegments();
  void DecodeLoop();
  void HandleText(const std::string &heard, bool was_armed);
  void ResetCapture();
  void EnterStandby();
  void AcceptRequest(const std::string &text);
  void ReturnToWakeOnly();

  SpeechInputSettings settings_;
  LogManager &log_manager_;
  SpeechInputCallbacks callbacks_;
  VoskModel *wake_model_;
  VoskRecognizer *wake_recognizer_;
  const SherpaOnnxOfflineRecognizer *asr_;
  const SherpaOnnxVoiceActivityDetector *vad_;
  std::vector<float> vad_pending_;
  std::deque<float> recent_samples_;
  std::int64_t samples_seen_;
  std::int64_t history_begin_;
  std::deque<Segment> segments_;
  std::deque<SpeechRecognitionResult> results_;
  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::thread decoder_;
  bool stop_;
  bool playing_;
  bool music_playing_;
  bool wake_candidate_;
  bool request_in_flight_;
  std::uint64_t recognition_generation_;
  float input_gain_;
  double inactivity_timeout_sec_;
  double ignore_until_;
  double armed_until_;
  double last_wake_time_;
};

} // namespace xiaocai_robot
