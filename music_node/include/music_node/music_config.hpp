#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

struct MusicSettings {
  std::string cli_path{"/usr/local/bin/ncm-cli"};
  std::string mpv_socket{""};
  std::string default_keyword{"轻音乐"};
  std::string audio_device{"alsa/xiaocai_speaker"};
  std::string wake_ack_path{"/home/lckfb/xiaocai_robot/resource/speech/wake_ack.wav"};
};

class MusicConfig : public xc::NodeConfigBase {
public:
  MusicConfig();
  const MusicSettings &Settings() const noexcept { return settings_; }

private:
  MusicSettings settings_;
};
