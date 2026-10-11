#pragma once
#include <cstdint>

namespace xiaocai_robot::common {
enum class MusicState : std::uint8_t {
  Stopped = 0,
  Searching = 1,
  Playing = 2,
  Paused = 3,
  Error = 4
};
}
