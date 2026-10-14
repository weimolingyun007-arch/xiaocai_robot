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

MusicTrack TrackFromSong(const json &song) {
  MusicTrack track{
      song.value("id", std::string{}), song.value("name", std::string{}), {}};
  if (song.contains("artists") && song["artists"].is_array()) {
    for (const auto &artist : song["artists"]) {
      if (!artist.is_object() || !artist.contains("name") ||
          !artist["name"].is_string())
        continue;
      if (!track.artist.empty())
        track.artist += " / ";
      track.artist += artist["name"].get<std::string>();
    }
  }
  return track;
}
} // namespace

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
                                 SpeechCallback speech_callback,
                                 LoginCallback login_callback)
    : settings_(std::move(settings)), log_manager_(log_manager),
      status_callback_(std::move(status_callback)),
      speech_callback_(std::move(speech_callback)),
      login_callback_(std::move(login_callback)), stop_(false),
      silenced_(false), voice_playing_(false), busy_(false),
      last_voice_at_(0.0), last_conversation_at_(0.0), awake_(false),
      conversation_active_(false), resume_after_dialogue_(false),
      manually_paused_(true), revision_(0), backend_fd_(-1) {
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
  resume_after_dialogue_ = false;
  manually_paused_ = action == common::MusicAction::Pause ||
                     action == common::MusicAction::Stop;
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
    resume_after_dialogue_ = false;
    manually_paused_ = true;
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
    }
    last_voice_at_ = Monotonic();
    common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
  }
}

void MusicController::HandleVoicePlaying(bool playing) {
  voice_playing_ = playing;
  last_voice_at_ = Monotonic();
}

void MusicController::HandleSpeechAwake(bool awake) {
  awake_ = awake;
  last_conversation_at_ = Monotonic();
  if (!awake)
    return;
  silenced_ = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!manually_paused_ && !busy_ &&
        (status_.state == State::Playing || status_.state == State::Paused))
      resume_after_dialogue_ = true;
  }
  if (busy_) {
    ++revision_;
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.clear();
  }
  common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
}

void MusicController::HandleConversation(common::ConversationPhase phase) {
  conversation_active_ = phase != common::ConversationPhase::Idle;
  last_conversation_at_ = Monotonic();
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
  if (busy_) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_.state == State::Searching)
      return;
  }
  if (resume_after_dialogue_ && !awake_ && !conversation_active_ &&
      !voice_playing_ && !silenced_ && Monotonic() - last_voice_at_ > 0.8 &&
      Monotonic() - last_conversation_at_ > 0.8) {
    if (idle.value("data", true)) {
      resume_after_dialogue_ = false;
    } else if (common::MpvOk(common::MpvCommand(
                   {"set_property", "pause", false}, settings_.mpv_socket))) {
      resume_after_dialogue_ = false;
      LOG_INFO("Dialogue finished; music resumed at the saved position");
    }
  }
  auto pause =
      common::MpvCommand({"get_property", "pause"}, settings_.mpv_socket);
  if (!common::MpvOk(pause))
    return;
  auto position =
      common::MpvCommand({"get_property", "time-pos"}, settings_.mpv_socket);
  auto duration =
      common::MpvCommand({"get_property", "duration"}, settings_.mpv_socket);
  auto playlist_position = common::MpvCommand({"get_property", "playlist-pos"},
                                              settings_.mpv_socket);
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
  if (common::MpvOk(playlist_position) &&
      playlist_position["data"].is_number_integer()) {
    const auto index = playlist_position["data"].get<std::int64_t>();
    if (index >= 0 && static_cast<std::size_t>(index) < playlist_.size()) {
      const auto &track = playlist_[static_cast<std::size_t>(index)];
      status_.song_id = track.song_id;
      status_.title = track.title;
      status_.artist = track.artist;
    }
  }
  if (status_.state == State::Stopped) {
    status_.song_id.clear();
    status_.title.clear();
    status_.artist.clear();
  }
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
    auto search = RunCliCommand(
        {"search", "song", "--keyword", keyword, "--output", "json"},
        job.revision);
    if (!IsJobCurrent(job.revision))
      return;
    if (!search.ok || !search.data.is_object() ||
        search.data.value("code", 0) != 200 || !search.data.contains("data") ||
        !search.data["data"].is_object() ||
        !search.data["data"].contains("records") ||
        !search.data["data"]["records"].is_array()) {
      const int api_code = search.data.is_object() &&
                                   search.data.contains("code") &&
                                   search.data["code"].is_number_integer()
                               ? search.data["code"].get<int>()
                               : -1;
      LOG_WARN("Music search failed: exit_code={} timed_out={} api_code={} "
               "output_bytes={}",
               search.exit_code, search.timed_out, api_code,
               search.output.size());
      if (search.output.find("unknown command") != std::string::npos ||
          api_code == 401 || api_code == 403 || api_code == 301) {
        const auto login = RunCliCommand(
            {"login", "--check", "--output", "json"}, job.revision);
        if (!IsJobCurrent(job.revision))
          return;
        const auto login_message =
            login.data.is_object() && login.data.contains("message") &&
                    login.data["message"].is_string()
                ? login.data["message"].get<std::string>()
                : std::string{};
        const bool expired =
            login_message.find("未登录") != std::string::npos ||
            login_message.find("过期") != std::string::npos ||
            login_message.find("失效") != std::string::npos;
        if (expired && login.data.contains("success") &&
            login.data["success"].is_boolean() &&
            !login.data["success"].get<bool>()) {
          SetPlaybackStatus(State::Error,
                            "网易云登录已失效，请扫描屏幕二维码登录。",
                            job.request_id);
          if (login_callback_)
            login_callback_();
          else
            speech_callback_("网易云登录已失效，请扫描屏幕二维码登录。");
          return;
        }
        LOG_WARN("ncm-cli search command unavailable or authorization "
                 "rejected; login check exit_code={}",
                 login.exit_code);
      }
      FailJob(job, search.timed_out ? "网易云搜索超时，请稍后再试。"
                                    : "网易云搜索暂时没成功，请稍后再试。");
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
        if (common::MpvOk(idle) && !idle.value("data", true)) {
          const auto audio = common::MpvCommand(
              {"get_property", "audio-out-params"}, settings_.mpv_socket);
          if (common::MpvOk(audio) && audio.contains("data") &&
              audio["data"].is_object())
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      return false;
    };
    // The CLI starts a detached controller before song lookup and MPV startup
    // complete. Keep its daemon subscribed and allow the compatibility
    // adapter's 20-second request budget before considering a retry.
    // Audio output initialization, not the detached CLI's exit code, confirms
    // that the track loaded.
    bool loaded = wait_for_track(25.0);
    if (!loaded && IsJobCurrent(job.revision)) {
      LOG_WARN(
          "ncm-cli did not load the track on first attempt; retrying once");
      LOG_WARN("First ncm-cli play: exit_code={} timed_out={}", play.exit_code,
               play.timed_out);
      play = RunCliCommand(play_args, job.revision);
      if (!IsJobCurrent(job.revision)) {
        common::MpvCommand({"stop"}, settings_.mpv_socket);
        return;
      }
      loaded = wait_for_track(25.0);
    }
    if (!loaded) {
      if (!play.ok ||
          (play.data.is_object() && !play.data.value("success", false)))
        LOG_WARN("ncm-cli play failed: exit_code={} timed_out={}",
                 play.exit_code, play.timed_out);
      common::MpvCommand({"stop"}, settings_.mpv_socket);
      FailJob(job, "网易云后台两次都没有加载出歌曲，请稍后再试。");
      return;
    }
    std::vector<MusicTrack> playlist{TrackFromSong(*selected)};
    common::MpvCommand({"set_property", "audio-device", settings_.audio_device},
                       settings_.mpv_socket);
    {
      auto idle = common::MpvCommand({"get_property", "idle-active"},
                                     settings_.mpv_socket);
      if (common::MpvOk(idle) && !idle.value("data", true)) {
        // Start the selected track immediately. Filling the follow-up queue
        // performs network requests and must not keep the first song paused.
        // Repeat the current track until the complete playlist is prepared.
        if (!common::MpvOk(common::MpvCommand(
                {"set_property", "loop-file", "inf"}, settings_.mpv_socket))) {
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
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          playlist_ = playlist;
          status_.song_id = playlist_.front().song_id;
          status_.title = playlist_.front().title;
          status_.artist = playlist_.front().artist;
        }
        SetPlaybackStatus(State::Playing, {}, job.request_id);
        LOG_INFO("MusicNode playback started: {}",
                 selected->value("name", std::string{}));
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
            playlist.push_back(TrackFromSong(song));
            std::lock_guard<std::mutex> lock(state_mutex_);
            playlist_ = playlist;
          } else {
            LOG_WARN("Could not queue next search result: exit_code={} "
                     "timed_out={}",
                     added.exit_code, added.timed_out);
          }
        }
        const auto loop = common::MpvCommand(
            {"set_property", "loop-playlist", "inf"}, settings_.mpv_socket);
        if (common::MpvOk(loop))
          common::MpvCommand({"set_property", "loop-file", "no"},
                             settings_.mpv_socket);
        else
          LOG_WARN("Could not loop the playlist; keeping current track loop");
        LOG_INFO("Music queue ready: {} follow-up tracks", queued);
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
  if (state == State::Stopped || state == State::Searching ||
      state == State::Error) {
    status_.song_id.clear();
    status_.title.clear();
    status_.artist.clear();
    status_.position_seconds = 0;
    status_.duration_seconds = 0;
  }
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
  const int exit_code = WIFEXITED(status)     ? WEXITSTATUS(status)
                        : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                              : -1;
  return {!cancelled && exit_code == 0, data, text, exit_code,
          cancelled && IsJobCurrent(revision)};
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
