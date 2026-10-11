#pragma once

#include <cstdint>

namespace xiaocai_robot::common {

enum class MusicAction : std::uint8_t {
  Play = 1,
  Pause = 2,
  Resume = 3,
  Stop = 4,
  Next = 5,
  Previous = 6,
};

constexpr bool IsMusicAction(MusicAction action) {
  return action >= MusicAction::Play && action <= MusicAction::Previous;
}

constexpr const char *MusicActionName(MusicAction action) {
  switch (action) {
  case MusicAction::Play:
    return "music_play";
  case MusicAction::Pause:
    return "music_pause";
  case MusicAction::Resume:
    return "music_resume";
  case MusicAction::Stop:
    return "music_stop";
  case MusicAction::Next:
    return "music_next";
  case MusicAction::Previous:
    return "music_previous";
  }
  return "unknown";
}

} // namespace xiaocai_robot::common
