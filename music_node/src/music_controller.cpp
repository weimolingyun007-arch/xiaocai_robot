#include "music_node/music_controller.hpp"
#include "common/mpv.hpp"
#include <algorithm>
#include <cctype>
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

std::string SongTitleKey(std::string title) {
  // Versions of the same song must not occupy consecutive queue entries.
  for (const auto *mark : {"(", "（", "[", "【"}) {
    const auto position = title.find(mark);
    if (position != std::string::npos && position > 0)
      title.resize(position);
  }
  title.erase(std::remove_if(title.begin(), title.end(),
                             [](unsigned char c) { return std::isspace(c); }),
              title.end());
  std::transform(title.begin(), title.end(), title.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return title;
}

bool SameSongTitle(const std::string &left, const std::string &right) {
  const auto a = SongTitleKey(left);
  const auto b = SongTitleKey(right);
  return !a.empty() && !b.empty() && (a.rfind(b, 0) == 0 || b.rfind(a, 0) == 0);
}

bool IsPlayableSong(const json &song) {
  return song.is_object() && song.value("playFlag", false) &&
         song.contains("originalId") && !song["originalId"].is_null() &&
         !song.value("id", std::string{}).empty() &&
         !SongTitleKey(song.value("name", std::string{})).empty();
}

std::string OriginalSongId(const json &song) {
  return song["originalId"].is_string() ? song["originalId"].get<std::string>()
                                        : song["originalId"].dump();
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
  if (action == common::MusicAction::Play ||
      action == common::MusicAction::Next ||
      action == common::MusicAction::Previous) {
    common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
    SetPlaybackStatus(State::Searching, {}, request_id);
  }
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
  bool cancel_loading = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!manually_paused_ &&
        (status_.state == State::Playing || status_.state == State::Paused))
      resume_after_dialogue_ = true;
    cancel_loading = busy_ && status_.state == State::Searching;
  }
  if (cancel_loading) {
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
  bool advance = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    advance = idle.value("data", true) && status_.state == State::Playing &&
              !playlist_.empty() && !manually_paused_ && !busy_;
  }
  if (advance) {
    HandleMusicCommand(common::MusicAction::Next, {}, 0);
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
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (status_.state == State::Error)
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
      if (!IsPlayableSong(song))
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
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      playlist_ = {{TrackFromSong(*selected), OriginalSongId(*selected)}};
      playlist_index_ = 0;
    }
    AppendPlaylist(songs);
    QueueTrack track;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      track = playlist_.front();
      LOG_INFO("Music queue selected: {} distinct songs", playlist_.size());
    }
    PlayTrack(job, track, 0);
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
    bool needs_alternatives = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      needs_alternatives = playlist_.size() == 1;
    }
    if (needs_alternatives) {
      // A specific-song search can contain only covers of that song. Broaden
      // the search instead of treating another version as the next song.
      auto search =
          RunCliCommand({"search", "song", "--keyword",
                         settings_.default_keyword, "--output", "json"},
                        job.revision);
      if (!IsJobCurrent(job.revision))
        return;
      if (search.ok && search.data.is_object() &&
          search.data.value("code", 0) == 200 && search.data.contains("data") &&
          search.data["data"].is_object() &&
          search.data["data"].contains("records") &&
          search.data["data"]["records"].is_array()) {
        AppendPlaylist(search.data["data"]["records"]);
      } else {
        LOG_WARN("Music alternatives search failed: exit_code={} timed_out={}",
                 search.exit_code, search.timed_out);
      }
    }
    QueueTrack track;
    std::size_t index = 0;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (playlist_.size() > 1) {
        index =
            job.action == Action::Next
                ? (playlist_index_ + 1) % playlist_.size()
                : (playlist_index_ + playlist_.size() - 1) % playlist_.size();
        track = playlist_[index];
        found = true;
      }
    }
    if (!found) {
      FailJob(job, "没有找到其他可播放的歌曲，请点一首新的歌。");
      return;
    }
    PlayTrack(job, track, index);
  }
}

void MusicController::AppendPlaylist(const json &songs) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  for (const auto &song : songs) {
    if (playlist_.size() >= 6)
      break;
    if (!IsPlayableSong(song))
      continue;
    auto track = TrackFromSong(song);
    const bool duplicate = std::any_of(
        playlist_.begin(), playlist_.end(), [&](const QueueTrack &queued) {
          return queued.track.song_id == track.song_id ||
                 SameSongTitle(queued.track.title, track.title);
        });
    if (!duplicate)
      playlist_.push_back({std::move(track), OriginalSongId(song)});
  }
}

void MusicController::PlayTrack(const Job &job, const QueueTrack &track,
                                std::size_t index) {
  if (!WaitForSpeechToFinish(job.revision))
    return;
  const auto previous_path =
      common::MpvCommand({"get_property", "path"}, settings_.mpv_socket);
  const bool switching = job.action == common::MusicAction::Next ||
                         job.action == common::MusicAction::Previous;
  // Each CLI play owns one track. The robot owns song order and metadata.
  common::MpvCommand({"set_property", "loop-file", "no"}, settings_.mpv_socket);
  common::MpvCommand({"set_property", "loop-playlist", "no"},
                     settings_.mpv_socket);
  common::MpvCommand({"stop"}, settings_.mpv_socket);
  const std::vector<std::string> play_args{
      "play",          "--song",          "--encrypted-id", track.track.song_id,
      "--original-id", track.original_id, "--output",       "json"};
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
        const auto path =
            common::MpvCommand({"get_property", "path"}, settings_.mpv_socket);
        if (switching && common::MpvOk(previous_path) &&
            (!common::MpvOk(path) || path["data"] == previous_path["data"])) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
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
    LOG_WARN("ncm-cli did not load the track on first attempt; retrying once");
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
      LOG_WARN("ncm-cli play failed: exit_code={} timed_out={}", play.exit_code,
               play.timed_out);
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    FailJob(job, "网易云后台两次都没有加载出歌曲，请稍后再试。");
    return;
  }
  if (!IsJobCurrent(job.revision))
    return;
  common::MpvCommand({"set_property", "audio-device", settings_.audio_device},
                     settings_.mpv_socket);
  if (!common::MpvOk(common::MpvCommand({"set_property", "pause", false},
                                        settings_.mpv_socket))) {
    FailJob(job, "音乐已加载，但无法从扬声器开始播放。");
    return;
  }
  const auto paused =
      common::MpvCommand({"get_property", "pause"}, settings_.mpv_socket);
  if (!common::MpvOk(paused) || paused.value("data", true)) {
    FailJob(job, "音乐没有从扬声器真正播放出来，请稍后再试。");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    playlist_index_ = index;
    status_.song_id = track.track.song_id;
    status_.title = track.track.title;
    status_.artist = track.track.artist;
    status_.position_seconds = 0;
  }
  SetPlaybackStatus(State::Playing, {}, job.request_id);
  LOG_INFO("MusicNode playback started: {} - {}; queue_index={}",
           track.track.title, track.track.artist, index);
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
  std::string directory = settings_.mpv_socket;
  const auto slash = directory.find_last_of('/');
  directory = slash == std::string::npos ? "." : directory.substr(0, slash);
  const std::string control_path = directory + "/player-daemon.sock";
  const double deadline = Monotonic() + 25.0;
  while (IsJobCurrent(revision) && Monotonic() < deadline) {
    {
      std::lock_guard<std::mutex> lock(backend_mutex_);
      if (backend_fd_ >= 0)
        return true;
    }
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
          std::lock_guard<std::mutex> lock(backend_mutex_);
          if (!IsJobCurrent(revision)) {
            ::close(subscription);
            return false;
          }
          if (backend_fd_ >= 0) {
            ::close(subscription);
            return true;
          }
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
