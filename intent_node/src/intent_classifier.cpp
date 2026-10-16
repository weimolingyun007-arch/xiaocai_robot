#include "intent_node/intent_classifier.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace xiaocai_robot {

namespace {

using Json = nlohmann::json;

std::vector<std::pair<std::string, std::uint32_t>>
DecodeUtf8(const std::string &text) {
  std::vector<std::pair<std::string, std::uint32_t>> codepoints;
  for (std::size_t offset = 0; offset < text.size();) {
    const auto first = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1;
    std::uint32_t value = first;
    if ((first & 0xE0U) == 0xC0U && offset + 1 < text.size()) {
      length = 2;
      value = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U && offset + 2 < text.size()) {
      length = 3;
      value = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U && offset + 3 < text.size()) {
      length = 4;
      value = first & 0x07U;
    }
    for (std::size_t index = 1; index < length; ++index) {
      value = (value << 6U) |
              (static_cast<unsigned char>(text[offset + index]) & 0x3FU);
    }
    codepoints.emplace_back(text.substr(offset, length), value);
    offset += length;
  }
  return codepoints;
}

bool IsWhitespace(std::uint32_t codepoint) {
  return codepoint == 0x20U || (codepoint >= 0x09U && codepoint <= 0x0DU) ||
         codepoint == 0x00A0U || codepoint == 0x1680U ||
         (codepoint >= 0x2000U && codepoint <= 0x200AU) ||
         codepoint == 0x2028U || codepoint == 0x2029U || codepoint == 0x202FU ||
         codepoint == 0x205FU || codepoint == 0x3000U;
}

std::uint32_t FeatureHash(const std::string &ngram) {
  std::uint32_t value = 2166136261U;
  for (const unsigned char byte : ngram) {
    value ^= byte;
    value *= 16777619U;
  }
  return value;
}

std::vector<double> Softmax(const std::vector<double> &logits) {
  if (logits.empty())
    return {};
  const double maximum = *std::max_element(logits.begin(), logits.end());
  std::vector<double> probabilities;
  probabilities.reserve(logits.size());
  double total = 0.0;
  for (const double value : logits) {
    probabilities.push_back(std::exp(value - maximum));
    total += probabilities.back();
  }
  for (double &probability : probabilities)
    probability /= total;
  return probabilities;
}

std::string RemoveWhitespace(const std::string &text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const auto &[token, codepoint] : DecodeUtf8(text)) {
    if (!IsWhitespace(codepoint))
      normalized += token;
  }
  return normalized;
}

std::string Trim(std::string text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())))
    text.erase(text.begin());
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.pop_back();
  return text;
}

bool IsChineseDigit(const std::string &token) {
  static const std::array<std::string, 13> kDigits{"零", "一", "二", "两", "三",
                                                   "四", "五", "六", "七", "八",
                                                   "九", "十", "百"};
  return std::find(kDigits.begin(), kDigits.end(), token) != kDigits.end();
}

int ChineseNumber(const std::string &text) {
  if (text.empty())
    return -1;
  if (std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isdigit(character);
      })) {
    try {
      return std::stoi(text);
    } catch (const std::exception &) {
      return -1;
    }
  }

  int total = 0;
  int pending = 0;
  for (const auto &[token, codepoint] : DecodeUtf8(text)) {
    (void)codepoint;
    if (token == "十") {
      total += (pending == 0 ? 1 : pending) * 10;
      pending = 0;
    } else if (token == "百") {
      total += (pending == 0 ? 1 : pending) * 100;
      pending = 0;
    } else {
      static const std::array<std::string, 10> kDigits{
          "零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
      const auto iterator = std::find(kDigits.begin(), kDigits.end(), token);
      if (token == "两")
        pending = 2;
      else if (iterator != kDigits.end())
        pending = static_cast<int>(std::distance(kDigits.begin(), iterator));
      else
        return -1;
    }
  }
  return total + pending;
}

bool FindNumber(const std::string &text, std::size_t from, std::size_t &start,
                std::size_t &end, int &value) {
  const auto codepoints = DecodeUtf8(text);
  std::size_t byte_offset = 0;
  std::size_t token_start = std::string::npos;
  std::string number;
  for (const auto &[token, codepoint] : codepoints) {
    const bool is_ascii_digit =
        token.size() == 1 && std::isdigit(static_cast<unsigned char>(token[0]));
    const bool is_digit = is_ascii_digit || IsChineseDigit(token);
    if (byte_offset >= from && is_digit) {
      if (token_start == std::string::npos)
        token_start = byte_offset;
      number += token;
    } else if (token_start != std::string::npos) {
      start = token_start;
      end = byte_offset;
      value = ChineseNumber(number);
      return value >= 0;
    }
    byte_offset += token.size();
    (void)codepoint;
  }
  if (token_start == std::string::npos)
    return false;
  start = token_start;
  end = text.size();
  value = ChineseNumber(number);
  return value >= 0;
}

std::size_t FindLastAny(const std::string &text,
                        const std::vector<std::string> &markers,
                        std::size_t &marker_length) {
  std::size_t result = std::string::npos;
  marker_length = 0;
  for (const auto &marker : markers) {
    const std::size_t found = text.rfind(marker);
    if (found != std::string::npos &&
        (result == std::string::npos || found > result)) {
      result = found;
      marker_length = marker.size();
    }
  }
  return result;
}

std::string StripCommandPrefix(std::string text,
                               const std::vector<std::string> &prefixes) {
  bool removed = true;
  while (removed) {
    removed = false;
    for (const auto &prefix : prefixes) {
      if (text.rfind(prefix, 0) == 0) {
        text.erase(0, prefix.size());
        removed = true;
        break;
      }
    }
  }
  return Trim(std::move(text));
}

std::string ExtractMusicQuery(const std::string &text) {
  std::string command_text = text;
  std::size_t correction_marker_length = 0;
  const std::size_t correction_position =
      FindLastAny(command_text,
                  {"而是", "其实", "我想说的是", "我的意思是", "我想要的是",
                   "纠正一下，", "更正一下，", "刚才说错了，", "不对，", "不是",
                   "我是想", "我想", "我要"},
                  correction_marker_length);
  if (correction_position != std::string::npos)
    command_text.erase(0, correction_position + correction_marker_length);
  std::string query = StripCommandPrefix(
      std::move(command_text),
      {"请你",     "请",     "麻烦",       "帮我",   "给我",
       "播放一下", "播放点", "我想听一首", "我想听", "我想播放",
       "我想要听", "我想要", "我是想",     "我想",   "想听",
       "听一首",   "听",     "播放",       "放一点", "放点",
       "放一首",   "放首",   "来一首",     "放音乐", "放歌"});
  if (query.rfind("一首", 0) == 0)
    query.erase(0, std::string("一首").size());
  for (const std::string &suffix : {"吧", "啊", "呢"}) {
    if (query.size() >= suffix.size() &&
        query.compare(query.size() - suffix.size(), suffix.size(), suffix) == 0)
      query.erase(query.size() - suffix.size());
  }
  for (const std::string &mark : {"《", "》", "“", "”", "\"", "'"}) {
    std::size_t position = query.find(mark);
    while (position != std::string::npos) {
      query.erase(position, mark.size());
      position = query.find(mark);
    }
  }
  if (query == "音乐" || query == "歌曲" || query == "歌" || query == "")
    return {};
  return query;
}

int ExtractVolume(const std::string &text) {
  std::size_t start = 0;
  std::size_t end = 0;
  int value = 0;
  if (!FindNumber(text, 0, start, end, value))
    return -1;
  return std::clamp(value, 0, 150);
}

std::string ExtractMemoryContent(const std::string &text, bool forget) {
  std::string content_text = text;
  std::size_t correction_marker_length = 0;
  const std::size_t correction_position = FindLastAny(
      content_text,
      {"而是", "其实", "我的意思是", "我想说的是", "我想要的是", "纠正一下，",
       "更正一下，", "刚才说错了，", "不对，", "不是"},
      correction_marker_length);
  if (correction_position != std::string::npos)
    content_text.erase(0, correction_position + correction_marker_length);
  const std::vector<std::string> action_markers =
      forget
          ? std::vector<std::string>{"不要记得", "别再记得", "忘掉", "忘记",
                                     "删除",     "清除",     "移除"}
          : std::vector<std::string>{"记住", "记得", "保存", "记下来", "记录"};
  std::size_t action_marker_length = 0;
  const std::size_t action_position =
      FindLastAny(content_text, action_markers, action_marker_length);
  if (action_position != std::string::npos && action_position > 0) {
    const std::size_t action_end = action_position + action_marker_length;
    const bool trailing_le =
        content_text.compare(action_end, std::string("了").size(), "了") == 0 &&
        action_end + std::string("了").size() == content_text.size();
    if (action_end == content_text.size() || trailing_le)
      content_text.erase(action_position);
  }
  const std::vector<std::string> prefixes =
      forget
          ? std::vector<std::string>{"请帮我", "帮我",     "请",       "忘掉",
                                     "忘记",   "不要记得", "别再记得", "你忘掉",
                                     "你忘记", "删除",     "清除",     "移除",
                                     "把",     "请把",     "我要",     "我想要"}
          : std::vector<std::string>{
                "请帮我", "帮我",   "请",   "你记住", "你记得", "记住",  "记得",
                "保存",   "记下来", "记录", "把",     "我要",   "我想要"};
  return StripCommandPrefix(std::move(content_text), prefixes);
}

bool ExtractReminder(const std::string &text, int &delay_seconds,
                     std::string &content) {
  std::size_t number_start = 0;
  std::size_t number_end = 0;
  int amount = 0;
  if (!FindNumber(text, 0, number_start, number_end, amount) || amount <= 0)
    return false;

  std::size_t unit_start = number_end;
  if (text.compare(unit_start, std::string("个").size(), "个") == 0)
    unit_start += std::string("个").size();
  int seconds_per_unit = 0;
  std::size_t unit_length = 0;
  if (text.compare(unit_start, std::string("秒").size(), "秒") == 0) {
    seconds_per_unit = 1;
    unit_length = std::string("秒").size();
    if (text.compare(unit_start, std::string("秒钟").size(), "秒钟") == 0)
      unit_length = std::string("秒钟").size();
  } else if (text.compare(unit_start, std::string("分钟").size(), "分钟") ==
             0) {
    seconds_per_unit = 60;
    unit_length = std::string("分钟").size();
  } else if (text.compare(unit_start, std::string("小时").size(), "小时") ==
             0) {
    seconds_per_unit = 3600;
    unit_length = std::string("小时").size();
  } else if (text.compare(unit_start, std::string("天").size(), "天") == 0) {
    seconds_per_unit = 86400;
    unit_length = std::string("天").size();
  } else {
    return false;
  }
  const std::size_t after_unit = unit_start + unit_length;
  if (text.compare(after_unit, std::string("后").size(), "后") != 0)
    return false;
  std::size_t content_start = text.find("提醒我", after_unit);
  std::size_t marker_length = std::string("提醒我").size();
  if (content_start == std::string::npos) {
    content_start = text.find("叫我", after_unit);
    marker_length = std::string("叫我").size();
  }
  if (content_start == std::string::npos)
    return false;
  content = text.substr(content_start + marker_length);
  content = StripCommandPrefix(content, {"再"});
  if (content.empty())
    return false;
  const long long seconds = static_cast<long long>(amount) * seconds_per_unit;
  if (seconds > 30LL * 86400LL)
    return false;
  delay_seconds = static_cast<int>(seconds);
  return true;
}

IntentResult Route(const std::string &label, const std::string &text) {
  IntentResult result;
  result.normalized_text = text;
  if (label == "music_play") {
    result.payload =
        MusicIntent{common::MusicAction::Play, ExtractMusicQuery(text)};
  } else if (label == "music_pause") {
    result.payload = MusicIntent{common::MusicAction::Pause, {}};
  } else if (label == "music_resume") {
    result.payload = MusicIntent{common::MusicAction::Resume, {}};
  } else if (label == "music_stop") {
    result.payload = MusicIntent{common::MusicAction::Stop, {}};
  } else if (label == "music_next") {
    result.payload = MusicIntent{common::MusicAction::Next, {}};
  } else if (label == "music_previous") {
    result.payload = MusicIntent{common::MusicAction::Previous, {}};
  } else if (label == "volume_get") {
    result.payload = DialogueIntent{common::IntentType::VolumeGet};
  } else if (label == "volume_set") {
    const int percent = ExtractVolume(text);
    result.payload =
        percent < 0
            ? IntentPayload{ChatIntent{}}
            : IntentPayload{VolumeIntent{common::VolumeAction::Set, percent}};
  } else if (label == "volume_up") {
    result.payload = VolumeIntent{common::VolumeAction::Up};
  } else if (label == "volume_down") {
    result.payload = VolumeIntent{common::VolumeAction::Down};
  } else if (label == "quiet_off") {
    result.payload = DialogueIntent{common::IntentType::QuietOff};
  } else if (label == "quiet_on") {
    result.payload = DialogueIntent{common::IntentType::QuietOn};
  } else if (label == "memory_list") {
    result.payload = MemoryIntent{common::MemoryOperation::ListFacts};
  } else if (label == "memory_forget") {
    const std::string content = ExtractMemoryContent(text, true);
    result.payload = content.empty()
                         ? IntentPayload{ChatIntent{}}
                         : IntentPayload{MemoryIntent{
                               common::MemoryOperation::ForgetFact, content}};
  } else if (label == "memory_save") {
    const std::string content = ExtractMemoryContent(text, false);
    result.payload = content.empty()
                         ? IntentPayload{ChatIntent{}}
                         : IntentPayload{MemoryIntent{
                               common::MemoryOperation::SaveFact, content}};
  } else if (label == "reminder_create") {
    int delay_seconds = 0;
    std::string content;
    result.payload =
        ExtractReminder(text, delay_seconds, content)
            ? IntentPayload{MemoryIntent{common::MemoryOperation::AddReminder,
                                         content, delay_seconds}}
            : IntentPayload{
                  DialogueIntent{common::IntentType::ReminderClarify}};
  } else if (label == "reminder_clarify") {
    result.payload = DialogueIntent{common::IntentType::ReminderClarify};
  } else {
    result.payload = ChatIntent{};
  }
  return result;
}

} // namespace

IntentClassifier::IntentClassifier(std::string model_path,
                                   double confidence_threshold,
                                   double correction_threshold)
    : feature_dimensions_(0), max_ngram_(0),
      confidence_threshold_(confidence_threshold),
      correction_threshold_(correction_threshold) {
  std::ifstream model_file(model_path);
  if (!model_file)
    throw std::runtime_error("Unable to open intent model: " + model_path);
  Json model;
  model_file >> model;
  if (model.at("version").get<int>() != 1 ||
      model.at("algorithm").get<std::string>() !=
          "hashed_character_ngram_softmax") {
    throw std::runtime_error("Unsupported intent model format: " + model_path);
  }

  feature_dimensions_ = model.at("feature_dimensions").get<std::size_t>();
  max_ngram_ = model.at("max_ngram").get<std::size_t>();
  labels_ = model.at("labels").get<std::vector<std::string>>();
  intent_weights_ = model.at("intent_weights").get<WeightMatrix>();
  correction_weights_ = model.at("correction_weights").get<WeightMatrix>();
  if (feature_dimensions_ == 0 || max_ngram_ == 0 || labels_.empty() ||
      intent_weights_.size() != labels_.size() ||
      correction_weights_.size() != 2) {
    throw std::runtime_error("Invalid intent model dimensions: " + model_path);
  }
  const std::size_t expected_weight_count = feature_dimensions_ + 1;
  for (const auto &weights : intent_weights_) {
    if (weights.size() != expected_weight_count)
      throw std::runtime_error("Invalid intent class weights: " + model_path);
  }
  for (const auto &weights : correction_weights_) {
    if (weights.size() != expected_weight_count)
      throw std::runtime_error("Invalid correction weights: " + model_path);
  }
}

std::vector<std::pair<std::size_t, double>>
IntentClassifier::FeatureVector(const std::string &text) const {
  std::vector<std::string> codepoints{"^"};
  for (const auto &[token, codepoint] : DecodeUtf8(text)) {
    if (!IsWhitespace(codepoint))
      codepoints.push_back(token);
  }
  codepoints.emplace_back("$");

  std::unordered_map<std::size_t, double> counts;
  for (std::size_t length = 1; length <= max_ngram_; ++length) {
    if (length > codepoints.size())
      break;
    for (std::size_t start = 0; start + length <= codepoints.size(); ++start) {
      std::string ngram;
      for (std::size_t index = start; index < start + length; ++index)
        ngram += codepoints[index];
      const std::size_t feature = FeatureHash(ngram) % feature_dimensions_;
      counts[feature] += 1.0;
    }
  }

  double squared_norm = 0.0;
  for (const auto &[index, count] : counts) {
    (void)index;
    squared_norm += count * count;
  }
  const double norm = squared_norm == 0.0 ? 1.0 : std::sqrt(squared_norm);
  std::vector<std::pair<std::size_t, double>> features;
  features.reserve(counts.size());
  for (const auto &[index, count] : counts)
    features.emplace_back(index, count / norm);
  return features;
}

std::vector<double> IntentClassifier::Scores(
    const WeightMatrix &weights,
    const std::vector<std::pair<std::size_t, double>> &features) const {
  std::vector<double> logits(weights.size(), 0.0);
  for (std::size_t class_index = 0; class_index < weights.size();
       ++class_index) {
    for (const auto &[feature, value] : features)
      logits[class_index] += weights[class_index][feature] * value;
    logits[class_index] += weights[class_index][feature_dimensions_];
  }
  return Softmax(logits);
}

IntentPrediction IntentClassifier::Classify(std::string text) const {
  IntentPrediction prediction;
  const std::string normalized_text = RemoveWhitespace(text);
  prediction.result.normalized_text = normalized_text;
  if (normalized_text.empty())
    return prediction;

  const auto features = FeatureVector(normalized_text);
  const auto intent_probabilities = Scores(intent_weights_, features);
  const auto best_iterator = std::max_element(intent_probabilities.begin(),
                                              intent_probabilities.end());
  const std::size_t best_index = static_cast<std::size_t>(
      std::distance(intent_probabilities.begin(), best_iterator));
  prediction.confidence = *best_iterator;
  prediction.label = prediction.confidence >= confidence_threshold_
                         ? labels_[best_index]
                         : "chat";
  const auto correction_probabilities = Scores(correction_weights_, features);
  prediction.correction_confidence =
      correction_probabilities.size() == 2 ? correction_probabilities[1] : 0.0;
  prediction.is_correction =
      prediction.confidence >= confidence_threshold_ &&
      prediction.correction_confidence >= correction_threshold_;
  prediction.result = Route(prediction.label, normalized_text);
  return prediction;
}

} // namespace xiaocai_robot
