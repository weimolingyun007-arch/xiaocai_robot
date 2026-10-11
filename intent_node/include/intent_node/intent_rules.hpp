#pragma once

#include <string>

struct IntentResult {
  std::string normalized_text;
  std::string intent = "chat";
  std::string content;
  int volume_percent = 0;
  int delay_seconds = 0;
};

class IntentRules {
public:
  IntentResult classify(std::string text) const;

private:
  static bool is_compound_request(const std::string &text);
  static int chinese_number(const std::string &text);
  static bool parse_music_intent(const std::string &text, IntentResult &result);
};
