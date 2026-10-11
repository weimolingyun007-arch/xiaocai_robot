#pragma once

#include <cstdint>

namespace xiaocai_robot::common {

enum class IntentType : std::uint8_t {
  Chat = 0,
  VolumeGet = 7,
  QuietOff = 11,
  QuietOn = 12,
  ReminderClarify = 17,
};

constexpr bool IsKnownIntent(IntentType intent) {
  switch (intent) {
  case IntentType::Chat:
  case IntentType::VolumeGet:
  case IntentType::QuietOff:
  case IntentType::QuietOn:
  case IntentType::ReminderClarify:
    return true;
  }
  return false;
}

constexpr const char *IntentName(IntentType intent) {
  switch (intent) {
  case IntentType::Chat:
    return "chat";
  case IntentType::VolumeGet:
    return "volume_get";
  case IntentType::QuietOff:
    return "quiet_off";
  case IntentType::QuietOn:
    return "quiet_on";
  case IntentType::ReminderClarify:
    return "reminder_clarify";
  }
  return "unknown";
}

} // namespace xiaocai_robot::common
