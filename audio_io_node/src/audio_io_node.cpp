#include "audio_io_node/audio_io_node.hpp"
#include "common/common.hpp"
#include "common/mpv.hpp"
#include "defination/volume_action.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <thread>

namespace xiaocai_robot {

AudioIoNode::AudioIoNode()
    : Node("audio_io_node"), audio_io_config_(),
      log_manager_("audio_io_node", audio_io_config_.Logging()),
      volume_percent_(80), stop_(false), silenced_(true), capture_pid_(-1),
      playback_pid_(-1) {
  const auto &settings = audio_io_config_.Settings();
  capture_device_ =
      declare_parameter<std::string>("capture_device", settings.capture_device);
  playback_device_ = declare_parameter<std::string>("playback_device",
                                                    settings.playback_device);
  wake_ack_path_ =
      declare_parameter<std::string>("wake_ack_path", settings.wake_ack_path);
  volume_percent_ = declare_parameter<int>(
      "volume_percent", static_cast<int>(settings.volume_percent));
  int adc_pga_gain = declare_parameter<int>(
      "adc_pga_gain", static_cast<int>(settings.adc_pga_gain));
  int mic_boost_gain = declare_parameter<int>(
      "mic_boost_gain", static_cast<int>(settings.mic_boost_gain));
  InitPublishers();
  InitSubscribers();
  InitServers();
  Run({"amixer", "-c", "0", "set", "Playback Path", "SPK"});
  Run({"amixer", "-c", "0", "set", "Capture MIC Path", "Main Mic"});
  MakeSilence();
  // Create the shared ALSA software-gain control with an inaudible zero PCM.
  Run({"aplay", "-q", "-D", playback_device_, silence_});
  SetVolume(volume_percent_);
  Run({"amixer", "-c", "0", "set", "ADC PGA Gain",
       std::to_string(adc_pga_gain)});
  Run({"amixer", "-c", "0", "set", "MIC Boost Gain",
       std::to_string(mic_boost_gain)});
  MakeBeep();
  capture_ = std::thread([this] { CaptureLoop(); });
  playback_ = std::thread([this] { PlaybackLoop(); });
}

void AudioIoNode::InitPublishers() {
  int initialized_count = 0;
  mic_publisher_ = create_publisher<std_msgs::msg::UInt8MultiArray>(
      "/xiaocai_robot/audio/mic", 10);
  ++initialized_count;
  playing_publisher_ =
      create_publisher<std_msgs::msg::Bool>("/xiaocai_robot/audio/playing", 10);
  ++initialized_count;
  volume_state_publisher_ = create_publisher<std_msgs::msg::Int32>(
      "/xiaocai_robot/audio/volume/state", 10);
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
  volume_command_subscriber_ =
      create_subscription<xiaocai_msgs::msg::VolumeCommand>(
          "/xiaocai_robot/audio/volume/command", 10,
          [this](xiaocai_msgs::msg::VolumeCommand::SharedPtr message) {
            OnVolumeCommandCallback(message);
          });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void AudioIoNode::OnAudioPlayCallback(
    const xiaocai_msgs::msg::AudioPlay::SharedPtr message) {
  if (message->path.empty()) {
    common::MpvCommand({"stop"});
    silenced_.store(true);
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue_.clear();
    }
    const pid_t pid = playback_pid_.load();
    if (pid > 0)
      kill(pid, SIGTERM);
    cv_.notify_one();
    return;
  }
  if (message->path == wake_ack_path_)
    silenced_.store(false);
  if (silenced_.load())
    return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(message->path);
  }
  cv_.notify_one();
}

void AudioIoNode::OnVolumeCommandCallback(
    const xiaocai_msgs::msg::VolumeCommand::SharedPtr message) {
  const auto action = static_cast<common::VolumeAction>(message->action);
  if (action == common::VolumeAction::Up)
    SetVolume(volume_percent_ + 10, true);
  else if (action == common::VolumeAction::Down)
    SetVolume(volume_percent_ - 10, true);
  else if (action == common::VolumeAction::Set)
    SetVolume(message->percent, true);
}

void AudioIoNode::InitServers() {
  volume_get_server_ = create_service<xiaocai_msgs::srv::GetVolume>(
      "/xiaocai_robot/audio/get_volume",
      [this](const std::shared_ptr<xiaocai_msgs::srv::GetVolume::Request>,
             std::shared_ptr<xiaocai_msgs::srv::GetVolume::Response> response) {
        response->percent = volume_percent_;
      });
}

void AudioIoNode::SetVolume(int percent, bool notify) {
  percent = std::clamp(percent, 0, 150);
  // The RK809 driver rejects writes above 252 even though its mixer advertises
  // 255.
  int dac = percent == 0 ? 0 : std::min(252, 200 + (percent * 52 + 50) / 100);
  int status = Run({"amixer", "-c", "0", "cset", "numid=15",
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
  status = Run({"amixer", "-c", "0", "cset", "name='Xiaocai Output Gain'",
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

AudioIoNode::~AudioIoNode() {
  stop_ = true;
  cv_.notify_all();
  if (capture_pid_ > 0)
    kill(capture_pid_, SIGTERM);
  if (playback_pid_ > 0)
    kill(playback_pid_, SIGTERM);
  if (capture_.joinable())
    capture_.join();
  if (playback_.joinable())
    playback_.join();
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
    if (silenced_.load())
      continue;
    if (path == "__beep__")
      path = beep_;
    if (access(path.c_str(), R_OK) != 0) {
      LOG_ERROR("Missing audio file: {}", path);
      continue;
    }
    std_msgs::msg::Bool state;
    state.data = true;
    playing_publisher_->publish(state);
    // Pause music synchronously before the wake response or spoken reply.
    common::MpvCommand({"set_property", "pause", true});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    int status = 0;
    if (!silenced_.load()) {
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
        if (silenced_.load())
          kill(pid, SIGTERM);
        int child_status = 0;
        while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {
        }
        playback_pid_.store(-1);
        status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
      }
    }
    if (status && !silenced_.load())
      LOG_ERROR("aplay failed: {}", status);
    if (path.find("/tmp/xiaocai_tts/") == 0)
      unlink(path.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    state.data = false;
    playing_publisher_->publish(state);
  }
}

} // namespace xiaocai_robot
