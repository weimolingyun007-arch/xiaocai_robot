#pragma once

#include <cstdint>

namespace xiaocai_robot::common {

enum class RobotState : std::uint8_t {
  Idle = 0,
  Listening = 1,
  Thinking = 2,
  Speaking = 3,
  Quiet = 4,
  Muted = 5,
};

}  // namespace xiaocai_robot::common
