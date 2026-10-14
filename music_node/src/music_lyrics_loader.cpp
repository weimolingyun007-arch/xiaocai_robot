#include "music_node/music_lyrics_loader.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace xiaocai_robot {

MusicLyricsLoader::MusicLyricsLoader(std::string cli_path,
                                     LogManager &log_manager,
                                     LyricsCallback callback)
    : cli_path_(std::move(cli_path)), log_manager_(log_manager),
      callback_(std::move(callback)), stop_(false), generation_(0),
      pending_(false) {
  worker_ = std::thread([this] { WorkerLoop(); });
}

MusicLyricsLoader::~MusicLyricsLoader() {
  stop_ = true;
  ++generation_;
  ready_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void MusicLyricsLoader::HandleTrack(const MusicTrack &track) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (track.song_id == track_.song_id)
    return;
  track_ = track;
  ++generation_;
  pending_ = !track.song_id.empty();
  callback_({track, pending_, {}});
  ready_.notify_one();
}

void MusicLyricsLoader::WorkerLoop() {
  while (!stop_) {
    MusicTrack track;
    std::uint64_t generation;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return stop_ || pending_; });
      if (stop_)
        return;
      track = track_;
      generation = generation_;
      pending_ = false;
    }
    MusicLyrics lyrics{track, false, {}};
    bool cacheable = false;
    try {
      const auto cached = cache_.find(track.song_id);
      if (cached != cache_.end()) {
        lyrics.lines = cached->second;
      } else {
        const auto response = nlohmann::json::parse(
            FetchLyrics(track.song_id, generation), nullptr, false);
        if (!response.is_object() || response.value("code", 0) != 200 ||
            !response.contains("data") || !response["data"].is_object())
          throw std::runtime_error("invalid lyric response");
        const auto &data = response["data"];
        if (data.contains("lyric") && data["lyric"].is_string())
          lyrics.lines =
              MusicLyricsParser::Parse(data["lyric"].get<std::string>());
        cacheable = true;
      }
    } catch (const std::exception &error) {
      if (!stop_ && generation == generation_)
        LOG_WARN("Could not load lyrics for {}: {}", track.title, error.what());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_ || generation != generation_)
      continue;
    if (cacheable) {
      if (cache_order_.size() >= 16) {
        cache_.erase(cache_order_.front());
        cache_order_.pop_front();
      }
      cache_[track.song_id] = lyrics.lines;
      cache_order_.push_back(track.song_id);
    }
    LOG_INFO("Lyrics ready for {}: {} timed lines", track.title,
             lyrics.lines.size());
    callback_(lyrics);
  }
}

std::string MusicLyricsLoader::FetchLyrics(const std::string &song_id,
                                           std::uint64_t generation) {
  int output[2];
  if (pipe2(output, O_CLOEXEC) != 0)
    throw std::runtime_error("cannot create lyric command pipe");
  std::vector<std::string> arguments{cli_path_, "song",     "lyric", "--songId",
                                     song_id,   "--output", "json"};
  std::vector<char *> argv;
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, output[0]);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  pid_t pid;
  const int error =
      posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(output[1]);
  if (error != 0) {
    close(output[0]);
    throw std::runtime_error("cannot start lyric command");
  }
  fcntl(output[0], F_SETFL, O_NONBLOCK);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  std::string text;
  int status = 0;
  bool exited = false;
  bool cancelled = false;
  auto drain = [&] {
    char buffer[4096];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) {
      if (text.size() + static_cast<std::size_t>(count) <= 2 * 1024 * 1024)
        text.append(buffer, count);
      else
        cancelled = true;
    }
  };
  while (!exited) {
    drain();
    const auto waited = waitpid(pid, &status, WNOHANG);
    exited = waited == pid || (waited < 0 && errno != EINTR);
    if (!exited && (cancelled || stop_ || generation != generation_ ||
                    std::chrono::steady_clock::now() >= deadline)) {
      cancelled = true;
      kill(-pid, SIGTERM);
      const auto grace =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      while (waitpid(pid, &status, WNOHANG) == 0 &&
             std::chrono::steady_clock::now() < grace)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      kill(-pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      exited = true;
    }
    if (!exited)
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  drain();
  close(output[0]);
  if (cancelled || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("lyric command failed or was cancelled");
  const auto begin = text.find('{');
  const auto end = text.rfind('}');
  return begin != std::string::npos && end != std::string::npos && end >= begin
             ? text.substr(begin, end - begin + 1)
             : "";
}

} // namespace xiaocai_robot
