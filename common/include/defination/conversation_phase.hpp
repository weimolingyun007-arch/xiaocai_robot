#pragma once
#include <cstdint>

namespace xiaocai_robot::common {
enum class ConversationPhase : std::uint8_t {
  Idle = 0,
  Listening = 1,
  Processing = 2,
  Speaking = 3,
};
} // namespace xiaocai_robot::common
