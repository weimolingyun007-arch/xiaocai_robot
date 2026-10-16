#pragma once

#include <cstdint>

namespace xiaocai_robot::common {

enum class BleStatusState : std::uint8_t {
  Disconnected = 0,
  Scanning = 1,
  Connected = 2,
};

} // namespace xiaocai_robot::common
