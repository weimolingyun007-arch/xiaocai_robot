#pragma once
#include "xiaocai_cpp/common.hpp"
#include "xiaocai_msgs/srv/get_volume.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <signal.h>

class AudioIo : public rclcpp::Node {
public:
  AudioIo();
  ~AudioIo() override;
private:
  void make_beep();
  void make_silence();
  void set_volume(int percent,bool notify=false);
  void capture_loop();
  void playback_loop();
  std::string capture_device_,playback_device_,beep_,silence_,wake_ack_path_;
  int volume_percent_=120;
  std::atomic<bool> stop_{false},silenced_{true};
  std::atomic<pid_t> capture_pid_{-1},playback_pid_{-1};
  std::thread capture_,playback_;
  std::mutex mu_; std::condition_variable cv_; std::deque<std::string> queue_;
  rclcpp::Publisher<std_msgs::msg::UInt8MultiArray>::SharedPtr mic_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr playing_;
  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr volume_state_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_sub_;
  rclcpp::Subscription<xiaocai_msgs::msg::VolumeCommand>::SharedPtr volume_command_sub_;
  rclcpp::Service<xiaocai_msgs::srv::GetVolume>::SharedPtr volume_get_service_;
};
