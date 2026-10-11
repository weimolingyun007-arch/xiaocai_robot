#pragma once
#include <optional>
#include <regex>
#include <string>

struct MusicIntent {
  std::string intent;
  std::string content;
};

inline std::optional<MusicIntent> parse_music_intent(const std::string &text) {
  const std::pair<const char *, const char *> controls[] = {
    {"^(?:请|帮我|给我)?(?:暂停|先暂停)(?:一下)?(?:音乐|歌曲|播放|音乐播放)?$", "music_pause"},
    {"^(?:请|帮我|给我)?(?:继续|恢复)(?:一下)?(?:播放|放歌|音乐|歌曲|播放音乐|播放歌曲)?$", "music_resume"},
    {"^(?:请|帮我|给我)?(?:停止|停止播放|关掉|关闭|别放|不要放|不要播放)(?:音乐|歌曲|放歌|播放|歌)?(?:了)?$", "music_stop"},
    {"^(?:请|帮我|给我)?(?:下一首|切歌|换一首|换首歌)(?:歌|歌曲)?$", "music_next"},
    {"^(?:请|帮我|给我)?(?:上一首|前一首)(?:歌|歌曲)?$", "music_previous"}
  };
  for (const auto &control : controls)
    if (std::regex_match(text, std::regex(control.first))) return MusicIntent{control.second, {}};
  std::smatch match;
  if (!std::regex_match(text, match, std::regex(
      "^(?:请你|请|帮我|给我)?(?:播放|放一首|放首|来一首|我想听一首|我想听|听一首|放音乐|放歌)(.*)$")))
    return std::nullopt;
  std::string song = match[1];
  if (song.rfind("一首", 0) == 0) song.erase(0, 6);
  if (song.size() >= 3 && song.compare(song.size() - 3, 3, "吧") == 0) song.resize(song.size() - 3);
  for (const auto &mark : {std::string("《"), std::string("》")}) {
    size_t position;
    while ((position = song.find(mark)) != std::string::npos) song.erase(position, mark.size());
  }
  if (std::regex_match(song, std::regex("^(?:任意|随便)?(?:一首|首)?(?:音乐|歌|歌曲)?$"))) song.clear();
  return MusicIntent{"music_play", song};
}
