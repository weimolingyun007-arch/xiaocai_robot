#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

struct SpeechInputSettings {
  std::string wake_model_path{"/home/lckfb/xiaocai_robot/resource/speech/vosk-model-small-cn-0.22"};
  std::string sense_model_path{"/home/lckfb/xiaocai_robot/resource/speech/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/model.int8.onnx"};
  std::string sense_tokens_path{"/home/lckfb/xiaocai_robot/resource/speech/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/tokens.txt"};
  std::string vad_model_path{"/home/lckfb/xiaocai_robot/resource/speech/silero_vad.onnx"};
  std::string wake_ack_path{"/home/lckfb/xiaocai_robot/resource/speech/wake_ack.wav"};
  std::string wake_ack_ding_path{"/home/lckfb/xiaocai_robot/resource/speech/wake_ack_ding.wav"};
  double input_gain{6.0};
  double inactivity_timeout_sec{120.0};
};

class SpeechInputConfig : public xc::NodeConfigBase {
public:
  SpeechInputConfig();
  const SpeechInputSettings &settings() const noexcept { return settings_; }

private:
  SpeechInputSettings settings_;
};
