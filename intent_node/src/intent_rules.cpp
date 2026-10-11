#include "intent_node/intent_rules.hpp"

#include "common/common.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <string>
#include <utility>

namespace {

struct MusicControlRule {
  std::regex pattern;
  const char *intent;
};

const std::array<MusicControlRule, 5> kMusicControlRules{{
    {std::regex(R"(^(?:请|帮我|给我)?(?:暂停|先暂停)(?:一下)?(?:音乐|歌曲|播放|音乐播放)?$)"), "music_pause"},
    {std::regex(R"(^(?:请|帮我|给我)?(?:继续|恢复)(?:一下)?(?:播放|放歌|音乐|歌曲|播放音乐|播放歌曲)?$)"), "music_resume"},
    {std::regex(R"(^(?:请|帮我|给我)?(?:停止|停止播放|关掉|关闭|别放|不要放|不要播放)(?:音乐|歌曲|放歌|播放|歌)?(?:了)?$)"), "music_stop"},
    {std::regex(R"(^(?:请|帮我|给我)?(?:下一首|切歌|换一首|换首歌)(?:歌|歌曲)?$)"), "music_next"},
    {std::regex(R"(^(?:请|帮我|给我)?(?:上一首|前一首)(?:歌|歌曲)?$)"), "music_previous"},
}};

const std::regex kMusicPlayPattern(
    R"(^(?:请你|请|帮我|给我)?(?:播放|放一首|放首|来一首|我想听一首|我想听|听一首|放音乐|放歌)(.*)$)");
const std::regex kEmptyMusicQueryPattern(
    R"(^(?:任意|随便)?(?:一首|首)?(?:音乐|歌|歌曲)?$)");
const std::regex kCompoundWordPattern(R"(顺便|并且|同时|另外|然后|还有|再帮我|以及)");
const std::regex kTimeQuestionPattern(R"(几点|时间|日期|星期|几号)");
const std::regex kOtherActionPattern(R"(音量|喇叭|播放|音乐|提醒|记住|忘记)");

const std::regex kVolumeGetPattern(
    R"((?:我)?(?:现在|当前|目前)?(?:的)?(?:音量|声音)(?:现在|当前|目前)?(?:是)?(?:多少|多大|几(?:档|成|分贝)?)(?:吗|呢|啊|呀)?|(?:多少|多大)(?:音量|声音))");
const std::regex kVolumeSetPattern(
    R"((?:音量|声音|喇叭)(?:调到|调成|设为|设置到|到)?(?:百分之)?([0-9]{1,3}|一百|(?:一|二|两|三|四|五|六|七|八|九)?十(?:一|二|三|四|五|六|七|八|九)?|零|一|二|两|三|四|五|六|七|八|九)(?:%|％)?)");
const std::regex kVolumeUpShortPattern(R"((?:你俩|你量|音量)(?:再)?大(?:一点|点)?)");
const std::regex kVolumeDownShortPattern(R"((?:你俩|你量|音量)(?:再)?小(?:一点|点)?)");
const std::regex kVolumeUpPattern(
    R"((?:音量|声音|喇叭).*?(?:调大|加大|提高|增大|调高|大一点|大点|更大)|(?:帮我)?(?:把)?(?:调大|加大|提高|增大|调高)(?:一下|一点|点)?(?:音量|声音|喇叭)|大声一点|大声点|声音太小)");
const std::regex kVolumeDownPattern(
    R"((?:音量|声音|喇叭).*?(?:调小|减小|降低|调低|小一点|小点|更小)|(?:帮我)?(?:把)?(?:调小|减小|降低|调低)(?:一下|一点|点)?(?:音量|声音|喇叭)|小声一点|小声点|声音太大)");
const std::regex kQuietOffPattern(R"(取消免打扰|关闭免打扰|可以提醒我|恢复提醒|不再安静)");
const std::regex kQuietOnPattern(R"(别打扰我|不要打扰|免打扰|安静一会|保持安静)");
const std::regex kMemoryListPattern(R"(你记得我|你记住了什么|我喜欢什么)");
const std::regex kMemoryForgetPattern(R"((?:忘记|忘掉|不要记得)(.+))");
const std::regex kMemorySavePattern(R"((?:记住|记得)(.+))");
const std::regex kReminderPattern(
    R"(([0-9]+|[一二两三四五六七八九十]+)(?:个)?(秒钟?|分钟?|小时|天)后(?:再)?提醒我(.+))");

}  // namespace

bool IntentRules::is_compound_request(const std::string &text) {
  if (std::regex_search(text, kCompoundWordPattern)) return true;
  return std::regex_search(text, kTimeQuestionPattern) &&
         std::regex_search(text, kOtherActionPattern);
}

int IntentRules::chinese_number(const std::string &text) {
  if (!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isdigit(c);
      })) {
    return std::stoi(text);
  }

  if (text == "一百" || text == "百") return 100;

  const std::array<std::string, 10> digits{
      "零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  const auto digit_value = [&digits](const std::string &value) {
    if (value == "两") return 2;
    for (int i = 0; i < static_cast<int>(digits.size()); ++i) {
      if (value == digits[static_cast<size_t>(i)]) return i;
    }
    return 0;
  };

  const size_t ten_position = text.find("十");
  if (ten_position != std::string::npos) {
    const int tens = ten_position == 0 ? 1 : digit_value(text.substr(0, ten_position));
    const size_t ones_position = ten_position + std::string("十").size();
    const int ones = ones_position >= text.size() ? 0 : digit_value(text.substr(ones_position));
    return tens * 10 + ones;
  }
  return digit_value(text);
}

bool IntentRules::parse_music_intent(const std::string &text, IntentResult &result) {
  for (const auto &rule : kMusicControlRules) {
    if (!std::regex_match(text, rule.pattern)) continue;
    result.intent = rule.intent;
    return true;
  }

  std::smatch match;
  if (!std::regex_match(text, match, kMusicPlayPattern)) return false;

  std::string song = match[1];
  if (song.rfind("一首", 0) == 0) song.erase(0, std::string("一首").size());
  if (song.size() >= std::string("吧").size() &&
      song.compare(song.size() - std::string("吧").size(), std::string("吧").size(), "吧") == 0) {
    song.resize(song.size() - std::string("吧").size());
  }

  for (const std::string mark : {"《", "》"}) {
    size_t position = song.find(mark);
    while (position != std::string::npos) {
      song.erase(position, mark.size());
      position = song.find(mark);
    }
  }

  if (std::regex_match(song, kEmptyMusicQueryPattern)) song.clear();
  result.intent = "music_play";
  result.content = std::move(song);
  return true;
}

IntentResult IntentRules::classify(std::string text) const {
  IntentResult result;
  result.normalized_text = xc::trim(text);
  result.normalized_text.erase(
      std::remove_if(result.normalized_text.begin(), result.normalized_text.end(),
                     [](unsigned char character) {
                       return character == ' ' || character == '\t' ||
                              character == '\n' || character == '\r';
                     }),
      result.normalized_text.end());

  if (result.normalized_text.empty() || is_compound_request(result.normalized_text)) {
    return result;
  }
  if (parse_music_intent(result.normalized_text, result)) return result;

  std::smatch match;
  if (std::regex_search(result.normalized_text, kVolumeGetPattern)) {
    result.intent = "volume_get";
  } else if (std::regex_search(result.normalized_text, match, kVolumeSetPattern)) {
    result.intent = "volume_set";
    result.volume_percent = std::clamp(chinese_number(match[1]), 0, 150);
  } else if (result.normalized_text.find("静音") != std::string::npos &&
             result.normalized_text.find("取消") == std::string::npos) {
    result.intent = "volume_set";
  } else if (std::regex_match(result.normalized_text, kVolumeUpShortPattern) ||
             std::regex_search(result.normalized_text, kVolumeUpPattern)) {
    result.intent = "volume_up";
  } else if (std::regex_match(result.normalized_text, kVolumeDownShortPattern) ||
             std::regex_search(result.normalized_text, kVolumeDownPattern)) {
    result.intent = "volume_down";
  } else if (std::regex_search(result.normalized_text, kQuietOffPattern)) {
    result.intent = "quiet_off";
  } else if (std::regex_search(result.normalized_text, kQuietOnPattern)) {
    result.intent = "quiet_on";
  } else if (std::regex_search(result.normalized_text, kMemoryListPattern)) {
    result.intent = "memory_list";
  } else if (std::regex_search(result.normalized_text, match, kMemoryForgetPattern)) {
    result.intent = "memory_forget";
    result.content = xc::trim(match[1]);
  } else if (std::regex_search(result.normalized_text, match, kMemorySavePattern)) {
    result.intent = "memory_save";
    result.content = xc::trim(match[1]);
  } else if (std::regex_search(result.normalized_text, match, kReminderPattern)) {
    const int amount = chinese_number(match[1]);
    const std::string unit = match[2];
    const std::string reminder_content = xc::trim(match[3]);
    int seconds_per_unit = 86400;
    if (unit.find("秒") == 0) {
      seconds_per_unit = 1;
    } else if (unit.find("分") == 0) {
      seconds_per_unit = 60;
    } else if (unit.find("小") == 0) {
      seconds_per_unit = 3600;
    }

    if (amount > 0 && amount * seconds_per_unit <= 30 * 86400 &&
        !reminder_content.empty()) {
      result.intent = "create_reminder";
      result.content = reminder_content;
      result.delay_seconds = amount * seconds_per_unit;
    } else {
      result.intent = "reminder_clarify";
    }
  } else if (result.normalized_text.find("提醒我") != std::string::npos) {
    result.intent = "reminder_clarify";
  }

  return result;
}
