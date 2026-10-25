#pragma once

#include <cstdint>

namespace xiaocai_robot::common {

enum class AudioStreamAction : std::uint8_t {
  Data = 1,
  End = 2,    // Drain the queued samples and close the output.
  Cancel = 3, // Discard queued samples and stop the output immediately.
};

} // namespace xiaocai_robot::common
