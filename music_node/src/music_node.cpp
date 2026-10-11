#include "music_node/music_node.hpp"
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

extern char **environ;
namespace { using State = xiaocai_robot::common::MusicState; }

namespace {
bool SendLine(int fd, const std::string &line) {
  std::size_t sent = 0;
  while (sent < line.size()) {
    const auto count = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReceiveLine(int fd, std::string &line, int timeout_ms) {
  line.clear();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline && line.size() < 8192) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, left)));
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) ||
        ((descriptor.revents & POLLHUP) && !(descriptor.revents & POLLIN))) return false;
    char value = 0;
    const auto count = ::recv(fd, &value, 1, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    if (value == '\n') return true;
    line.push_back(value);
  }
  return false;
}

int ConnectUnix(const std::string &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) { ::close(fd); return -1; }
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
}

MusicNode::MusicNode()
    : Node("music_node"), music_config_(),
      log_manager_("music_node", music_config_.Logging()) {
  const auto &settings = music_config_.Settings();
  cli_path_ = declare_parameter<std::string>("cli_path", settings.cli_path);
  socket_path_ = declare_parameter<std::string>("mpv_socket", settings.mpv_socket);
  default_keyword_ = declare_parameter<std::string>("default_keyword", settings.default_keyword);
  audio_device_ = declare_parameter<std::string>("audio_device", settings.audio_device);
  wake_ack_path_ = declare_parameter<std::string>("wake_ack_path", settings.wake_ack_path);
  InitPublishers();
  InitSubscribers();
  InitTimers();
  SetState(State::Stopped);
  worker_ = std::thread([this] { Work(); });
  LOG_INFO("NetEase voice music ready; CLI: {}", cli_path_);
}

void MusicNode::InitPublishers() {
  state_ = create_publisher<xiaocai_msgs::msg::MusicState>("/xiaocai_robot/music/state",
    rclcpp::QoS(1).transient_local());
  tts_ = create_publisher<std_msgs::msg::String>("/xiaocai_robot/tts/text", 10);
}

void MusicNode::InitSubscribers() {
  intent_sub_ = create_subscription<xiaocai_msgs::msg::Intent>("/xiaocai_robot/intent", 10,
    [this](xiaocai_msgs::msg::Intent::SharedPtr message) { OnIntentCallback(*message); });
  play_sub_ = create_subscription<xiaocai_msgs::msg::AudioPlay>("/xiaocai_robot/audio/play", 10,
    [this](xiaocai_msgs::msg::AudioPlay::SharedPtr message) { OnAudioPlayCallback(*message); });
  voice_sub_ = create_subscription<std_msgs::msg::Bool>("/xiaocai_robot/audio/playing", 10,
    [this](std_msgs::msg::Bool::SharedPtr message) { OnAudioPlayingCallback(*message); });
}

void MusicNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::milliseconds(500), [this] { UpdateState(); });
}

MusicNode::~MusicNode() {
  stop_ = true;
  ++revision_;
  ready_.notify_all();
  if (worker_.joinable()) worker_.join();
  xiaocai_robot::common::MpvCommand({"stop"}, socket_path_);
  CloseBackend();
}

bool MusicNode::Current(std::uint64_t revision) const {
  return !stop_ && !silenced_ && revision_.load() == revision;
}

void MusicNode::OnAudioPlayCallback(const xiaocai_msgs::msg::AudioPlay &message) {
  const auto &path = message.path;
  if (path == "__stop__") {
    silenced_ = true;
    ++revision_;
    { std::lock_guard<std::mutex> lock(jobs_mutex_); jobs_.clear(); }
    xiaocai_robot::common::MpvCommand({"stop"}, socket_path_);
    CloseBackend();
    SetState(State::Stopped);
    LOG_INFO("Hush: music stopped and pending requests cancelled");
  } else {
    if (path == wake_ack_path_) {
      silenced_ = false;
      ++revision_;
      { std::lock_guard<std::mutex> lock(jobs_mutex_); jobs_.clear(); }
      xiaocai_robot::common::MpvCommand({"stop"}, socket_path_);
      CloseBackend();
      SetState(State::Stopped);
      LOG_INFO("Wake acknowledged: continuous music playback ended");
      return;
    }
    last_voice_at_ = xc::Monotonic();
    xiaocai_robot::common::MpvCommand({"set_property", "pause", true}, socket_path_);
  }
}

void MusicNode::OnIntentCallback(const xiaocai_msgs::msg::Intent &message) {
  if (message.intent.rfind("music_", 0) != 0 || silenced_) return;
  const auto revision = ++revision_;
  if (message.intent == "music_stop" || message.intent == "music_pause") {
    xiaocai_robot::common::MpvCommand(message.intent == "music_stop"
      ? xc::json{"stop"} : xc::json{"set_property", "pause", true}, socket_path_);
  }
  { std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.clear();
    jobs_.push_back({message.intent, message.content, revision, message.request_id});
  }
  ready_.notify_one();
  LOG_INFO("MusicNode intent: {}; query: {}", message.intent, message.content);
}

void MusicNode::OnAudioPlayingCallback(const std_msgs::msg::Bool &message) {
  voice_playing_ = message.data;
  last_voice_at_ = xc::Monotonic();
}

void MusicNode::SetState(State state, const std::string &error,const std::string &request_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_message_.state = static_cast<std::uint8_t>(state);
  state_message_.error = error;
  state_message_.request_id = request_id;
  state_->publish(state_message_);
}

void MusicNode::UpdateState() {
  using namespace xiaocai_robot::common;
  MaintainBackend();
  auto idle = MpvCommand({"get_property", "idle-active"}, socket_path_);
  if (!MpvOk(idle)) return;
  if (silenced_ && !idle.value("data", true)) {
    MpvCommand({"stop"}, socket_path_);
    SetState(State::Stopped);
    return;
  }
  auto device = MpvCommand({"get_property", "audio-device"}, socket_path_);
  if (MpvOk(device) && device.value("data", std::string{}) != audio_device_)
    MpvCommand({"set_property", "audio-device", audio_device_}, socket_path_);
  if (busy_) return;
  auto pause = MpvCommand({"get_property", "pause"}, socket_path_);
  if (!MpvOk(pause)) return;
  auto position = MpvCommand({"get_property", "time-pos"}, socket_path_);
  auto duration = MpvCommand({"get_property", "duration"}, socket_path_);
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (state_message_.state == static_cast<std::uint8_t>(State::Error) && idle.value("data", true)) return;
  state_message_.state = static_cast<std::uint8_t>(idle.value("data", true) ? State::Stopped
    : pause.value("data", true) ? State::Paused : State::Playing);
  state_message_.position_seconds = MpvOk(position) && position["data"].is_number() ? position["data"].get<double>() : 0;
  state_message_.duration_seconds = MpvOk(duration) && duration["data"].is_number() ? duration["data"].get<double>() : 0;
  state_->publish(state_message_);
}

bool MusicNode::EnsureBackend(std::uint64_t revision) {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ >= 0) return true;

  std::string directory = socket_path_;
  const auto slash = directory.find_last_of('/');
  directory = slash == std::string::npos ? "." : directory.substr(0, slash);
  const std::string control_path = directory + "/player-daemon.sock";
  const double deadline = xc::Monotonic() + 5.0;
  while (Current(revision) && xc::Monotonic() < deadline) {
    int subscription = ConnectUnix(control_path);
    if (subscription >= 0) {
      const std::string request = xc::json{{"command", "subscribe"},
        {"role", "observer"}, {"controllerType", "cli"}}.dump() + "\n";
      std::string snapshot;
      if (SendLine(subscription, request) && ReceiveLine(subscription, snapshot, 1200)) {
        const auto event = xc::Parse(snapshot);
        if (event.is_object() && event.value("event", std::string{}) == "stateSnapshot") {
          const int flags = ::fcntl(subscription, F_GETFL, 0);
          if (flags >= 0) ::fcntl(subscription, F_SETFL, flags | O_NONBLOCK);
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

void MusicNode::MaintainBackend() {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ < 0) return;
  char buffer[4096];
  for (;;) {
    const auto count = ::recv(backend_fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0) continue;
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    ::close(backend_fd_);
    backend_fd_ = -1;
    LOG_WARN("ncm-cli player daemon subscription disconnected");
    return;
  }
}

void MusicNode::CloseBackend() {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ >= 0) ::close(backend_fd_);
  backend_fd_ = -1;
}

MusicNode::Result MusicNode::Cli(const std::vector<std::string> &arguments, std::uint64_t revision) {
  int output[2];
  if (pipe2(output, O_CLOEXEC) != 0) return {};
  std::vector<std::string> args{cli_path_};
  args.insert(args.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &arg : args) argv.push_back(arg.data());
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
  const int error = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(output[1]);
  if (error != 0) { close(output[0]); return {}; }
  fcntl(output[0], F_SETFL, O_NONBLOCK);
  std::string text;
  const double until = xc::Monotonic() + 30;
  int status = 0;
  bool exited = false, cancelled = false;
  while (!exited) {
    char buffer[4096];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
      if (text.size() < 2 * 1024 * 1024) text.append(buffer, count);
    exited = waitpid(pid, &status, WNOHANG) == pid;
    if (!exited && (!Current(revision) || xc::Monotonic() > until)) {
      cancelled = true;
      kill(-pid, SIGTERM);
      const double grace = xc::Monotonic() + 0.5;
      while (waitpid(pid, &status, WNOHANG) == 0 && xc::Monotonic() < grace)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      kill(-pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      exited = true;
    }
    if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  char buffer[4096]; ssize_t count;
  while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
    if (text.size() < 2 * 1024 * 1024) text.append(buffer, count);
  close(output[0]);
  xc::json data = xc::json::value_t::discarded;
  const auto begin = text.find('{');
  const auto end = text.rfind('}');
  if (begin != std::string::npos && end != std::string::npos && end >= begin)
    data = xc::Parse(text.substr(begin, end - begin + 1));
  return {!cancelled && WIFEXITED(status) && WEXITSTATUS(status) == 0, data, text};
}

bool MusicNode::WaitForSpeech(std::uint64_t revision) {
  const double until = xc::Monotonic() + 20;
  while (Current(revision) && xc::Monotonic() < until) {
    if (!voice_playing_ && xc::Monotonic() - last_voice_at_.load() > 0.5) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void MusicNode::Fail(const Job &job, const std::string &message) {
  if (!Current(job.revision)) return;
  SetState(State::Error, message,job.request_id);
  if(job.request_id.empty())tts_->publish(xc::Str(message));
  LOG_WARN("{}", message);
}

void MusicNode::Execute(const Job &job) {
  using namespace xiaocai_robot::common;
  if (job.intent == "music_play") {
    SetState(State::Searching,{},job.request_id);
    MpvCommand({"set_property", "pause", true}, socket_path_);
    const std::string keyword = job.content.empty() ? default_keyword_ : job.content;
    auto search = Cli({"search", "song", "--keyword", keyword}, job.revision);
    if (!Current(job.revision)) return;
    if (!search.ok || !search.data.is_object() || search.data.value("code", 0) != 200 || !search.data.contains("data") ||
        !search.data["data"].is_object() || !search.data["data"].contains("records") ||
        !search.data["data"]["records"].is_array()) {
      Fail(job, "网易云搜索暂时没成功，请稍后再试。"); return;
    }
    const auto &songs = search.data["data"]["records"];
    const xc::json *selected = nullptr;
    for (const auto &song : songs) {
      if (!song.is_object() || !song.value("playFlag", false) || !song.contains("originalId") ||
          song.value("id", std::string{}).empty()) continue;
      if (!selected) selected = &song;
      if (song.value("name", std::string{}) == keyword) { selected = &song; break; }
    }
    if (!selected) { Fail(job, "没有找到当前账号可以播放的歌曲，请换一首。"); return; }
    if (!WaitForSpeech(job.revision)) return;
    // Prevent old songs from being resumed by mpv when replacing the queue entry.
    MpvCommand({"stop"}, socket_path_);
    const std::vector<std::string> play_args{"play", "--song", "--encrypted-id",
      selected->value("id", std::string{}), "--original-id", (*selected)["originalId"].dump(),
      "--output", "json"};
    auto play = Cli(play_args, job.revision);
    if (!Current(job.revision)) { MpvCommand({"stop"}, socket_path_); return; }
    if (!EnsureBackend(job.revision)) {
      MpvCommand({"stop"}, socket_path_);
      Fail(job, "网易云播放器后台没有保持连接，请稍后再试。"); return;
    }

    auto wait_for_track = [&](double seconds) {
      const double until = xc::Monotonic() + seconds;
      while (Current(job.revision) && xc::Monotonic() < until) {
        auto idle = MpvCommand({"get_property", "idle-active"}, socket_path_);
        if (MpvOk(idle) && !idle.value("data", true)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      return false;
    };
    // A cold ncm-cli daemon can miss its first 3-second playback response.
    // Keep the daemon subscribed, then retry once without requiring a second
    // user utterance. MPV state, not the CLI exit code, decides whether it worked.
    bool loaded = wait_for_track(6.0);
    if (!loaded && Current(job.revision)) {
      LOG_WARN("ncm-cli did not load the track on first attempt; retrying once");
      if (!play.output.empty()) LOG_WARN("First ncm-cli play output: {:.400}", play.output);
      play = Cli(play_args, job.revision);
      if (!Current(job.revision)) { MpvCommand({"stop"}, socket_path_); return; }
      loaded = wait_for_track(12.0);
    }
    if (!loaded) {
      if (!play.ok || (play.data.is_object() && !play.data.value("success", false)))
        LOG_WARN("ncm-cli play failed: {:.400}", play.output);
      MpvCommand({"stop"}, socket_path_);
      Fail(job, "网易云后台两次都没有加载出歌曲，请稍后再试。"); return;
    }
    { std::lock_guard<std::mutex> lock(state_mutex_);
      state_message_.title = selected->value("name", std::string{});
    }
    MpvCommand({"set_property", "audio-device", audio_device_}, socket_path_);
    {
      auto idle = MpvCommand({"get_property", "idle-active"}, socket_path_);
      if (MpvOk(idle) && !idle.value("data", true)) {
        // Hold the first track while preparing the queue so short tracks cannot
        // end before their follow-up entries have been added.
        MpvCommand({"set_property", "pause", true}, socket_path_);
        // `play --song` replaces the ncm-cli queue with just the selected track.
        // Append playable search results, then loop the complete queue forever.
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
              !song.contains("originalId")) continue;
          const auto encrypted_id = song.value("id", std::string{});
          if (encrypted_id.empty() || encrypted_id == selected->value("id", std::string{})) continue;
          ++attempts;
          auto added = Cli({"queue", "add", "--encrypted-id", encrypted_id,
            "--original-id", song["originalId"].dump(), "--output", "json"}, job.revision);
          if (!Current(job.revision)) return;
          if (added.ok && added.data.is_object() && added.data.value("success", false)) {
            ++queued;
          } else {
            LOG_WARN("Could not queue next search result ({}): {:.240}",
              song.value("name", std::string{}), added.output);
          }
        }
        auto loop = MpvCommand({"set_property", "loop-playlist", "inf"}, socket_path_);
        if (!MpvOk(loop)) {
          LOG_WARN("Could not enable continuous playlist loop");
          loop = MpvCommand({"set_property", "loop-file", "inf"}, socket_path_);
        }
        if (!MpvOk(loop)) {
          MpvCommand({"stop"}, socket_path_);
          Fail(job, "网易云播放器无法保持连续播放，请稍后再试。"); return;
        }
        if (!voice_playing_ && !MpvOk(MpvCommand({"set_property", "pause", false}, socket_path_))) {
          MpvCommand({"stop"}, socket_path_);
          Fail(job, "音乐已加载，但无法从扬声器开始播放。"); return;
        }
        bool playing = false;
        const double verify_until = xc::Monotonic() + 2.0;
        while (Current(job.revision) && xc::Monotonic() < verify_until) {
          auto active = MpvCommand({"get_property", "idle-active"}, socket_path_);
          auto paused = MpvCommand({"get_property", "pause"}, socket_path_);
          if (MpvOk(active) && MpvOk(paused) && !active.value("data", true) &&
              !paused.value("data", true)) { playing = true; break; }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!playing) {
          MpvCommand({"stop"}, socket_path_);
          Fail(job, "音乐没有从扬声器真正播放出来，请稍后再试。"); return;
        }
        SetState(State::Playing,{},job.request_id);
        LOG_INFO("MusicNode playback started continuously: {}; queued {} follow-up tracks",
          selected->value("name", std::string{}), queued);
        return;
      }
    }
    Fail(job, "音乐加载后播放器状态丢失，请稍后再试。");
  } else if (job.intent == "music_resume") {
    if (!WaitForSpeech(job.revision)) return;
    auto idle = MpvCommand({"get_property", "idle-active"}, socket_path_);
    if (!MpvOk(idle) || idle.value("data", true)) { Fail(job, "还没有可继续播放的音乐，请先点歌。"); return; }
    if (!MpvOk(MpvCommand({"set_property", "pause", false}, socket_path_)))
      Fail(job, "音乐暂时无法继续播放。");
    else SetState(State::Playing,{},job.request_id);
  } else if (job.intent == "music_pause") {
    SetState(State::Paused,{},job.request_id);
  } else if (job.intent == "music_stop") {
    MpvCommand({"stop"}, socket_path_);
    SetState(State::Stopped,{},job.request_id);
  } else if (job.intent == "music_next" || job.intent == "music_previous") {
    if (!WaitForSpeech(job.revision)) return;
    auto result = Cli({job.intent == "music_next" ? "next" : "prev"}, job.revision);
    if (!Current(job.revision)) return;
    if (!result.ok || !result.data.is_object() || !result.data.value("success", false)) Fail(job, "没有可切换的歌曲，请点一首新的歌。");
    else if (MpvOk(MpvCommand({"set_property", "pause", false}, socket_path_)))SetState(State::Playing,{},job.request_id);
    else Fail(job,"音乐暂时无法切换播放。");
  }
}

void MusicNode::Work() {
  while (!stop_) {
    Job job;
    { std::unique_lock<std::mutex> lock(jobs_mutex_);
      ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front()); jobs_.pop_front();
    }
    if (!Current(job.revision)) continue;
    busy_ = true;
    try { Execute(job); }
    catch (const std::exception &error) {
      LOG_ERROR("MusicNode command failed: {}", error.what());
      Fail(job, "音乐服务暂时出了点问题，请再试一次。");
    }
    busy_ = false;
  }
}
