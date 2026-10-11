#include "music_node/music.hpp"
#include "common/mpv.hpp"
#include <algorithm>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
namespace { using State = xiaocai_robot::common::MusicState; }

Music::Music() : Node("music_node") {
  cli_path_ = declare_parameter<std::string>("cli_path", "/usr/local/bin/ncm-cli");
  socket_path_ = declare_parameter<std::string>("mpv_socket", xiaocai_robot::common::music_socket());
  default_keyword_ = declare_parameter<std::string>("default_keyword", "轻音乐");
  audio_device_ = declare_parameter<std::string>("audio_device", "alsa/xiaocai_speaker");
  wake_ack_path_ = declare_parameter<std::string>("wake_ack_path",
    "/home/lckfb/Documents/xiaocai_robot/resource/speech/wake_ack.wav");
  state_ = create_publisher<xiaocai_msgs::msg::MusicState>("/xiaocai_robot/music/state",
    rclcpp::QoS(1).transient_local());
  tts_ = create_publisher<std_msgs::msg::String>("/xiaocai_robot/tts/text", 10);
  intent_sub_ = create_subscription<xiaocai_msgs::msg::Intent>("/xiaocai_robot/intent", 10,
    [this](xiaocai_msgs::msg::Intent::SharedPtr message) { on_intent(*message); });
  play_sub_ = create_subscription<xiaocai_msgs::msg::AudioPlay>("/xiaocai_robot/audio/play", 10,
    [this](xiaocai_msgs::msg::AudioPlay::SharedPtr message) { on_audio_play(message->path); });
  voice_sub_ = create_subscription<std_msgs::msg::Bool>("/xiaocai_robot/audio/playing", 10,
    [this](std_msgs::msg::Bool::SharedPtr message) {
      voice_playing_ = message->data;
      last_voice_at_ = xc::monotonic();
    });
  timer_ = create_wall_timer(std::chrono::milliseconds(500), [this] { update_state(); });
  set_state(State::Stopped);
  worker_ = std::thread([this] { work(); });
  RCLCPP_INFO(get_logger(), "NetEase voice music ready; CLI: %s", cli_path_.c_str());
}

Music::~Music() {
  stop_ = true;
  ++revision_;
  ready_.notify_all();
  if (worker_.joinable()) worker_.join();
  xiaocai_robot::common::mpv_command({"stop"}, socket_path_);
}

bool Music::current(std::uint64_t revision) const {
  return !stop_ && !silenced_ && revision_.load() == revision;
}

void Music::on_audio_play(const std::string &path) {
  if (path == "__stop__") {
    silenced_ = true;
    ++revision_;
    { std::lock_guard<std::mutex> lock(jobs_mutex_); jobs_.clear(); }
    xiaocai_robot::common::mpv_command({"stop"}, socket_path_);
    set_state(State::Stopped);
    RCLCPP_INFO(get_logger(), "Hush: music stopped and pending requests cancelled");
  } else {
    if (path == wake_ack_path_) silenced_ = false;
    last_voice_at_ = xc::monotonic();
    xiaocai_robot::common::mpv_command({"set_property", "pause", true}, socket_path_);
  }
}

void Music::on_intent(const xiaocai_msgs::msg::Intent &message) {
  if (message.intent.rfind("music_", 0) != 0 || silenced_) return;
  const auto revision = ++revision_;
  if (message.intent == "music_stop" || message.intent == "music_pause") {
    xiaocai_robot::common::mpv_command(message.intent == "music_stop"
      ? xc::json{"stop"} : xc::json{"set_property", "pause", true}, socket_path_);
  }
  { std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.clear();
    jobs_.push_back({message.intent, message.content, revision, message.request_id});
  }
  ready_.notify_one();
  RCLCPP_INFO(get_logger(), "Music intent: %s; query: %s", message.intent.c_str(), message.content.c_str());
}

void Music::set_state(State state, const std::string &error,const std::string &request_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_message_.state = static_cast<std::uint8_t>(state);
  state_message_.error = error;
  state_message_.request_id = request_id;
  state_->publish(state_message_);
}

void Music::update_state() {
  using namespace xiaocai_robot::common;
  auto idle = mpv_command({"get_property", "idle-active"}, socket_path_);
  if (!mpv_ok(idle)) return;
  if (silenced_ && !idle.value("data", true)) {
    mpv_command({"stop"}, socket_path_);
    set_state(State::Stopped);
    return;
  }
  auto device = mpv_command({"get_property", "audio-device"}, socket_path_);
  if (mpv_ok(device) && device.value("data", std::string{}) != audio_device_)
    mpv_command({"set_property", "audio-device", audio_device_}, socket_path_);
  if (busy_) return;
  auto pause = mpv_command({"get_property", "pause"}, socket_path_);
  if (!mpv_ok(pause)) return;
  auto position = mpv_command({"get_property", "time-pos"}, socket_path_);
  auto duration = mpv_command({"get_property", "duration"}, socket_path_);
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (state_message_.state == static_cast<std::uint8_t>(State::Error) && idle.value("data", true)) return;
  state_message_.state = static_cast<std::uint8_t>(idle.value("data", true) ? State::Stopped
    : pause.value("data", true) ? State::Paused : State::Playing);
  state_message_.position_seconds = mpv_ok(position) && position["data"].is_number() ? position["data"].get<double>() : 0;
  state_message_.duration_seconds = mpv_ok(duration) && duration["data"].is_number() ? duration["data"].get<double>() : 0;
  state_->publish(state_message_);
}

Music::Result Music::cli(const std::vector<std::string> &arguments, std::uint64_t revision) {
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
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
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
  const double until = xc::monotonic() + 30;
  int status = 0;
  bool exited = false, cancelled = false;
  while (!exited) {
    char buffer[4096];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
      if (text.size() < 2 * 1024 * 1024) text.append(buffer, count);
    exited = waitpid(pid, &status, WNOHANG) == pid;
    if (!exited && (!current(revision) || xc::monotonic() > until)) {
      cancelled = true;
      kill(-pid, SIGTERM);
      const double grace = xc::monotonic() + 0.5;
      while (waitpid(pid, &status, WNOHANG) == 0 && xc::monotonic() < grace)
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
  auto data = xc::parse(text);
  return {!cancelled && WIFEXITED(status) && WEXITSTATUS(status) == 0, data};
}

bool Music::wait_for_speech(std::uint64_t revision) {
  const double until = xc::monotonic() + 20;
  while (current(revision) && xc::monotonic() < until) {
    if (!voice_playing_ && xc::monotonic() - last_voice_at_.load() > 0.5) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void Music::fail(const Job &job, const std::string &message) {
  if (!current(job.revision)) return;
  set_state(State::Error, message,job.request_id);
  if(job.request_id.empty())tts_->publish(xc::str(message));
  RCLCPP_WARN(get_logger(), "%s", message.c_str());
}

void Music::execute(const Job &job) {
  using namespace xiaocai_robot::common;
  if (job.intent == "music_play") {
    set_state(State::Searching,{},job.request_id);
    mpv_command({"set_property", "pause", true}, socket_path_);
    const std::string keyword = job.content.empty() ? default_keyword_ : job.content;
    auto search = cli({"search", "song", "--keyword", keyword}, job.revision);
    if (!current(job.revision)) return;
    if (!search.ok || !search.data.is_object() || search.data.value("code", 0) != 200 || !search.data.contains("data") ||
        !search.data["data"].is_object() || !search.data["data"].contains("records") ||
        !search.data["data"]["records"].is_array()) {
      fail(job, "网易云搜索暂时没成功，请稍后再试。"); return;
    }
    const auto &songs = search.data["data"]["records"];
    const xc::json *selected = nullptr;
    for (const auto &song : songs) {
      if (!song.is_object() || !song.value("playFlag", false) || !song.contains("originalId") ||
          song.value("id", std::string{}).empty()) continue;
      if (!selected) selected = &song;
      if (song.value("name", std::string{}) == keyword) { selected = &song; break; }
    }
    if (!selected) { fail(job, "没有找到当前账号可以播放的歌曲，请换一首。"); return; }
    if (!wait_for_speech(job.revision)) return;
    // Prevent old songs from being resumed by mpv when replacing the queue entry.
    mpv_command({"stop"}, socket_path_);
    auto play = cli({"play", "--song", "--encrypted-id", selected->value("id", std::string{}),
      "--original-id", (*selected)["originalId"].dump(), "--output", "json"}, job.revision);
    if (!current(job.revision)) { mpv_command({"stop"}, socket_path_); return; }
    // On a cold start ncm-cli exits successfully without JSON while its daemon
    // continues fetching the track. Verify the actual player instead.
    if (!play.ok || (play.data.is_object() && !play.data.value("success", false))) {
      fail(job, "这首歌暂时没能播放，请换一首。"); return;
    }
    { std::lock_guard<std::mutex> lock(state_mutex_);
      state_message_.title = selected->value("name", std::string{});
    }
    mpv_command({"set_property", "audio-device", audio_device_}, socket_path_);
    const double until = xc::monotonic() + 25;
    while (current(job.revision) && xc::monotonic() < until) {
      auto idle = mpv_command({"get_property", "idle-active"}, socket_path_);
      if (mpv_ok(idle) && !idle.value("data", true)) {
        if (!voice_playing_) mpv_command({"set_property", "pause", false}, socket_path_);
        set_state(State::Playing,{},job.request_id);
        RCLCPP_INFO(get_logger(), "Music playback started: %s", selected->value("name", std::string{}).c_str());
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fail(job, "音乐加载失败，请稍后再试。");
  } else if (job.intent == "music_resume") {
    if (!wait_for_speech(job.revision)) return;
    auto idle = mpv_command({"get_property", "idle-active"}, socket_path_);
    if (!mpv_ok(idle) || idle.value("data", true)) { fail(job, "还没有可继续播放的音乐，请先点歌。"); return; }
    if (!mpv_ok(mpv_command({"set_property", "pause", false}, socket_path_)))
      fail(job, "音乐暂时无法继续播放。");
    else set_state(State::Playing,{},job.request_id);
  } else if (job.intent == "music_pause") {
    set_state(State::Paused,{},job.request_id);
  } else if (job.intent == "music_stop") {
    mpv_command({"stop"}, socket_path_);
    set_state(State::Stopped,{},job.request_id);
  } else if (job.intent == "music_next" || job.intent == "music_previous") {
    if (!wait_for_speech(job.revision)) return;
    auto result = cli({job.intent == "music_next" ? "next" : "prev"}, job.revision);
    if (!current(job.revision)) return;
    if (!result.ok || !result.data.is_object() || !result.data.value("success", false)) fail(job, "没有可切换的歌曲，请点一首新的歌。");
    else if (mpv_ok(mpv_command({"set_property", "pause", false}, socket_path_)))set_state(State::Playing,{},job.request_id);
    else fail(job,"音乐暂时无法切换播放。");
  }
}

void Music::work() {
  while (!stop_) {
    Job job;
    { std::unique_lock<std::mutex> lock(jobs_mutex_);
      ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front()); jobs_.pop_front();
    }
    if (!current(job.revision)) continue;
    busy_ = true;
    try { execute(job); }
    catch (const std::exception &error) {
      RCLCPP_ERROR(get_logger(), "Music command failed: %s", error.what());
      fail(job, "音乐服务暂时出了点问题，请再试一次。");
    }
    busy_ = false;
  }
}

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<Music>());
  rclcpp::shutdown();
}
