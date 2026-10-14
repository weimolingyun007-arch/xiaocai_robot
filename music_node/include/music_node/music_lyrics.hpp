#pragma once

#include <string>
#include <vector>

namespace xiaocai_robot {

struct MusicTrack {
  std::string song_id;
  std::string title;
  std::string artist;
};

struct MusicLyricLine {
  double start_seconds{0.0};
  std::string text;
};

struct MusicLyrics {
  MusicTrack track;
  bool loading{false};
  std::vector<MusicLyricLine> lines;
};

class MusicLyricsParser {
public:
  static std::vector<MusicLyricLine> Parse(const std::string &lrc);
};

} // namespace xiaocai_robot
