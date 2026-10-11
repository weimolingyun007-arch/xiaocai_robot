#pragma once
#include "xiaocai_cpp/common.hpp"
#include "defination/music_state.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <xiaocai_msgs/msg/audio_play.hpp>
#include <xiaocai_msgs/msg/intent.hpp>
#include <xiaocai_msgs/msg/music_state.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class Music : public rclcpp::Node {
public:
  Music();
  ~Music() override;
private:
  struct Job { std::string intent, content; std::uint64_t revision; std::string request_id; };
  struct Result { bool ok=false; xc::json data; };
  void on_intent(const xiaocai_msgs::msg::Intent &message);
  void on_audio_play(const std::string &path);
  void work();
  void execute(const Job &job);
  Result cli(const std::vector<std::string> &arguments, std::uint64_t revision);
  bool current(std::uint64_t revision) const;
  bool wait_for_speech(std::uint64_t revision);
  void fail(const Job &job, const std::string &message);
  void update_state();
  void maintain_backend();
  void set_state(xiaocai_robot::common::MusicState state, const std::string &error={},const std::string &request_id={});
  std::string cli_path_,socket_path_,default_keyword_,wake_ack_path_,audio_device_;
  std::atomic<bool> stop_{false},silenced_{false},voice_playing_{false},busy_{false};
  std::atomic<double> last_voice_at_{0};
  std::atomic<std::uint64_t> revision_{0};
  std::mutex jobs_mutex_,state_mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  std::thread worker_;
  int backend_fd_=-1;
  xiaocai_msgs::msg::MusicState state_message_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicState>::SharedPtr state_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr tts_;
  rclcpp::Subscription<xiaocai_msgs::msg::Intent>::SharedPtr intent_sub_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlay>::SharedPtr play_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr voice_sub_;
  rclcpp::TimerBase::SharedPtr timer_;
};
