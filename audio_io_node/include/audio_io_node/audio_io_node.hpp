#pragma once
#include "audio_io_node/audio_io_config.hpp"
#include "common/common.hpp"
#include "common/log_manager.hpp"
#include "common/node_readiness_publisher.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include "xiaocai_msgs/msg/audio_playback_result.hpp"
#include "xiaocai_msgs/msg/audio_stream.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include "xiaocai_msgs/srv/get_volume.hpp"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <signal.h>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/empty.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <thread>

namespace xiaocai_robot {

class AudioIoNode : public rclcpp::Node {
public:
  AudioIoNode();
  ~AudioIoNode() override;

private:
  // ROS communication initialization.
  void InitPublishers();
  void InitSubscribers();
  void InitServers();

  // ROS message publishing.
  void PublishPlaybackResult(const std::string &path, bool success);

  // ROS subscription callbacks.
  void
  OnAudioPlayCallback(const xiaocai_msgs::msg::AudioPlay::SharedPtr message);
  void OnStartupMusicCallback(const std_msgs::msg::Empty::SharedPtr message);
  void OnAudioStreamCallback(
      const xiaocai_msgs::msg::AudioStream::SharedPtr message);
  void OnVolumeCommandCallback(
      const xiaocai_msgs::msg::VolumeCommand::SharedPtr message,
      bool notify = true);

  // Playback configuration and PCM helpers.
  void SetVolume(int percent, bool notify = false);
  void MakeBeep();
  void MakeSilence();

  // Audio worker loops.
  void CaptureLoop();
  void PlaybackLoop();
  void StreamPlaybackLoop();
  void CancelAudioStream();

  AudioIoConfig audio_io_config_;
  LogManager log_manager_;
  common::NodeReadinessPublisher readiness_publisher_;
  std::string capture_device_, playback_device_, beep_, silence_,
      wake_ack_path_, startup_music_path_;
  int volume_percent_;
  bool startup_music_queued_{false};
  std::atomic<bool> stop_, silenced_;
  std::atomic<pid_t> capture_pid_, playback_pid_, stream_pid_;
  std::thread capture_, playback_;
  std::thread stream_playback_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  std::mutex stream_mu_;
  std::condition_variable stream_cv_;
  std::deque<xiaocai_msgs::msg::AudioStream> stream_queue_;
  std::uint64_t stream_id_{0}, stream_sequence_{0}, failed_stream_id_{0};
  std::uint64_t stream_dropped_{0};
  std::uint32_t stream_sample_rate_{0};
  std::uint8_t stream_channels_{0};
  bool stream_ended_{false};
  std::atomic<std::uint64_t> stream_revision_{0};
  rclcpp::Publisher<std_msgs::msg::UInt8MultiArray>::SharedPtr mic_publisher_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr playing_publisher_;
  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr volume_state_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::AudioPlaybackResult>::SharedPtr
      playback_result_publisher_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlay>::SharedPtr
      play_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Empty>::SharedPtr
      startup_music_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioStream>::SharedPtr
      stream_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      volume_command_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      silent_volume_command_subscriber_;
  rclcpp::Service<xiaocai_msgs::srv::GetVolume>::SharedPtr volume_get_server_;
};

} // namespace xiaocai_robot
