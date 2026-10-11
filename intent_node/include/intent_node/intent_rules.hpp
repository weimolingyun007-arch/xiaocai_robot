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
  IntentResult Classify(std::string text) const;

private:
  static bool IsCompoundRequest(const std::string &text);
  static int ChineseNumber(const std::string &text);
  static bool ParseMusicIntent(const std::string &text, IntentResult &result);
};
