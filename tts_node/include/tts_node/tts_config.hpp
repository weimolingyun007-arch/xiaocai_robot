#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

struct TtsSettings {
  std::string model_dir{"/home/lckfb/xiaocai_robot/resource/tts/sherpa-onnx-vits-zh-ll"};
  std::int64_t speaker_id{2};
};

class TtsConfig : public xc::NodeConfigBase {
public:
  TtsConfig();
  const TtsSettings &settings() const noexcept { return settings_; }

private:
  TtsSettings settings_;
};
