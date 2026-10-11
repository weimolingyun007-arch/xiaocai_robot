#include "speech_input_node/speech_input_node.hpp"
#include "common/common.hpp"
#include "common/mpv.hpp"
#include "defination/music_state.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace {
size_t find_wakeword(const std::string &text) {
  static const std::array<std::string, 7> names = {
    "小蔡", "小菜", "小才", "小猜", "小采", "小彩", "小财"
  };
  size_t position = std::string::npos;
  for (const auto &name : names) position = std::min(position, text.find(name));
  return position <= 12 ? position : std::string::npos;
}

std::string compact_text(std::string text) {
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
  }), text.end());
  return xc::trim(text);
}

std::string strip_wakeword_tail(std::string rest) {
  static const std::array<std::string, 11> marks = {
    "，", "。", "！", "？", "、", ",", ".", "!", "?", ":", "："
  };
  auto trim_marks = [&] {
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &mark : marks) {
        if (rest.rfind(mark, 0) == 0) {
          rest.erase(0, mark.size());
          changed = true;
        }
        if (rest.size() >= mark.size() &&
            rest.compare(rest.size() - mark.size(), mark.size(), mark) == 0) {
          rest.erase(rest.size() - mark.size());
          changed = true;
        }
      }
    }
  };
  trim_marks();
  while (true) {
    if (rest.rfind("同学", 0) == 0) rest = rest.substr(6);
    else if (find_wakeword(rest) == 0) rest = rest.substr(6);
    else break;
    trim_marks();
  }
  return rest;
}

bool exact_wake_phrase(const std::string &text) {
  const auto compact = compact_text(text);
  return find_wakeword(compact) == 0 &&
    strip_wakeword_tail(compact.substr(6)).empty();
}

bool is_hush_command(const std::string &text) {
  const auto compact = compact_text(text);
  static const std::array<std::string, 6> commands = {
    "闭嘴", "别说了", "别说话", "不要说话", "停止说话", "别讲话"
  };
  return std::any_of(commands.begin(), commands.end(), [&compact](const auto &command) {
    return compact.find(command) != std::string::npos;
  });
}

size_t wake_alias_prefix_size(const std::string &text) {
  const auto known = find_wakeword(text);
  if (known == 0) return 6;
  if (text.size() >= 12 && text.rfind("小", 0) == 0 &&
      text.compare(6, 6, "同学") == 0) return 12;
  if (text.size() >= 12 && text.rfind("小", 0) == 0 &&
      text.compare(6, 3, "小") == 0 &&
      text.compare(0, 3, text, 6, 3) == 0) return 12;
  return 0;
}
}

SpeechInput::SpeechInput()
    : Node("speech_input_node"), speech_input_config_(),
      log_manager_("speech_input_node", speech_input_config_.logging()) {
  const auto &settings = speech_input_config_.settings();
  const auto wake_path = declare_parameter<std::string>(
    "wake_model_path", settings.wake_model_path);
  const auto model_path = declare_parameter<std::string>(
    "sense_model_path", settings.sense_model_path);
  const auto tokens_path = declare_parameter<std::string>(
    "sense_tokens_path", settings.sense_tokens_path);
  const auto vad_path = declare_parameter<std::string>(
    "vad_model_path", settings.vad_model_path);
  wake_ack_path_ = declare_parameter<std::string>("wake_ack_path", settings.wake_ack_path);
  wake_ack_ding_path_ = declare_parameter<std::string>(
    "wake_ack_ding_path", settings.wake_ack_ding_path);
  const double gain = declare_parameter<double>("input_gain", settings.input_gain);
  if (gain <= 0 || gain > 30) throw std::runtime_error("input_gain must be in (0, 30]");
  input_gain_ = static_cast<float>(gain);
  inactivity_timeout_sec_ = declare_parameter<double>("inactivity_timeout_sec", settings.inactivity_timeout_sec);
  if (inactivity_timeout_sec_ < 1.0)
    throw std::runtime_error("inactivity_timeout_sec must be at least 1");

  vosk_set_log_level(-1);
  wake_model_ = vosk_model_new(wake_path.c_str());
  if (!wake_model_) throw std::runtime_error("Cannot load wake model: " + wake_path);
  wake_recognizer_ = vosk_recognizer_new_grm(wake_model_, 16000,
    R"(["小蔡","小菜","小蔡 同学","小菜 同学","小蔡 小蔡","小菜 小菜","闭嘴","别说了","别说话","不要说话","停止说话","别讲话","[unk]"])");
  if (!wake_recognizer_) throw std::runtime_error("Cannot create wake recognizer");

  SherpaOnnxOfflineRecognizerConfig asr_config{};
  asr_config.feat_config.sample_rate = 16000;
  asr_config.feat_config.feature_dim = 80;
  asr_config.model_config.sense_voice.model = model_path.c_str();
  asr_config.model_config.sense_voice.language = "zh";
  asr_config.model_config.sense_voice.use_itn = 1;
  asr_config.model_config.tokens = tokens_path.c_str();
  asr_config.model_config.num_threads = 2;
  asr_config.model_config.provider = "cpu";
  asr_config.decoding_method = "greedy_search";
  asr_ = SherpaOnnxCreateOfflineRecognizer(&asr_config);
  if (!asr_) throw std::runtime_error("Cannot load SenseVoice model: " + model_path);

  SherpaOnnxVadModelConfig vad_config{};
  vad_config.silero_vad.model = vad_path.c_str();
  vad_config.silero_vad.threshold = 0.25f;
  vad_config.silero_vad.min_silence_duration = 0.5f;
  vad_config.silero_vad.min_speech_duration = 0.2f;
  vad_config.silero_vad.max_speech_duration = 12.f;
  vad_config.silero_vad.window_size = 512;
  vad_config.sample_rate = 16000;
  vad_config.num_threads = 1;
  vad_config.provider = "cpu";
  vad_ = SherpaOnnxCreateVoiceActivityDetector(&vad_config, 30.f);
  if (!vad_) throw std::runtime_error("Cannot load Silero VAD: " + vad_path);

  InitPublishers();
  InitSubscribers();
  InitTimers();
  decoder_ = std::thread([this] { decode_loop(); });
  log_manager_.logger()->info("Wake-only standby ready; SenseVoice runs after wake detection");
}

void SpeechInput::InitPublishers() {
  text_ = create_publisher<std_msgs::msg::String>("/xiaocai_robot/speech/text", 10);
  awake_ = create_publisher<std_msgs::msg::Bool>("/xiaocai_robot/speech/awake", 10);
  play_ = create_publisher<xiaocai_msgs::msg::AudioPlay>("/xiaocai_robot/audio/play", 10);
}

void SpeechInput::InitSubscribers() {
  mic_sub_ = create_subscription<std_msgs::msg::UInt8MultiArray>("/xiaocai_robot/audio/mic", 10,
    [this](std_msgs::msg::UInt8MultiArray::SharedPtr m) { on_audio(*m); });
  playing_sub_ = create_subscription<std_msgs::msg::Bool>("/xiaocai_robot/audio/playing", 10,
    [this](std_msgs::msg::Bool::SharedPtr m) { on_playing(m->data); });
  music_sub_ = create_subscription<xiaocai_msgs::msg::MusicState>("/xiaocai_robot/music/state",
    rclcpp::QoS(1).transient_local(), [this](xiaocai_msgs::msg::MusicState::SharedPtr m) {
      const bool playing = m->state == static_cast<std::uint8_t>(xiaocai_robot::common::MusicState::Playing);
      if (playing == music_playing_) return;
      music_playing_ = playing;
      on_playing(playing_);
      ++recognition_generation_;
      request_in_flight_ = false;
      std::lock_guard<std::mutex> lock(queue_mutex_);
      segments_.clear();
      results_.clear();
    });
}

void SpeechInput::InitTimers() {
  result_timer_ = create_wall_timer(std::chrono::milliseconds(50), [this] { drain_results(); });
}

SpeechInput::~SpeechInput() {
  { std::lock_guard<std::mutex> lock(queue_mutex_); stop_ = true; }
  queue_ready_.notify_all();
  if (decoder_.joinable()) decoder_.join();
  if (vad_) SherpaOnnxDestroyVoiceActivityDetector(vad_);
  if (asr_) SherpaOnnxDestroyOfflineRecognizer(asr_);
  if (wake_recognizer_) vosk_recognizer_free(wake_recognizer_);
  if (wake_model_) vosk_model_free(wake_model_);
}

void SpeechInput::on_playing(bool playing) {
  playing_ = playing;
  reset_capture();
  if (!playing) ignore_until_ = xc::monotonic() + 0.4;
}

void SpeechInput::reset_capture() {
  wake_candidate_ = false;
  vosk_recognizer_reset(wake_recognizer_);
  SherpaOnnxVoiceActivityDetectorReset(vad_);
  vad_pending_.clear();
  recent_samples_.clear();
  samples_seen_=history_begin_=0;
}

void SpeechInput::collect_segments() {
  while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
    const auto *segment = SherpaOnnxVoiceActivityDetectorFront(vad_);
    const bool armed = xc::monotonic() < armed_until_;
    if (segment && segment->n >= 3200 && (armed || wake_candidate_) && !request_in_flight_) {
      if (armed) armed_until_ = xc::monotonic() + inactivity_timeout_sec_;
      const int64_t prefix_start = std::max<int64_t>(
        history_begin_, static_cast<int64_t>(segment->start) - 16000);
      const size_t prefix_offset = static_cast<size_t>(prefix_start - history_begin_);
      const size_t prefix_count = static_cast<size_t>(
        static_cast<int64_t>(segment->start) - prefix_start);
      std::vector<float> audio;
      audio.reserve(prefix_count + segment->n);
      auto prefix = recent_samples_.begin();
      std::advance(prefix, prefix_offset);
      auto prefix_end = prefix;
      std::advance(prefix_end, prefix_count);
      audio.insert(audio.end(), prefix, prefix_end);
      audio.insert(audio.end(), segment->samples, segment->samples + segment->n);
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        segments_.push_back({std::move(audio), armed, recognition_generation_});
        request_in_flight_ = true;
      }
      queue_ready_.notify_one();
    }
    if (segment) SherpaOnnxDestroySpeechSegment(segment);
    SherpaOnnxVoiceActivityDetectorPop(vad_);
    wake_candidate_ = false;
  }
}

void SpeechInput::on_audio(const std_msgs::msg::UInt8MultiArray &m) {
  if (xc::monotonic() < ignore_until_ || m.data.size() < 2) return;
  if (playing_ || music_playing_) {
    if (vosk_recognizer_accept_waveform(wake_recognizer_,
        reinterpret_cast<const char *>(m.data.data()), m.data.size()) == 1) {
      const auto result = xc::parse(vosk_recognizer_result(wake_recognizer_));
      const auto phrase = !result.is_object() ? std::string{} :
        compact_text(result.value("text", std::string{}));
      if (is_hush_command(phrase)) {
        log_manager_.logger()->info("hush command detected during playback: {}", phrase);
        return_to_wake_only();
      } else if (music_playing_ && !playing_ && exact_wake_phrase(phrase) &&
                 xc::monotonic() - last_wake_time_ > 2.0) {
        acknowledge_wake();
      }
    }
    return;
  }
  const size_t count = m.data.size() / 2;
  float block_peak = 0.f;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t raw = static_cast<uint16_t>(m.data[i * 2]) |
      (static_cast<uint16_t>(m.data[i * 2 + 1]) << 8);
    const float sample = static_cast<int16_t>(raw) / 32768.f;
    block_peak = std::max(block_peak, std::abs(sample));
  }
  const float effective_gain = block_peak > 0.f
    ? std::min(input_gain_, 0.90f / block_peak) : input_gain_;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t raw = static_cast<uint16_t>(m.data[i * 2]) |
      (static_cast<uint16_t>(m.data[i * 2 + 1]) << 8);
    const int16_t pcm = static_cast<int16_t>(raw);
    vad_pending_.push_back(pcm * effective_gain / 32768.f);
  }
  size_t processed = 0;
  while (vad_pending_.size() - processed >= 512) {
    for (size_t i=0; i<512; ++i) {
      recent_samples_.push_back(vad_pending_[processed+i]);
      ++samples_seen_;
    }
    while (recent_samples_.size() > 16000 * 13) {
      recent_samples_.pop_front();
      ++history_begin_;
    }
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, vad_pending_.data() + processed, 512);
    processed += 512;
  }
  vad_pending_.erase(vad_pending_.begin(), vad_pending_.begin() + processed);
  if (armed_until_ > 0) {
    const double now = xc::monotonic();
    if (SherpaOnnxVoiceActivityDetectorDetected(vad_)) {
      armed_until_ = now + inactivity_timeout_sec_;
    } else if (now >= armed_until_) {
      enter_standby();
      log_manager_.logger()->info("No request for {:.0f} seconds; wake-only standby",
        inactivity_timeout_sec_);
    }
  }
  if (vosk_recognizer_accept_waveform(wake_recognizer_,
      reinterpret_cast<const char *>(m.data.data()), m.data.size()) == 1) {
    const auto result = xc::parse(vosk_recognizer_result(wake_recognizer_));
    const auto phrase = !result.is_object() ? std::string{} :
      compact_text(result.value("text", std::string{}));
    if (is_hush_command(phrase) && armed_until_ > xc::monotonic()) {
      log_manager_.logger()->info("hush command detected: {}", phrase);
      return_to_wake_only();
      return;
    }
    if (exact_wake_phrase(phrase) && xc::monotonic() - last_wake_time_ > 2.0) {
      log_manager_.logger()->info("wake phrase detected by grammar: {}", phrase);
      SherpaOnnxVoiceActivityDetectorReset(vad_);
      vad_pending_.clear();
      recent_samples_.clear();
      samples_seen_ = history_begin_ = 0;
      wake_candidate_ = false;
      acknowledge_wake();
      return;
    }
    if (find_wakeword(phrase) != std::string::npos) wake_candidate_ = true;
  } else {
    const auto partial = xc::parse(vosk_recognizer_partial_result(wake_recognizer_));
    if (partial.is_object() &&
        find_wakeword(compact_text(partial.value("partial", std::string{}))) != std::string::npos)
      wake_candidate_ = true;
  }
  collect_segments();
}

void SpeechInput::decode_loop() {
  while (true) {
    Segment segment;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_ready_.wait(lock, [this] { return stop_ || !segments_.empty(); });
      if (stop_) return;
      segment = std::move(segments_.front());
      segments_.pop_front();
    }
    const auto *stream = SherpaOnnxCreateOfflineStream(asr_);
    if (!stream) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      results_.push_back({{}, segment.was_armed, segment.generation});
      continue;
    }
    SherpaOnnxAcceptWaveformOffline(stream, 16000, segment.samples.data(),
      static_cast<int32_t>(segment.samples.size()));
    const double began = xc::monotonic();
    SherpaOnnxDecodeOfflineStream(asr_, stream);
    const auto *result = SherpaOnnxGetOfflineStreamResult(stream);
    const auto heard = result && result->text ? compact_text(result->text) : std::string{};
    log_manager_.logger()->info("SenseVoice heard: {} ({:.2f} s decode)",
      heard, xc::monotonic() - began);
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      results_.push_back({heard, segment.was_armed, segment.generation});
    }
    if (result) SherpaOnnxDestroyOfflineRecognizerResult(result);
    SherpaOnnxDestroyOfflineStream(stream);
  }
}

void SpeechInput::drain_results() {
  std::deque<Recognition> results;
  { std::lock_guard<std::mutex> lock(queue_mutex_); results.swap(results_); }
  for (const auto &result : results) {
    if (result.generation != recognition_generation_) continue;
    request_in_flight_ = false;
    if (!result.text.empty()) handle_text(result.text, result.was_armed);
  }
}

void SpeechInput::handle_text(const std::string &heard, bool was_armed) {
  if (music_playing_ || (was_armed && armed_until_ == 0)) return;
  if (is_hush_command(heard) &&
      (was_armed || find_wakeword(heard) != std::string::npos)) {
    return_to_wake_only();
    return;
  }
  const auto position = find_wakeword(heard);
  if (position != std::string::npos) {
    const auto rest = strip_wakeword_tail(heard.substr(position + 6));
    if (rest.empty()) {
      if (!was_armed && xc::monotonic() - last_wake_time_ > 2.0) acknowledge_wake();
    } else {
      const double now = xc::monotonic();
      if (now - last_wake_time_ > 2.0) {
        if (!was_armed) {
          acknowledge_wake();
        } else {
          std_msgs::msg::Bool awake;
          awake.data = true;
          awake_->publish(awake);
        }
      }
      last_wake_time_ = xc::monotonic();
      accept_request(rest);
    }
  } else if (was_armed) {
    std::string command = heard;
    if (xc::monotonic() - last_wake_time_ < 4.0) {
      const auto alias_size = wake_alias_prefix_size(heard);
      if (alias_size > 0) {
        command = strip_wakeword_tail(heard.substr(alias_size));
        if (command.empty()) {
          log_manager_.logger()->info("ignored duplicate ASR wake phrase: {}", heard);
          return;
        }
      }
    }
    accept_request(command);
  }
}

void SpeechInput::acknowledge_wake() {
  // A wake word explicitly ends the current music session.
  xiaocai_robot::common::mpv_command({"stop"});
  music_playing_ = false;
  armed_until_ = xc::monotonic() + inactivity_timeout_sec_;
  last_wake_time_ = xc::monotonic();
  ++recognition_generation_;
  request_in_flight_ = false;
  reset_capture();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    segments_.clear();
    results_.clear();
  }
  std_msgs::msg::Bool awake;
  awake.data = true;
  awake_->publish(awake);
  xiaocai_msgs::msg::AudioPlay play;
  play.path=wake_ack_path_;
  play_->publish(play);
  log_manager_.logger()->info("wakeword acknowledged");
}

void SpeechInput::acknowledge_input() {
  xiaocai_msgs::msg::AudioPlay play;
  play.path = wake_ack_ding_path_;
  play_->publish(play);
  log_manager_.logger()->info("accepted speech; acknowledgement beep queued");
}

void SpeechInput::enter_standby() {
  armed_until_ = 0;
  ++recognition_generation_;
  request_in_flight_ = false;
  reset_capture();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    segments_.clear();
    results_.clear();
  }
  std_msgs::msg::Bool awake;
  awake.data = false;
  awake_->publish(awake);
}

void SpeechInput::accept_request(const std::string &text) {
  // Close this wake session before publishing the request. Pending or in-flight
  // ASR results carry the old generation and cannot become a second request.
  enter_standby();
  acknowledge_input();
  text_->publish(xc::str(text));
  log_manager_.logger()->info("Single request accepted; waiting for the next wake word");
}

void SpeechInput::return_to_wake_only() {
  xiaocai_robot::common::mpv_command({"stop"});
  last_wake_time_ = xc::monotonic();
  enter_standby();
  xiaocai_msgs::msg::AudioPlay stop;
  stop.path = "__stop__";
  play_->publish(stop);
  log_manager_.logger()->info("Hush command received; waiting for the next wake word");
}
