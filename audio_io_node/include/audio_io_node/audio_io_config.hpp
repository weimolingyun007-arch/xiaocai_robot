#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

struct AudioIoSettings {
  std::string capture_device{"hw:0,0"};
  std::string playback_device{"xiaocai_speaker"};
  std::string wake_ack_path{"/home/lckfb/xiaocai_robot/resource/speech/wake_ack.wav"};
  std::int64_t volume_percent{80};
  std::int64_t adc_pga_gain{9};
  std::int64_t mic_boost_gain{1};
};

class AudioIoConfig : public xc::NodeConfigBase {
public:
  AudioIoConfig();
  const AudioIoSettings &settings() const noexcept { return settings_; }

private:
  AudioIoSettings settings_;
};
