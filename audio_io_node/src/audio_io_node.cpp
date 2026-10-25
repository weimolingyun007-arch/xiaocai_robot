#include "audio_io_node/audio_io_node.hpp"
#include "common/common.hpp"
#include "defination/audio_stream_action.hpp"
#include "defination/volume_action.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <rclcpp/rclcpp.hpp>
#include <spawn.h>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>

extern char **environ;

namespace xiaocai_robot {

AudioIoNode::AudioIoNode()
    : Node("audio_io_node"), audio_io_config_(),
      log_manager_("audio_io_node", audio_io_config_.Logging()),
      readiness_publisher_(*this, "audio_io_node"), volume_percent_(80),
      stop_(false), silenced_(true), capture_pid_(-1), playback_pid_(-1),
      stream_pid_(-1) {
  const auto &settings = audio_io_config_.Settings();
  ConfigureAlsa("audio_io_node", settings.alsa);
  capture_device_ =
      declare_parameter<std::string>("capture_device", settings.capture_device);
  playback_device_ = declare_parameter<std::string>("playback_device",
                                                    settings.playback_device);
  wake_ack_path_ =
      declare_parameter<std::string>("wake_ack_path", settings.wake_ack_path);
  startup_music_path_ = audio_io_config_.StartupMusicPath().string();
  volume_percent_ = declare_parameter<int>(
      "volume_percent", static_cast<int>(settings.volume_percent));
  int adc_pga_gain = declare_parameter<int>(
      "adc_pga_gain", static_cast<int>(settings.adc_pga_gain));
  int mic_boost_gain = declare_parameter<int>(
      "mic_boost_gain", static_cast<int>(settings.mic_boost_gain));
  InitPublishers();
  InitSubscribers();
  InitServers();
  const auto mixer_card = std::to_string(settings.alsa.card);
  Run({"amixer", "-c", mixer_card, "set", "Playback Path", "SPK"});
  Run({"amixer", "-c", mixer_card, "set", "Capture MIC Path", "Main Mic"});
  MakeSilence();
  // Create the shared ALSA software-gain control with an inaudible zero PCM.
  Run({"aplay", "-q", "-D", playback_device_, silence_});
  SetVolume(volume_percent_, true);
  Run({"amixer", "-c", mixer_card, "set", "ADC PGA Gain",
       std::to_string(adc_pga_gain)});
  Run({"amixer", "-c", mixer_card, "set", "MIC Boost Gain",
       std::to_string(mic_boost_gain)});
  signal(SIGPIPE, SIG_IGN);
  MakeBeep();
  capture_ = std::thread([this] { CaptureLoop(); });
  playback_ = std::thread([this] { PlaybackLoop(); });
  stream_playback_ = std::thread([this] { StreamPlaybackLoop(); });
  readiness_publisher_.MarkReady();
}

AudioIoNode::~AudioIoNode() {
  stop_ = true;
  ++stream_revision_;
  cv_.notify_all();
  stream_cv_.notify_all();
  for (const auto pid :
       {capture_pid_.load(), playback_pid_.load(), stream_pid_.load()})
    if (pid > 0)
      kill(pid, SIGTERM);
  if (capture_.joinable())
    capture_.join();
  if (playback_.joinable())
    playback_.join();
  if (stream_playback_.joinable())
    stream_playback_.join();
}

// ROS communication initialization.
void AudioIoNode::InitPublishers() {
  int initialized_count = 0;
  mic_publisher_ = create_publisher<std_msgs::msg::UInt8MultiArray>(
      "/xiaocai_robot/audio/mic", 10);
  ++initialized_count;
  playing_publisher_ =
      create_publisher<std_msgs::msg::Bool>("/xiaocai_robot/audio/playing", 10);
  ++initialized_count;
  volume_state_publisher_ = create_publisher<std_msgs::msg::Int32>(
      "/xiaocai_robot/audio/volume_state", rclcpp::QoS(1).transient_local());
  ++initialized_count;
  playback_result_publisher_ =
      create_publisher<xiaocai_msgs::msg::AudioPlaybackResult>(
          "/xiaocai_robot/audio/result", 100);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void AudioIoNode::InitSubscribers() {
  int initialized_count = 0;
  play_subscriber_ = create_subscription<xiaocai_msgs::msg::AudioPlay>(
      "/xiaocai_robot/audio/play", 10,
      [this](xiaocai_msgs::msg::AudioPlay::SharedPtr message) {
        OnAudioPlayCallback(message);
      });
  ++initialized_count;
  startup_music_subscriber_ = create_subscription<std_msgs::msg::Empty>(
      "/xiaocai_robot/audio/startup_music", 10,
      [this](std_msgs::msg::Empty::SharedPtr message) {
        OnStartupMusicCallback(message);
      });
  ++initialized_count;
  stream_subscriber_ = create_subscription<xiaocai_msgs::msg::AudioStream>(
      "/xiaocai_robot/audio/stream", rclcpp::QoS(10),
      [this](xiaocai_msgs::msg::AudioStream::SharedPtr message) {
        OnAudioStreamCallback(message);
      });
  ++initialized_count;
  volume_command_subscriber_ =
      create_subscription<xiaocai_msgs::msg::VolumeCommand>(
          "/xiaocai_robot/audio/volume_command", 10,
          [this](xiaocai_msgs::msg::VolumeCommand::SharedPtr message) {
            OnVolumeCommandCallback(message);
          });
  ++initialized_count;
  silent_volume_command_subscriber_ =
      create_subscription<xiaocai_msgs::msg::VolumeCommand>(
          "/xiaocai_robot/audio/volume_silent_command", 10,
          [this](xiaocai_msgs::msg::VolumeCommand::SharedPtr message) {
            OnVolumeCommandCallback(message, false);
          });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void AudioIoNode::InitServers() {
  volume_get_server_ = create_service<xiaocai_msgs::srv::GetVolume>(
      "/xiaocai_robot/audio/get_volume",
      [this](const std::shared_ptr<xiaocai_msgs::srv::GetVolume::Request>,
             std::shared_ptr<xiaocai_msgs::srv::GetVolume::Response> response) {
        response->percent = volume_percent_;
      });
}

// ROS message publishing.
void AudioIoNode::PublishPlaybackResult(const std::string &path, bool success) {
  xiaocai_msgs::msg::AudioPlaybackResult message;
  message.timestamp = now();
  message.path = path;
  message.success = success;
  playback_result_publisher_->publish(message);
}

// ROS subscription callbacks.
void AudioIoNode::OnAudioPlayCallback(
    const xiaocai_msgs::msg::AudioPlay::SharedPtr message) {
  if (message->path.empty()) {
    silenced_.store(true);
    CancelAudioStream();
    std::deque<std::string> cancelled;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cancelled.swap(queue_);
    }
    for (const auto &path : cancelled) {
      if (path.rfind("/tmp/xiaocai_tts/", 0) == 0)
        unlink(path.c_str());
      PublishPlaybackResult(path, false);
    }
    const auto pid = playback_pid_.load();
    if (pid > 0)
      kill(pid, SIGTERM);
    return;
  }
  if (message->path == wake_ack_path_)
    silenced_.store(false);
  if (silenced_.load()) {
    if (message->path.rfind("/tmp/xiaocai_tts/", 0) == 0)
      unlink(message->path.c_str());
    PublishPlaybackResult(message->path, false);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(message->path);
  }
  cv_.notify_one();
}

void AudioIoNode::OnStartupMusicCallback(
    const std_msgs::msg::Empty::SharedPtr) {
  if (startup_music_queued_)
    return;
  if (access(startup_music_path_.c_str(), R_OK) != 0) {
    LOG_ERROR("Missing startup music file: {}", startup_music_path_);
    return;
  }

  startup_music_queued_ = true;
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(startup_music_path_);
  }
  cv_.notify_one();
  LOG_INFO("Queued startup music");
}

void AudioIoNode::CancelAudioStream() {
  std::lock_guard<std::mutex> lock(stream_mu_);
  stream_queue_.clear();
  stream_ended_ = true;
  ++stream_revision_;
  if (stream_pid_ > 0)
    kill(stream_pid_, SIGTERM);
  stream_cv_.notify_one();
}

void AudioIoNode::OnAudioStreamCallback(
    const xiaocai_msgs::msg::AudioStream::SharedPtr message) {
  using Action = common::AudioStreamAction;
  const auto action = static_cast<Action>(message->action);
  if (message->stream_id == 0 ||
      (action != Action::Data && action != Action::End &&
       action != Action::Cancel))
    return;
  if (action == Action::Data &&
      (silenced_ || message->sample_rate < 8000 ||
       message->sample_rate > 192000 || message->channels < 1 ||
       message->channels > 2 || message->data.empty() ||
       message->data.size() >
           message->sample_rate * message->channels * 2 / 50 ||
       message->data.size() % (message->channels * 2) != 0)) {
    LOG_WARN("Ignoring gated or invalid PCM frame");
    return;
  }
  std::lock_guard<std::mutex> lock(stream_mu_);
  if (message->stream_id < stream_id_ ||
      message->stream_id == failed_stream_id_)
    return;
  if (message->stream_id != stream_id_) {
    stream_id_ = message->stream_id;
    stream_sequence_ = 0;
    stream_sample_rate_ = message->sample_rate;
    stream_channels_ = message->channels;
    stream_ended_ = false;
    stream_queue_.clear();
    ++stream_revision_;
    if (stream_pid_ > 0)
      kill(stream_pid_, SIGTERM);
  } else if (message->sequence < stream_sequence_) {
    return;
  }
  stream_sequence_ = message->sequence + 1;
  if (action == Action::Cancel) {
    stream_queue_.clear();
    stream_ended_ = true;
    ++stream_revision_;
    if (stream_pid_ > 0)
      kill(stream_pid_, SIGTERM);
  } else if (action == Action::End) {
    stream_ended_ = true;
  } else if (!stream_ended_ && message->sample_rate == stream_sample_rate_ &&
             message->channels == stream_channels_) {
    // At most five 20 ms frames; preserve recent audio if a sink stalls.
    if (stream_queue_.size() >= 5) {
      stream_queue_.pop_front();
      ++stream_dropped_;
      if ((stream_dropped_ & (stream_dropped_ - 1)) == 0)
        LOG_WARN("PCM sink queue overflow; dropped frames={}", stream_dropped_);
    }
    stream_queue_.push_back(*message);
  }
  stream_cv_.notify_one();
}

void AudioIoNode::OnVolumeCommandCallback(
    const xiaocai_msgs::msg::VolumeCommand::SharedPtr message, bool notify) {
  const auto action = static_cast<common::VolumeAction>(message->action);
  if (action == common::VolumeAction::Up)
    SetVolume(volume_percent_ + 10, notify);
  else if (action == common::VolumeAction::Down)
    SetVolume(volume_percent_ - 10, notify);
  else if (action == common::VolumeAction::Set)
    SetVolume(message->percent, notify);
}

// Playback configuration and PCM helpers.
void AudioIoNode::SetVolume(int percent, bool notify) {
  percent = std::clamp(percent, 0, 150);
  // The RK809 driver rejects writes above 252 even though its mixer advertises
  // 255.
  int dac = percent == 0 ? 0 : std::min(252, 200 + (percent * 52 + 50) / 100);
  const auto &alsa = audio_io_config_.Settings().alsa;
  const auto mixer_card = std::to_string(alsa.card);
  int status = Run({"amixer", "-c", mixer_card, "cset", "numid=15",
                    std::to_string(dac) + "," + std::to_string(dac)});
  if (status) {
    LOG_ERROR("Cannot set playback volume: {}", status);
    return;
  }
  // 100% sets the hardware DAC to full scale. Above it, add up to +6 dB in
  // the shared PCM so voice prompts and mpv music are raised together.
  const double boost_db = std::max(0, percent - 100) * 0.12;
  const int software_volume =
      static_cast<int>(std::lround((60.0 + boost_db) / 66.0 * 255.0));
  status = Run({"amixer", "-c", mixer_card, "cset",
                "name='" + alsa.softvol_control_name + "'",
                std::to_string(software_volume) + "," +
                    std::to_string(software_volume)});
  if (status) {
    LOG_ERROR("Cannot set software output gain: {}", status);
    return;
  }
  volume_percent_ = percent;
  LOG_INFO("Playback volume: {}% (DAC {})", percent, dac);
  if (notify) {
    std_msgs::msg::Int32 state;
    state.data = percent;
    volume_state_publisher_->publish(state);
  }
}

void AudioIoNode::MakeBeep() {
  beep_ = "/tmp/xiaocai_beep.wav";
  std::ofstream out(beep_, std::ios::binary);
  const int n = 1440;
  const int size = n * 2;
  out.write("RIFF", 4);
  unsigned v = 36 + size;
  out.write((char *)&v, 4);
  out.write("WAVEfmt ", 8);
  v = 16;
  out.write((char *)&v, 4);
  unsigned short s = 1;
  out.write((char *)&s, 2);
  out.write((char *)&s, 2);
  v = 16000;
  out.write((char *)&v, 4);
  v = 32000;
  out.write((char *)&v, 4);
  s = 2;
  out.write((char *)&s, 2);
  s = 16;
  out.write((char *)&s, 2);
  out.write("data", 4);
  v = size;
  out.write((char *)&v, 4);
  for (int i = 0; i < n; ++i) {
    short sample =
        static_cast<short>(2500 * std::sin(2 * M_PI * 750 * i / 16000));
    out.write((char *)&sample, 2);
  }
}

void AudioIoNode::MakeSilence() {
  silence_ = "/tmp/xiaocai_silence.wav";
  std::ofstream out(silence_, std::ios::binary);
  unsigned samples = 160, bytes = samples * 2;
  out.write("RIFF", 4);
  unsigned size = 36 + bytes;
  out.write(reinterpret_cast<char *>(&size), 4);
  out.write("WAVEfmt ", 8);
  size = 16;
  out.write(reinterpret_cast<char *>(&size), 4);
  unsigned short format = 1;
  out.write(reinterpret_cast<char *>(&format), 2);
  out.write(reinterpret_cast<char *>(&format), 2);
  size = 16000;
  out.write(reinterpret_cast<char *>(&size), 4);
  size = 32000;
  out.write(reinterpret_cast<char *>(&size), 4);
  unsigned short block = 2, bits = 16;
  out.write(reinterpret_cast<char *>(&block), 2);
  out.write(reinterpret_cast<char *>(&bits), 2);
  out.write("data", 4);
  out.write(reinterpret_cast<char *>(&bytes), 4);
  const std::array<char, 320> zeros{};
  out.write(zeros.data(), zeros.size());
}

// Audio worker loops.
void AudioIoNode::CaptureLoop() {
  while (!stop_) {
    int fds[2];
    if (pipe(fds) != 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    pid_t pid = fork();
    if (pid == 0) {
      close(fds[0]);
      dup2(fds[1], STDOUT_FILENO);
      close(fds[1]);
      execlp("arecord", "arecord", "-q", "-D", capture_device_.c_str(), "-c",
             "2", "-r", "16000", "-f", "S16_LE", "-t", "raw", (char *)nullptr);
      _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
      close(fds[0]);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    capture_pid_ = pid;
    if (stop_)
      kill(pid, SIGTERM);
    std::vector<unsigned char> raw(6400);
    while (!stop_) {
      size_t received = 0;
      while (received < raw.size()) {
        ssize_t n = read(fds[0], raw.data() + received, raw.size() - received);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          break;
        received += static_cast<size_t>(n);
      }
      if (received != raw.size())
        break;
      std_msgs::msg::UInt8MultiArray msg;
      msg.data.resize(3200);
      for (size_t i = 0; i < 1600; ++i) {
        msg.data[2 * i] = raw[4 * i];
        msg.data[2 * i + 1] = raw[4 * i + 1];
      }
      mic_publisher_->publish(msg);
    }
    close(fds[0]);
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    capture_pid_ = -1;
    if (!stop_)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void AudioIoNode::PlaybackLoop() {
  while (!stop_) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_)
        break;
      path = queue_.front();
      queue_.pop_front();
    }
    const bool is_startup_music = path == startup_music_path_;
    if (silenced_.load() && !is_startup_music) {
      if (path.rfind("/tmp/xiaocai_tts/", 0) == 0)
        unlink(path.c_str());
      PublishPlaybackResult(path, false);
      continue;
    }
    if (path == "__beep__")
      path = beep_;
    if (access(path.c_str(), R_OK) != 0) {
      LOG_ERROR("Missing audio file: {}", path);
      PublishPlaybackResult(path, false);
      continue;
    }
    std_msgs::msg::Bool state;
    if (!is_startup_music) {
      state.data = true;
      playing_publisher_->publish(state);
      // music_node subscribes to audio/play and audio/playing and pauses
      // its decoder before spoken replies reach the shared speaker.
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    int status = -1;
    if (!silenced_.load() || is_startup_music) {
      std::vector<std::string> arguments = {"aplay", "-q", "-D",
                                            playback_device_, path};
      std::vector<char *> argv;
      for (auto &argument : arguments)
        argv.push_back(argument.data());
      argv.push_back(nullptr);
      const pid_t pid = fork();
      if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
      }
      if (pid < 0)
        status = -1;
      else {
        playback_pid_.store(pid);
        if (silenced_.load() && !is_startup_music)
          kill(pid, SIGTERM);
        int child_status = 0;
        while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {
        }
        playback_pid_.store(-1);
        status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
      }
    }
    if (status && (is_startup_music || !silenced_.load()))
      LOG_ERROR("aplay failed: {}", status);
    if (path.find("/tmp/xiaocai_tts/") == 0)
      unlink(path.c_str());
    if (!is_startup_music) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      state.data = false;
      playing_publisher_->publish(state);
    }
    PublishPlaybackResult(path, status == 0 &&
                                    (is_startup_music || !silenced_.load()));
  }
}

void AudioIoNode::StreamPlaybackLoop() {
  while (!stop_) {
    xiaocai_msgs::msg::AudioStream first;
    std::uint64_t revision;
    {
      std::unique_lock<std::mutex> lock(stream_mu_);
      stream_cv_.wait(lock, [this] { return stop_ || !stream_queue_.empty(); });
      if (stop_)
        return;
      first = stream_queue_.front();
      revision = stream_revision_;
    }
    int input[2];
    if (pipe2(input, O_CLOEXEC) != 0) {
      LOG_ERROR("Cannot create PCM output pipe: {}", errno);
      std::lock_guard<std::mutex> lock(stream_mu_);
      failed_stream_id_ = first.stream_id;
      stream_queue_.clear();
      PublishPlaybackResult("pcm:" + std::to_string(first.stream_id), false);
      continue;
    }
    fcntl(input[1], F_SETPIPE_SZ, 4096);
    fcntl(input[1], F_SETFL, O_NONBLOCK);
    std::vector<std::string> args{"aplay",
                                  "-q",
                                  "-D",
                                  playback_device_,
                                  "-t",
                                  "raw",
                                  "-f",
                                  "S16_LE",
                                  "-r",
                                  std::to_string(first.sample_rate),
                                  "-c",
                                  std::to_string(first.channels),
                                  "--buffer-time=80000",
                                  "--period-time=20000"};
    std::vector<char *> argv;
    for (auto &arg : args)
      argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, input[0]);
    posix_spawn_file_actions_addclose(&actions, input[1]);
    pid_t pid = -1;
    const int spawned =
        posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    bool failed = spawned != 0;
    if (!failed) {
      std::lock_guard<std::mutex> lock(stream_mu_);
      stream_pid_ = pid;
      if (stop_ || stream_revision_ != revision)
        kill(pid, SIGTERM);
    }
    if (!failed)
      LOG_INFO("ROS PCM output started: stream={} {} Hz, {} channels",
               first.stream_id, first.sample_rate, first.channels);
    while (!failed && !stop_ && stream_revision_ == revision) {
      xiaocai_msgs::msg::AudioStream frame;
      {
        std::unique_lock<std::mutex> lock(stream_mu_);
        stream_cv_.wait(lock, [this, revision] {
          return stop_ || stream_revision_ != revision ||
                 !stream_queue_.empty() || stream_ended_;
        });
        if (stop_ || stream_revision_ != revision ||
            (stream_queue_.empty() && stream_ended_))
          break;
        frame = std::move(stream_queue_.front());
        stream_queue_.pop_front();
      }
      std::size_t offset = 0;
      while (offset < frame.data.size() && !stop_ &&
             stream_revision_ == revision) {
        const auto count = write(input[1], frame.data.data() + offset,
                                 frame.data.size() - offset);
        if (count > 0) {
          offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
          continue;
        } else if (count < 0 && errno == EAGAIN) {
          pollfd descriptor{input[1], POLLOUT, 0};
          poll(&descriptor, 1, 50);
        } else {
          failed = true;
          break;
        }
      }
    }
    close(input[1]);
    if (spawned == 0) {
      if (failed || stop_ || stream_revision_ != revision)
        kill(pid, SIGTERM);
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      failed = failed || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    }
    bool current;
    {
      std::lock_guard<std::mutex> lock(stream_mu_);
      stream_pid_ = -1;
      current = stream_revision_ == revision;
      if (failed && current) {
        failed_stream_id_ = first.stream_id;
        stream_queue_.clear();
      }
    }
    if (!stop_ && current)
      PublishPlaybackResult("pcm:" + std::to_string(first.stream_id), !failed);
  }
}

} // namespace xiaocai_robot
