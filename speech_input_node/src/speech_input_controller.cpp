#include "speech_input_node/speech_input_controller.hpp"
#include "common/common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace xiaocai_robot {

namespace {
size_t FindWakeword(const std::string &text) {
  static const std::array<std::string, 7> names = {
      "小蔡", "小菜", "小才", "小猜", "小采", "小彩", "小财"};
  size_t position = std::string::npos;
  for (const auto &name : names)
    position = std::min(position, text.find(name));
  return position <= 12 ? position : std::string::npos;
}

std::string CompactText(std::string text) {
  text.erase(std::remove_if(text.begin(), text.end(),
                            [](unsigned char c) {
                              return c == ' ' || c == '\n' || c == '\r' ||
                                     c == '\t';
                            }),
             text.end());
  return Trim(text);
}

std::string StripWakewordTail(std::string rest) {
  static const std::array<std::string, 11> marks = {
      "，", "。", "！", "？", "、", ",", ".", "!", "?", ":", "："};
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
    if (rest.rfind("同学", 0) == 0)
      rest = rest.substr(6);
    else if (FindWakeword(rest) == 0)
      rest = rest.substr(6);
    else
      break;
    trim_marks();
  }
  return rest;
}

bool ExactWakePhrase(const std::string &text) {
  const auto compact = CompactText(text);
  return FindWakeword(compact) == 0 &&
         StripWakewordTail(compact.substr(6)).empty();
}

bool IsHushCommand(const std::string &text) {
  const auto compact = CompactText(text);
  static const std::array<std::string, 6> commands = {
      "闭嘴", "别说了", "别说话", "不要说话", "停止说话", "别讲话"};
  return std::any_of(commands.begin(), commands.end(),
                     [&compact](const auto &command) {
                       return compact.find(command) != std::string::npos;
                     });
}

size_t WakeAliasPrefixSize(const std::string &text) {
  const auto known = FindWakeword(text);
  if (known == 0)
    return 6;
  if (text.size() >= 12 && text.rfind("小", 0) == 0 &&
      text.compare(6, 6, "同学") == 0)
    return 12;
  if (text.size() >= 12 && text.rfind("小", 0) == 0 &&
      text.compare(6, 3, "小") == 0 && text.compare(0, 3, text, 6, 3) == 0)
    return 12;
  return 0;
}
} // namespace

SpeechInputController::SpeechInputController(SpeechInputSettings settings,
                                             LogManager &log_manager,
                                             SpeechInputCallbacks callbacks)
    : settings_(std::move(settings)), log_manager_(log_manager),
      callbacks_(std::move(callbacks)), wake_model_(nullptr),
      wake_recognizer_(nullptr), asr_(nullptr), vad_(nullptr), vad_pending_(),
      recent_samples_(), samples_seen_(0), history_begin_(0), segments_(),
      results_(), queue_mutex_(), queue_ready_(), asr_should_be_loaded_(false),
      asr_load_attempted_(false), decoder_(), stop_(false), playing_(false),
      music_playing_(false), conversation_active_(false),
      wake_candidate_(false), request_in_flight_(false),
      recognition_generation_(0),
      input_gain_(static_cast<float>(settings_.input_gain)),
      inactivity_timeout_sec_(settings_.inactivity_timeout_sec),
      ignore_until_(0.0), armed_until_(0.0), wake_candidate_deadline_(0.0),
      last_wake_time_(0.0), listening_deadline_(0.0) {
  if (!callbacks_.publish_text || !callbacks_.publish_audio_play ||
      !callbacks_.publish_awake)
    throw std::invalid_argument("SpeechInputController callbacks are required");

  vosk_set_log_level(-1);
  wake_model_ = vosk_model_new(settings_.wake_model_path.c_str());
  if (!wake_model_)
    throw std::runtime_error("Cannot load wake model: " +
                             settings_.wake_model_path);
  wake_recognizer_ = vosk_recognizer_new_grm(
      wake_model_, 16000,
      R"(["小蔡","小菜","小蔡 同学","小菜 同学","小蔡 小蔡","小菜 小菜","闭嘴","别说了","别说话","不要说话","停止说话","别讲话","[unk]"])");
  if (!wake_recognizer_)
    throw std::runtime_error("Cannot create wake recognizer");

  SherpaOnnxVadModelConfig vad_config{};
  vad_config.silero_vad.model = settings_.vad_model_path.c_str();
  vad_config.silero_vad.threshold = 0.25f;
  vad_config.silero_vad.min_silence_duration = 0.5f;
  vad_config.silero_vad.min_speech_duration = 0.2f;
  vad_config.silero_vad.max_speech_duration = 12.f;
  vad_config.silero_vad.window_size = 512;
  vad_config.sample_rate = 16000;
  vad_config.num_threads = 1;
  vad_config.provider = "cpu";
  vad_ = SherpaOnnxCreateVoiceActivityDetector(&vad_config, 30.f);
  if (!vad_)
    throw std::runtime_error("Cannot load Silero VAD: " +
                             settings_.vad_model_path);

  decoder_ = std::thread([this] { DecodeLoop(); });
  LOG_INFO("Wake-only standby ready; SenseVoice loads after wake detection");
}

SpeechInputController::~SpeechInputController() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stop_ = true;
  }
  queue_ready_.notify_all();
  if (decoder_.joinable())
    decoder_.join();
  if (vad_)
    SherpaOnnxDestroyVoiceActivityDetector(vad_);
  ReleaseAsrModel();
  if (wake_recognizer_)
    vosk_recognizer_free(wake_recognizer_);
  if (wake_model_)
    vosk_model_free(wake_model_);
}

void SpeechInputController::HandleAudioData(
    const std::vector<std::uint8_t> &audio_data) {
  if (Monotonic() < ignore_until_ || audio_data.size() < 2)
    return;
  if (playing_ || music_playing_) {
    if (vosk_recognizer_accept_waveform(
            wake_recognizer_, reinterpret_cast<const char *>(audio_data.data()),
            audio_data.size()) == 1) {
      const auto result = Parse(vosk_recognizer_result(wake_recognizer_));
      const auto phrase =
          !result.is_object()
              ? std::string{}
              : CompactText(result.value("text", std::string{}));
      if (IsHushCommand(phrase)) {
        LOG_INFO("hush command detected during playback: {}", phrase);
        ReturnToWakeOnly();
      } else if (!playing_ && ExactWakePhrase(phrase) &&
                 Monotonic() - last_wake_time_ > 2.0) {
        AcknowledgeWake();
      }
    }
    return;
  }
  const size_t count = audio_data.size() / 2;
  float block_peak = 0.f;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t raw = static_cast<uint16_t>(audio_data[i * 2]) |
                         (static_cast<uint16_t>(audio_data[i * 2 + 1]) << 8);
    const float sample = static_cast<int16_t>(raw) / 32768.f;
    block_peak = std::max(block_peak, std::abs(sample));
  }
  const float effective_gain = block_peak > 0.f
                                   ? std::min(input_gain_, 0.90f / block_peak)
                                   : input_gain_;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t raw = static_cast<uint16_t>(audio_data[i * 2]) |
                         (static_cast<uint16_t>(audio_data[i * 2 + 1]) << 8);
    const int16_t pcm = static_cast<int16_t>(raw);
    vad_pending_.push_back(pcm * effective_gain / 32768.f);
  }
  size_t processed = 0;
  while (vad_pending_.size() - processed >= 512) {
    for (size_t i = 0; i < 512; ++i) {
      recent_samples_.push_back(vad_pending_[processed + i]);
      ++samples_seen_;
    }
    while (recent_samples_.size() > 16000 * 13) {
      recent_samples_.pop_front();
      ++history_begin_;
    }
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(
        vad_, vad_pending_.data() + processed, 512);
    processed += 512;
  }
  vad_pending_.erase(vad_pending_.begin(), vad_pending_.begin() + processed);
  if (armed_until_ > 0) {
    const double now = Monotonic();
    if (SherpaOnnxVoiceActivityDetectorDetected(vad_)) {
      armed_until_ = listening_deadline_ > 0 ? listening_deadline_
                                             : now + inactivity_timeout_sec_;
    } else if (now >= armed_until_) {
      EnterStandby();
      LOG_INFO("No request for {:.0f} seconds; wake-only standby",
               inactivity_timeout_sec_);
    }
  }
  if (vosk_recognizer_accept_waveform(
          wake_recognizer_, reinterpret_cast<const char *>(audio_data.data()),
          audio_data.size()) == 1) {
    const auto result = Parse(vosk_recognizer_result(wake_recognizer_));
    const auto phrase = !result.is_object()
                            ? std::string{}
                            : CompactText(result.value("text", std::string{}));
    if (IsHushCommand(phrase) &&
        (armed_until_ > Monotonic() || conversation_active_)) {
      LOG_INFO("hush command detected: {}", phrase);
      ReturnToWakeOnly();
      return;
    }
    if (ExactWakePhrase(phrase) && Monotonic() - last_wake_time_ > 2.0) {
      LOG_INFO("wake phrase detected by grammar: {}", phrase);
      SherpaOnnxVoiceActivityDetectorReset(vad_);
      vad_pending_.clear();
      recent_samples_.clear();
      samples_seen_ = history_begin_ = 0;
      wake_candidate_ = false;
      AcknowledgeWake();
      return;
    }
    if (FindWakeword(phrase) != std::string::npos) {
      if (!wake_candidate_)
        wake_candidate_deadline_ = Monotonic() + 15.0;
      wake_candidate_ = true;
      RequestAsrModel(true);
    }
  } else {
    const auto partial =
        Parse(vosk_recognizer_partial_result(wake_recognizer_));
    if (partial.is_object() &&
        FindWakeword(CompactText(partial.value("partial", std::string{}))) !=
            std::string::npos) {
      if (!wake_candidate_)
        wake_candidate_deadline_ = Monotonic() + 15.0;
      wake_candidate_ = true;
      RequestAsrModel(true);
    }
  }
  CollectSegments();
}

void SpeechInputController::HandleAudioPlaying(bool playing) {
  playing_ = playing;
  ResetCapture();
  if (!playing)
    ignore_until_ = Monotonic() + 0.4;
}

void SpeechInputController::HandleMusicState(common::MusicState state) {
  const bool playing = state == common::MusicState::Playing;
  if (playing == music_playing_)
    return;
  music_playing_ = playing;
  HandleAudioPlaying(playing_);
  ++recognition_generation_;
  request_in_flight_ = false;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  segments_.clear();
  results_.clear();
}

void SpeechInputController::HandleConversation(common::ConversationPhase phase,
                                               bool expect_reply) {
  conversation_active_ = phase != common::ConversationPhase::Idle;
  if (phase == common::ConversationPhase::Listening && expect_reply) {
    armed_until_ = 0;
    ++recognition_generation_;
    request_in_flight_ = false;
    ResetCapture();
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      segments_.clear();
      results_.clear();
    }
    listening_deadline_ = Monotonic() + settings_.followup_timeout_sec;
    armed_until_ = listening_deadline_;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (!asr_should_be_loaded_) {
        asr_should_be_loaded_ = true;
        asr_load_attempted_ = false;
      }
    }
    queue_ready_.notify_one();
    ignore_until_ = Monotonic() + 0.4;
    callbacks_.publish_awake(true);
    LOG_INFO("Listening for a follow-up answer for at most {:.0f} seconds",
             settings_.followup_timeout_sec);
  } else if (phase == common::ConversationPhase::Idle && armed_until_ > 0) {
    EnterStandby();
  }
}

void SpeechInputController::DrainResults() {
  if (wake_candidate_ && armed_until_ == 0 &&
      Monotonic() >= wake_candidate_deadline_ && !request_in_flight_) {
    wake_candidate_ = false;
    wake_candidate_deadline_ = 0;
    RequestAsrModel(false);
  }

  // A hard deadline is independent of VAD activity and still expires if the
  // microphone stops producing data. Old ASR work cannot reopen this turn.
  if (listening_deadline_ > 0 && Monotonic() >= listening_deadline_) {
    EnterStandby();
    LOG_INFO("Listening window timed out; waiting for the next wake word");
  }
  std::deque<SpeechRecognitionResult> results;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    results.swap(results_);
  }
  for (const auto &result : results) {
    HandleRecognitionResult(result);
  }
}

std::uint64_t SpeechInputController::AcknowledgeWake() {
  // music_node pauses playback when it receives the awake notification.
  listening_deadline_ = Monotonic() + inactivity_timeout_sec_;
  music_playing_ = false;
  conversation_active_ = true;
  armed_until_ = listening_deadline_;
  last_wake_time_ = Monotonic();
  ++recognition_generation_;
  request_in_flight_ = false;
  ResetCapture();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    segments_.clear();
    results_.clear();
    asr_should_be_loaded_ = true;
    asr_load_attempted_ = false;
  }
  queue_ready_.notify_one();
  callbacks_.publish_awake(true);
  callbacks_.publish_audio_play(settings_.wake_ack_path);
  LOG_INFO("wakeword acknowledged");
  return recognition_generation_;
}

void SpeechInputController::HandleRecognitionResult(
    const SpeechRecognitionResult &result) {
  if (result.generation != recognition_generation_)
    return;
  request_in_flight_ = false;
  if (!result.text.empty())
    HandleText(result.text, result.was_armed);
  if (!result.was_armed && armed_until_ == 0)
    RequestAsrModel(false);
}

void SpeechInputController::CollectSegments() {
  while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
    const auto *segment = SherpaOnnxVoiceActivityDetectorFront(vad_);
    const bool armed = Monotonic() < armed_until_;
    const bool wake_candidate = wake_candidate_;
    bool queued_segment = false;
    if (segment && segment->n >= 3200 && (armed || wake_candidate_) &&
        !request_in_flight_) {
      if (armed)
        armed_until_ = Monotonic() + inactivity_timeout_sec_;
      const int64_t prefix_start = std::max<int64_t>(
          history_begin_, static_cast<int64_t>(segment->start) - 16000);
      const size_t prefix_offset =
          static_cast<size_t>(prefix_start - history_begin_);
      const size_t prefix_count = static_cast<size_t>(
          static_cast<int64_t>(segment->start) - prefix_start);
      std::vector<float> audio;
      audio.reserve(prefix_count + segment->n);
      auto prefix = recent_samples_.begin();
      std::advance(prefix, prefix_offset);
      auto prefix_end = prefix;
      std::advance(prefix_end, prefix_count);
      audio.insert(audio.end(), prefix, prefix_end);
      audio.insert(audio.end(), segment->samples,
                   segment->samples + segment->n);
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        segments_.push_back({std::move(audio), armed, recognition_generation_});
        request_in_flight_ = true;
        queued_segment = true;
      }
      queue_ready_.notify_one();
    }
    if (segment)
      SherpaOnnxDestroySpeechSegment(segment);
    SherpaOnnxVoiceActivityDetectorPop(vad_);
    wake_candidate_ = false;
    wake_candidate_deadline_ = 0;
    if (wake_candidate && !armed && !queued_segment && !request_in_flight_)
      RequestAsrModel(false);
  }
}

void SpeechInputController::DecodeLoop() {
  while (true) {
    Segment segment;
    bool load_asr = false;
    bool release_asr = false;
    bool has_segment = false;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_ready_.wait(lock, [this] {
        return stop_ || !segments_.empty() ||
               (asr_should_be_loaded_ && !asr_ && !asr_load_attempted_) ||
               (!asr_should_be_loaded_ && asr_);
      });
      if (stop_)
        return;
      if (asr_should_be_loaded_ && !asr_ && !asr_load_attempted_) {
        asr_load_attempted_ = true;
        load_asr = true;
      } else if (!asr_should_be_loaded_ && asr_) {
        release_asr = true;
      } else if (!segments_.empty()) {
        segment = std::move(segments_.front());
        segments_.pop_front();
        has_segment = true;
      }
    }
    if (load_asr) {
      LoadAsrModel();
      continue;
    }
    if (release_asr) {
      ReleaseAsrModel();
      continue;
    }
    if (!has_segment)
      continue;

    if (!asr_) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      results_.push_back({{}, segment.was_armed, segment.generation});
      continue;
    }
    const auto *stream = SherpaOnnxCreateOfflineStream(asr_);
    if (!stream) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      results_.push_back({{}, segment.was_armed, segment.generation});
      continue;
    }
    SherpaOnnxAcceptWaveformOffline(
        stream, 16000, segment.samples.data(),
        static_cast<int32_t>(segment.samples.size()));
    const double began = Monotonic();
    SherpaOnnxDecodeOfflineStream(asr_, stream);
    const auto *result = SherpaOnnxGetOfflineStreamResult(stream);
    const auto heard =
        result && result->text ? CompactText(result->text) : std::string{};
    LOG_INFO("SenseVoice heard: {} ({:.2f} s decode)", heard,
             Monotonic() - began);
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      results_.push_back({heard, segment.was_armed, segment.generation});
    }
    if (result)
      SherpaOnnxDestroyOfflineRecognizerResult(result);
    SherpaOnnxDestroyOfflineStream(stream);
  }
}

void SpeechInputController::LoadAsrModel() {
  SherpaOnnxOfflineRecognizerConfig asr_config{};
  asr_config.feat_config.sample_rate = 16000;
  asr_config.feat_config.feature_dim = 80;
  asr_config.model_config.sense_voice.model =
      settings_.sense_model_path.c_str();
  asr_config.model_config.sense_voice.language = "zh";
  asr_config.model_config.sense_voice.use_itn = 1;
  asr_config.model_config.tokens = settings_.sense_tokens_path.c_str();
  asr_config.model_config.num_threads = 2;
  asr_config.model_config.provider = "cpu";
  asr_config.decoding_method = "greedy_search";
  asr_ = SherpaOnnxCreateOfflineRecognizer(&asr_config);
  if (!asr_) {
    LOG_ERROR("Cannot load SenseVoice model: {}", settings_.sense_model_path);
    return;
  }
  LOG_INFO("SenseVoice loaded for active listening");
}

void SpeechInputController::RequestAsrModel(bool should_load) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (asr_should_be_loaded_ != should_load) {
      asr_should_be_loaded_ = should_load;
      asr_load_attempted_ = false;
    }
  }
  queue_ready_.notify_one();
}

void SpeechInputController::ReleaseAsrModel() {
  if (!asr_)
    return;
  SherpaOnnxDestroyOfflineRecognizer(asr_);
  asr_ = nullptr;
  LOG_INFO("SenseVoice unloaded; wake-only standby ready");
}

void SpeechInputController::HandleText(const std::string &heard,
                                       bool was_armed) {
  if (music_playing_ || (was_armed && armed_until_ == 0))
    return;
  if (IsHushCommand(heard) &&
      (was_armed || FindWakeword(heard) != std::string::npos)) {
    ReturnToWakeOnly();
    return;
  }
  const auto position = FindWakeword(heard);
  if (position != std::string::npos) {
    const auto rest = StripWakewordTail(heard.substr(position + 6));
    if (rest.empty()) {
      if (!was_armed && Monotonic() - last_wake_time_ > 2.0)
        AcknowledgeWake();
    } else {
      const double now = Monotonic();
      if (now - last_wake_time_ > 2.0) {
        if (!was_armed) {
          AcknowledgeWake();
        } else {
          callbacks_.publish_awake(true);
        }
      }
      last_wake_time_ = Monotonic();
      AcceptRequest(rest);
    }
  } else if (was_armed) {
    std::string command = heard;
    if (Monotonic() - last_wake_time_ < 4.0) {
      const auto alias_size = WakeAliasPrefixSize(heard);
      if (alias_size > 0) {
        command = StripWakewordTail(heard.substr(alias_size));
        if (command.empty()) {
          LOG_INFO("ignored duplicate ASR wake phrase: {}", heard);
          return;
        }
      }
    }
    AcceptRequest(command);
  }
}

void SpeechInputController::ResetCapture() {
  wake_candidate_ = false;
  wake_candidate_deadline_ = 0;
  vosk_recognizer_reset(wake_recognizer_);
  SherpaOnnxVoiceActivityDetectorReset(vad_);
  vad_pending_.clear();
  recent_samples_.clear();
  samples_seen_ = history_begin_ = 0;
}

void SpeechInputController::EnterStandby() {
  armed_until_ = 0;
  listening_deadline_ = 0;
  ++recognition_generation_;
  request_in_flight_ = false;
  ResetCapture();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    segments_.clear();
    results_.clear();
    asr_should_be_loaded_ = false;
    asr_load_attempted_ = false;
  }
  queue_ready_.notify_one();
  callbacks_.publish_awake(false);
}

void SpeechInputController::AcceptRequest(const std::string &text) {
  // Suspend capture while this turn is processed. Dialogue policy re-arms it
  // after a question has finished playing; old ASR generations are discarded.
  EnterStandby();
  callbacks_.publish_text(text);
  LOG_INFO("Request accepted; waiting for the dialogue response");
}

void SpeechInputController::ReturnToWakeOnly() {
  conversation_active_ = false;
  last_wake_time_ = Monotonic();
  EnterStandby();
  callbacks_.publish_audio_play({});
  LOG_INFO("Hush command received; waiting for the next wake word");
}

} // namespace xiaocai_robot
