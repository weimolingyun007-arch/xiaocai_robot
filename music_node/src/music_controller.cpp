#include "music_node/music_controller.hpp"
#include "common/mpv.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace xiaocai_robot {

namespace {
using State = common::MusicState;
}

namespace {
bool SendLine(int fd, const std::string &line) {
  std::size_t sent = 0;
  while (sent < line.size()) {
    const auto count =
        ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReceiveLine(int fd, std::string &line, int timeout_ms) {
  line.clear();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline && line.size() < 8192) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1,
                             static_cast<int>(std::max<std::int64_t>(1, left)));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) ||
        ((descriptor.revents & POLLHUP) && !(descriptor.revents & POLLIN)))
      return false;
    char value = 0;
    const auto count = ::recv(fd, &value, 1, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    if (value == '\n')
      return true;
    line.push_back(value);
  }
  return false;
}

int ConnectUnix(const std::string &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
} // namespace

MusicController::MusicController(MusicSettings settings,
                                 LogManager &log_manager,
                                 StatusCallback status_callback,
                                 SpeechCallback speech_callback)
    : settings_(std::move(settings)), log_manager_(log_manager),
      status_callback_(std::move(status_callback)),
      speech_callback_(std::move(speech_callback)) {
  SetPlaybackStatus(State::Stopped);
  worker_ = std::thread([this] { WorkerLoop(); });
}

MusicController::~MusicController() {
  stop_ = true;
  ++revision_;
  ready_.notify_all();
  if (worker_.joinable())
    worker_.join();
  common::MpvCommand({"stop"}, settings_.mpv_socket);
  CloseBackendSubscription();
}

void MusicController::HandleMusicCommand(common::MusicAction action,
                                         const std::string &query,
                                         std::uint64_t request_id) {
  if (!common::IsMusicAction(action) || silenced_)
    return;
  const auto revision = ++revision_;
  if (action == common::MusicAction::Stop ||
      action == common::MusicAction::Pause) {
    common::MpvCommand(action == common::MusicAction::Stop
                           ? json{"stop"}
                           : json{"set_property", "pause", true},
                       settings_.mpv_socket);
  }
  {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.clear();
    jobs_.push_back({action, query, revision, request_id});
  }
  ready_.notify_one();
  LOG_INFO("MusicNode action: {}; query: {}", common::MusicActionName(action),
           query);
}

void MusicController::HandleAudioPlay(const std::string &path) {
  if (path.empty()) {
    silenced_ = true;
    ++revision_;
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.clear();
    }
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    CloseBackendSubscription();
    SetPlaybackStatus(State::Stopped);
    LOG_INFO("Hush: music stopped and pending requests cancelled");
  } else {
    if (path == settings_.wake_ack_path) {
      silenced_ = false;
      ++revision_;
      {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_.clear();
      }
      common::MpvCommand({"stop"}, settings_.mpv_socket);
      CloseBackendSubscription();
      SetPlaybackStatus(State::Stopped);
      LOG_INFO("Wake acknowledged: continuous music playback ended");
      return;
    }
    last_voice_at_ = Monotonic();
    common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
  }
}

void MusicController::HandleVoicePlaying(bool playing) {
  voice_playing_ = playing;
  last_voice_at_ = Monotonic();
}

void MusicController::HandleStateUpdate() {
  MaintainBackendSubscription();
  auto idle =
      common::MpvCommand({"get_property", "idle-active"}, settings_.mpv_socket);
  if (!common::MpvOk(idle))
    return;
  if (silenced_ && !idle.value("data", true)) {
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    SetPlaybackStatus(State::Stopped);
    return;
  }
  auto device = common::MpvCommand({"get_property", "audio-device"},
                                   settings_.mpv_socket);
  if (common::MpvOk(device) &&
      device.value("data", std::string{}) != settings_.audio_device)
    common::MpvCommand({"set_property", "audio-device", settings_.audio_device},
                       settings_.mpv_socket);
  if (busy_)
    return;
  auto pause =
      common::MpvCommand({"get_property", "pause"}, settings_.mpv_socket);
  if (!common::MpvOk(pause))
    return;
  auto position =
      common::MpvCommand({"get_property", "time-pos"}, settings_.mpv_socket);
  auto duration =
      common::MpvCommand({"get_property", "duration"}, settings_.mpv_socket);
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (status_.state == State::Error && idle.value("data", true))
    return;
  status_.state = idle.value("data", true)    ? State::Stopped
                  : pause.value("data", true) ? State::Paused
                                              : State::Playing;
  status_.position_seconds =
      common::MpvOk(position) && position["data"].is_number()
          ? position["data"].get<double>()
          : 0;
  status_.duration_seconds =
      common::MpvOk(duration) && duration["data"].is_number()
          ? duration["data"].get<double>()
          : 0;
  status_callback_(status_);
}

void MusicController::WorkerLoop() {
  while (!stop_) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(jobs_mutex_);
      ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_)
        return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    if (!IsJobCurrent(job.revision))
      continue;
    busy_ = true;
    try {
      ExecuteJob(job);
    } catch (const std::exception &error) {
      LOG_ERROR("MusicNode command failed: {}", error.what());
      FailJob(job, "音乐服务暂时出了点问题，请再试一次。");
    }
    busy_ = false;
  }
}

void MusicController::ExecuteJob(const Job &job) {
  using Action = common::MusicAction;
  if (job.action == Action::Play) {
    SetPlaybackStatus(State::Searching, {}, job.request_id);
    common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
    const std::string keyword =
        job.query.empty() ? settings_.default_keyword : job.query;
    auto search =
        RunCliCommand({"search", "song", "--keyword", keyword}, job.revision);
    if (!IsJobCurrent(job.revision))
      return;
    if (!search.ok || !search.data.is_object() ||
        search.data.value("code", 0) != 200 || !search.data.contains("data") ||
        !search.data["data"].is_object() ||
        !search.data["data"].contains("records") ||
        !search.data["data"]["records"].is_array()) {
      FailJob(job, "网易云搜索暂时没成功，请稍后再试。");
      return;
    }
    const auto &songs = search.data["data"]["records"];
    const json *selected = nullptr;
    for (const auto &song : songs) {
      if (!song.is_object() || !song.value("playFlag", false) ||
          !song.contains("originalId") ||
          song.value("id", std::string{}).empty())
        continue;
      if (!selected)
        selected = &song;
      if (song.value("name", std::string{}) == keyword) {
        selected = &song;
        break;
      }
    }
    if (!selected) {
      FailJob(job, "没有找到当前账号可以播放的歌曲，请换一首。");
      return;
    }
    if (!WaitForSpeechToFinish(job.revision))
      return;
    // Prevent old songs from being resumed by mpv when replacing the queue
    // entry.
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    const std::vector<std::string> play_args{
        "play",           "--song",
        "--encrypted-id", selected->value("id", std::string{}),
        "--original-id",  (*selected)["originalId"].dump(),
        "--output",       "json"};
    auto play = RunCliCommand(play_args, job.revision);
    if (!IsJobCurrent(job.revision)) {
      common::MpvCommand({"stop"}, settings_.mpv_socket);
      return;
    }
    if (!EnsureBackendSubscription(job.revision)) {
      common::MpvCommand({"stop"}, settings_.mpv_socket);
      FailJob(job, "网易云播放器后台没有保持连接，请稍后再试。");
      return;
    }

    auto wait_for_track = [&](double seconds) {
      const double until = Monotonic() + seconds;
      while (IsJobCurrent(job.revision) && Monotonic() < until) {
        auto idle = common::MpvCommand({"get_property", "idle-active"},
                                       settings_.mpv_socket);
        if (common::MpvOk(idle) && !idle.value("data", true))
          return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      return false;
    };
    // A cold ncm-cli daemon can miss its first 3-second playback response.
    // Keep the daemon subscribed, then retry once without requiring a second
    // user utterance. MPV state, not the CLI exit code, decides whether it
    // worked.
    bool loaded = wait_for_track(6.0);
    if (!loaded && IsJobCurrent(job.revision)) {
      LOG_WARN(
          "ncm-cli did not load the track on first attempt; retrying once");
      if (!play.output.empty())
        LOG_WARN("First ncm-cli play output: {:.400}", play.output);
      play = RunCliCommand(play_args, job.revision);
      if (!IsJobCurrent(job.revision)) {
        common::MpvCommand({"stop"}, settings_.mpv_socket);
        return;
      }
      loaded = wait_for_track(12.0);
    }
    if (!loaded) {
      if (!play.ok ||
          (play.data.is_object() && !play.data.value("success", false)))
        LOG_WARN("ncm-cli play failed: {:.400}", play.output);
      common::MpvCommand({"stop"}, settings_.mpv_socket);
      FailJob(job, "网易云后台两次都没有加载出歌曲，请稍后再试。");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      status_.title = selected->value("name", std::string{});
    }
    common::MpvCommand({"set_property", "audio-device", settings_.audio_device},
                       settings_.mpv_socket);
    {
      auto idle = common::MpvCommand({"get_property", "idle-active"},
                                     settings_.mpv_socket);
      if (common::MpvOk(idle) && !idle.value("data", true)) {
        // Hold the first track while preparing the queue so short tracks cannot
        // end before their follow-up entries have been added.
        common::MpvCommand({"set_property", "pause", true},
                           settings_.mpv_socket);
        // `play --song` replaces the ncm-cli queue with just the selected
        // track. Append playable search results, then loop the complete queue
        // forever.
        bool after_selected = false;
        size_t queued = 0;
        size_t attempts = 0;
        for (const auto &song : songs) {
          if (&song == selected) {
            after_selected = true;
            continue;
          }
          if (!after_selected || attempts >= 5 || !song.is_object() ||
              !song.value("playFlag", false) || !song.contains("id") ||
              !song.contains("originalId"))
            continue;
          const auto encrypted_id = song.value("id", std::string{});
          if (encrypted_id.empty() ||
              encrypted_id == selected->value("id", std::string{}))
            continue;
          ++attempts;
          auto added = RunCliCommand(
              {"queue", "add", "--encrypted-id", encrypted_id, "--original-id",
               song["originalId"].dump(), "--output", "json"},
              job.revision);
          if (!IsJobCurrent(job.revision))
            return;
          if (added.ok && added.data.is_object() &&
              added.data.value("success", false)) {
            ++queued;
          } else {
            LOG_WARN("Could not queue next search result ({}): {:.240}",
                     song.value("name", std::string{}), added.output);
          }
        }
        auto loop = common::MpvCommand({"set_property", "loop-playlist", "inf"},
                                       settings_.mpv_socket);
        if (!common::MpvOk(loop)) {
          LOG_WARN("Could not enable continuous playlist loop");
          loop = common::MpvCommand({"set_property", "loop-file", "inf"},
                                    settings_.mpv_socket);
        }
        if (!common::MpvOk(loop)) {
          common::MpvCommand({"stop"}, settings_.mpv_socket);
          FailJob(job, "网易云播放器无法保持连续播放，请稍后再试。");
          return;
        }
        if (!voice_playing_ &&
            !common::MpvOk(common::MpvCommand({"set_property", "pause", false},
                                              settings_.mpv_socket))) {
          common::MpvCommand({"stop"}, settings_.mpv_socket);
          FailJob(job, "音乐已加载，但无法从扬声器开始播放。");
          return;
        }
        bool playing = false;
        const double verify_until = Monotonic() + 2.0;
        while (IsJobCurrent(job.revision) && Monotonic() < verify_until) {
          auto active = common::MpvCommand({"get_property", "idle-active"},
                                           settings_.mpv_socket);
          auto paused = common::MpvCommand({"get_property", "pause"},
                                           settings_.mpv_socket);
          if (common::MpvOk(active) && common::MpvOk(paused) &&
              !active.value("data", true) && !paused.value("data", true)) {
            playing = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!playing) {
          common::MpvCommand({"stop"}, settings_.mpv_socket);
          FailJob(job, "音乐没有从扬声器真正播放出来，请稍后再试。");
          return;
        }
        SetPlaybackStatus(State::Playing, {}, job.request_id);
        LOG_INFO("MusicNode playback started continuously: {}; queued {} "
                 "follow-up tracks",
                 selected->value("name", std::string{}), queued);
        return;
      }
    }
    FailJob(job, "音乐加载后播放器状态丢失，请稍后再试。");
  } else if (job.action == Action::Resume) {
    if (!WaitForSpeechToFinish(job.revision))
      return;
    auto idle = common::MpvCommand({"get_property", "idle-active"},
                                   settings_.mpv_socket);
    if (!common::MpvOk(idle) || idle.value("data", true)) {
      FailJob(job, "还没有可继续播放的音乐，请先点歌。");
      return;
    }
    if (!common::MpvOk(common::MpvCommand({"set_property", "pause", false},
                                          settings_.mpv_socket)))
      FailJob(job, "音乐暂时无法继续播放。");
    else
      SetPlaybackStatus(State::Playing, {}, job.request_id);
  } else if (job.action == Action::Pause) {
    SetPlaybackStatus(State::Paused, {}, job.request_id);
  } else if (job.action == Action::Stop) {
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    SetPlaybackStatus(State::Stopped, {}, job.request_id);
  } else if (job.action == Action::Next || job.action == Action::Previous) {
    if (!WaitForSpeechToFinish(job.revision))
      return;
    auto result = RunCliCommand({job.action == Action::Next ? "next" : "prev"},
                                job.revision);
    if (!IsJobCurrent(job.revision))
      return;
    if (!result.ok || !result.data.is_object() ||
        !result.data.value("success", false))
      FailJob(job, "没有可切换的歌曲，请点一首新的歌。");
    else if (common::MpvOk(common::MpvCommand({"set_property", "pause", false},
                                              settings_.mpv_socket)))
      SetPlaybackStatus(State::Playing, {}, job.request_id);
    else
      FailJob(job, "音乐暂时无法切换播放。");
  }
}

bool MusicController::IsJobCurrent(std::uint64_t revision) const {
  return !stop_ && !silenced_ && revision_.load() == revision;
}

void MusicController::SetPlaybackStatus(State state, const std::string &error,
                                        std::uint64_t request_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  status_.state = state;
  status_.error = error;
  status_.request_id = request_id;
  status_callback_(status_);
}

bool MusicController::EnsureBackendSubscription(std::uint64_t revision) {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ >= 0)
    return true;

  std::string directory = settings_.mpv_socket;
  const auto slash = directory.find_last_of('/');
  directory = slash == std::string::npos ? "." : directory.substr(0, slash);
  const std::string control_path = directory + "/player-daemon.sock";
  const double deadline = Monotonic() + 5.0;
  while (IsJobCurrent(revision) && Monotonic() < deadline) {
    int subscription = ConnectUnix(control_path);
    if (subscription >= 0) {
      const std::string request = json{{"command", "subscribe"},
                                       {"role", "observer"},
                                       {"controllerType", "cli"}}
                                      .dump() +
                                  "\n";
      std::string snapshot;
      if (SendLine(subscription, request) &&
          ReceiveLine(subscription, snapshot, 1200)) {
        const auto event = Parse(snapshot);
        if (event.is_object() &&
            event.value("event", std::string{}) == "stateSnapshot") {
          const int flags = ::fcntl(subscription, F_GETFL, 0);
          if (flags >= 0)
            ::fcntl(subscription, F_SETFL, flags | O_NONBLOCK);
          backend_fd_ = subscription;
          LOG_INFO("Subscribed to ncm-cli player daemon state");
          return true;
        }
      }
      ::close(subscription);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void MusicController::MaintainBackendSubscription() {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ < 0)
    return;
  char buffer[4096];
  for (;;) {
    const auto count =
        ::recv(backend_fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0)
      continue;
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    ::close(backend_fd_);
    backend_fd_ = -1;
    LOG_WARN("ncm-cli player daemon subscription disconnected");
    return;
  }
}

void MusicController::CloseBackendSubscription() {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ >= 0)
    ::close(backend_fd_);
  backend_fd_ = -1;
}

MusicController::CliResult
MusicController::RunCliCommand(const std::vector<std::string> &arguments,
                               std::uint64_t revision) {
  int output[2];
  if (pipe2(output, O_CLOEXEC) != 0)
    return {};
  std::vector<std::string> args{settings_.cli_path};
  args.insert(args.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
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
    return {};
  }
  fcntl(output[0], F_SETFL, O_NONBLOCK);
  std::string text;
  const double until = Monotonic() + 30;
  int status = 0;
  bool exited = false, cancelled = false;
  while (!exited) {
    char buffer[4096];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
      if (text.size() < 2 * 1024 * 1024)
        text.append(buffer, count);
    exited = waitpid(pid, &status, WNOHANG) == pid;
    if (!exited && (!IsJobCurrent(revision) || Monotonic() > until)) {
      cancelled = true;
      kill(-pid, SIGTERM);
      const double grace = Monotonic() + 0.5;
      while (waitpid(pid, &status, WNOHANG) == 0 && Monotonic() < grace)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      kill(-pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      exited = true;
    }
    if (!exited)
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  char buffer[4096];
  ssize_t count;
  while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
    if (text.size() < 2 * 1024 * 1024)
      text.append(buffer, count);
  close(output[0]);
  json data = json::value_t::discarded;
  const auto begin = text.find('{');
  const auto end = text.rfind('}');
  if (begin != std::string::npos && end != std::string::npos && end >= begin)
    data = Parse(text.substr(begin, end - begin + 1));
  return {!cancelled && WIFEXITED(status) && WEXITSTATUS(status) == 0, data,
          text};
}

bool MusicController::WaitForSpeechToFinish(std::uint64_t revision) {
  const double until = Monotonic() + 20;
  while (IsJobCurrent(revision) && Monotonic() < until) {
    if (!voice_playing_ && Monotonic() - last_voice_at_.load() > 0.5)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void MusicController::FailJob(const Job &job, const std::string &message) {
  if (!IsJobCurrent(job.revision))
    return;
  SetPlaybackStatus(State::Error, message, job.request_id);
  if (job.request_id == 0)
    speech_callback_(message);
  LOG_WARN("{}", message);
}

} // namespace xiaocai_robot
