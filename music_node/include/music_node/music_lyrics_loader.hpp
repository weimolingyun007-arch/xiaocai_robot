#pragma once

#include "common/log_manager.hpp"
#include "music_node/music_lyrics.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace xiaocai_robot {

class MusicLyricsLoader {
public:
  using LyricsCallback = std::function<void(const MusicLyrics &)>;

  MusicLyricsLoader(std::string cli_path, LogManager &log_manager,
                    LyricsCallback callback);
  ~MusicLyricsLoader();
  void HandleTrack(const MusicTrack &track);

private:
  // Asynchronous retrieval and cancellation.
  void WorkerLoop();
  std::string FetchLyrics(const std::string &song_id, std::uint64_t generation);

  std::string cli_path_;
  LogManager &log_manager_;
  LyricsCallback callback_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::atomic<bool> stop_;
  std::atomic<std::uint64_t> generation_;
  bool pending_;
  MusicTrack track_;
  std::thread worker_;
  std::unordered_map<std::string, std::vector<MusicLyricLine>> cache_;
  std::deque<std::string> cache_order_;
};

} // namespace xiaocai_robot
