#pragma once

#include <cstdint>

namespace xiaocai_robot::common {

enum class MemoryOperation : std::uint8_t {
  SaveFact = 1,
  ForgetFact = 2,
  ListFacts = 3,
  AddReminder = 4,
  SetQuiet = 5,
  GetQuiet = 6,
};

}  // namespace xiaocai_robot::common
