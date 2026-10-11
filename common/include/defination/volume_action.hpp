#pragma once

#include <cstdint>

namespace xiaocai_robot::common {

// Action values carried by the uint8 VolumeCommand.action field.
enum class VolumeAction : std::uint8_t {
  Up = 1,
  Down = 2,
  Set = 3,
};

} // namespace xiaocai_robot::common
