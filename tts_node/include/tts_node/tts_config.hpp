#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

namespace xiaocai_robot {

struct TtsSettings {
  std::string model_dir{
      "/home/lckfb/xiaocai_robot/resource/tts/sherpa-onnx-vits-zh-ll"};
  std::int64_t speaker_id{2};
};

class TtsConfig : public NodeConfigBase {
public:
  TtsConfig();
  const TtsSettings &Settings() const noexcept { return settings_; }

private:
  TtsSettings settings_;
};

} // namespace xiaocai_robot
