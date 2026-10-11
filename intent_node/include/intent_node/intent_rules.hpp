#pragma once

#include "defination/intent_type.hpp"
#include "defination/memory_operation.hpp"
#include "defination/music_action.hpp"
#include "defination/volume_action.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace xiaocai_robot {

struct ChatIntent {};

struct DialogueIntent {
  common::IntentType type;
};

struct MusicIntent {
  common::MusicAction action;
  std::string query;
};

struct VolumeIntent {
  common::VolumeAction action;
  int percent = 0;
};

struct MemoryIntent {
  common::MemoryOperation operation;
  std::string content;
  int delay_seconds = 0;
};

struct IntentResult {
  std::string normalized_text;
  std::variant<ChatIntent, DialogueIntent, MusicIntent, VolumeIntent,
               MemoryIntent>
      payload{ChatIntent{}};
};

class IntentRules {
public:
  IntentResult Classify(std::string text) const;

private:
  static bool IsCompoundRequest(const std::string &text);
  static int ChineseNumber(const std::string &text);
  static bool ParseMusicIntent(const std::string &text, IntentResult &result);
};

} // namespace xiaocai_robot
