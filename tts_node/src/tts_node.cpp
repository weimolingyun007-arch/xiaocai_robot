#include "tts_node/tts_node.hpp"
#include "common/common.hpp"
#include <algorithm>
#include <c-api.h>
#include <cmath>
#include <filesystem>
#include <rclcpp/rclcpp.hpp>
#include <vector>

namespace xiaocai_robot {

TtsNode::TtsNode()
    : Node("tts_node"), tts_config_(),
      log_manager_("tts_node", tts_config_.Logging()) {
  const auto &settings = tts_config_.Settings();
  std::string dir =
      declare_parameter<std::string>("model_dir", settings.model_dir);
  speaker_ = declare_parameter<int>("speaker_id",
                                    static_cast<int>(settings.speaker_id));
  model_ = dir + "/model.onnx";
  lexicon_ = dir + "/lexicon.txt";
  tokens_ = dir + "/tokens.txt";
  fsts_ = dir + "/phone.fst," + dir + "/date.fst," + dir + "/number.fst";
  SherpaOnnxOfflineTtsConfig config{};
  config.model.vits.model = model_.c_str();
  config.model.vits.lexicon = lexicon_.c_str();
  config.model.vits.tokens = tokens_.c_str();
  config.model.vits.length_scale = 1.0f;
  config.model.vits.noise_scale = 0.667f;
  config.model.vits.noise_scale_w = 0.8f;
  config.model.num_threads = 2;
  config.model.provider = "cpu";
  config.rule_fsts = fsts_.c_str();
  config.max_num_sentences = 1;
  tts_ = SherpaOnnxCreateOfflineTts(&config);
  if (!tts_)
    throw std::runtime_error("Cannot load sherpa TTS model");
  std::filesystem::create_directories("/tmp/xiaocai_tts");
  InitPublishers();
  InitSubscribers();
  LOG_INFO("sherpa Chinese TTS loaded");
}

void TtsNode::InitPublishers() {
  int initialized_count = 0;
  play_publisher_ = create_publisher<xiaocai_msgs::msg::AudioPlay>(
      "/xiaocai_robot/audio/play", 10);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void TtsNode::InitSubscribers() {
  int initialized_count = 0;
  text_subscriber_ = create_subscription<std_msgs::msg::String>(
      "/xiaocai_robot/tts/text", 10,
      [this](std_msgs::msg::String::SharedPtr message) {
        OnTextCallback(message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void TtsNode::OnTextCallback(const std_msgs::msg::String::SharedPtr message) {
  Synthesize(message->data);
}

TtsNode::~TtsNode() {
  if (tts_)
    SherpaOnnxDestroyOfflineTts(tts_);
}

void TtsNode::Synthesize(std::string text) {
  text = Utf8Prefix(Trim(text), 400);
  if (text.empty())
    return;
  SherpaOnnxGenerationConfig generation_config{};
  generation_config.sid = speaker_;
  generation_config.speed = 0.95f;
  auto audio = SherpaOnnxOfflineTtsGenerateWithConfig(
      tts_, text.c_str(), &generation_config, nullptr, nullptr);
  if (!audio || audio->n <= 0) {
    LOG_ERROR("TTS generation failed");
    return;
  }
  auto path = "/tmp/xiaocai_tts/" + std::to_string(++id_) + ".wav";
  std::vector<float> samples(audio->samples, audio->samples + audio->n);
  float peak = 0.0f;
  for (float sample : samples)
    peak = std::max(peak, std::abs(sample));
  if (peak > 0.0f) {
    float gain = std::min(3.0f, 0.8f / peak);
    for (float &sample : samples)
      sample = std::clamp(sample * gain, -0.95f, 0.95f);
  }
  int ok = SherpaOnnxWriteWave(samples.data(), audio->n, audio->sample_rate,
                               path.c_str());
  SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  if (ok) {
    xiaocai_msgs::msg::AudioPlay play;
    play.timestamp = now();
    play.path = path;
    play_publisher_->publish(play);
  } else
    LOG_ERROR("Cannot write TTS wave");
}

} // namespace xiaocai_robot
