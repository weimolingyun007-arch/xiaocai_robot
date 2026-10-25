#pragma once

#include "audio_io_node/alsa_pcm.hpp"
#include "common/node_config_base.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace xiaocai_robot {

struct AudioIoSettings {
  AlsaPcmSettings alsa;
  std::string capture_device{"hw:0,0"};
  std::string playback_device{"xiaocai_speaker"};
  std::string wake_ack_path{
      "/home/lckfb/xiaocai_robot/resource/speech/wake_ack.wav"};
  std::string startup_music_path{"audio/boot_music.wav"};
  std::int64_t volume_percent{80};
  std::int64_t adc_pga_gain{9};
  std::int64_t mic_boost_gain{1};
};

class AudioIoConfig : public NodeConfigBase {
public:
  AudioIoConfig();
  const AudioIoSettings &Settings() const noexcept { return settings_; }
  std::filesystem::path StartupMusicPath() const;

private:
  AudioIoSettings settings_;
};

} // namespace xiaocai_robot
