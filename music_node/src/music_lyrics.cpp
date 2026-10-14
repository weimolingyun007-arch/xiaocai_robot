#include "music_node/music_lyrics.hpp"

#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>

namespace xiaocai_robot {

std::vector<MusicLyricLine> MusicLyricsParser::Parse(const std::string &lrc) {
  static const std::regex kOffset(R"(\[offset:\s*([+-]?\d{1,8})\s*\])",
                                  std::regex::icase);
  static const std::regex kTimestamp(
      R"(\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\])");
  std::smatch offset_match;
  const double offset =
      std::regex_search(lrc, offset_match, kOffset)
          ? std::clamp(std::stod(offset_match[1]), -60000.0, 60000.0) / 1000.0
          : 0.0;
  std::vector<MusicLyricLine> result;
  std::istringstream input(lrc);
  std::string line;
  while (std::getline(input, line) && result.size() < 5000) {
    if (line.compare(0, 3, "\xef\xbb\xbf") == 0)
      line.erase(0, 3);
    std::vector<double> starts;
    std::smatch match;
    while (std::regex_search(line, match, kTimestamp,
                             std::regex_constants::match_continuous)) {
      const int seconds = std::stoi(match[2]);
      if (seconds < 60) {
        const std::string fraction = match[3];
        starts.push_back(
            std::stod(match[1]) * 60.0 + seconds +
            (fraction.empty()
                 ? 0.0
                 : std::stod(fraction) / std::pow(10.0, fraction.size())) +
            offset);
      }
      line.erase(0, match.length());
    }
    const auto first = line.find_first_not_of(" \t\r");
    line = first == std::string::npos
               ? ""
               : line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    for (double start : starts) {
      if (result.size() >= 5000)
        break;
      result.push_back({std::max(0.0, start), line});
    }
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const auto &left, const auto &right) {
                     return left.start_seconds < right.start_seconds;
                   });
  std::vector<MusicLyricLine> unique;
  for (auto &item : result) {
    if (!unique.empty() &&
        std::abs(unique.back().start_seconds - item.start_seconds) < 0.000001) {
      if (!item.text.empty())
        unique.back().text = std::move(item.text);
    } else {
      unique.push_back(std::move(item));
    }
  }
  return unique;
}

} // namespace xiaocai_robot
