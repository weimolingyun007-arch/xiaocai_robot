#pragma once

#include "common/log_manager.hpp"
#include "defination/robot_state.hpp"
#include "display_node/display_config.hpp"
#include "display_node/display_window.hpp"
#include "display_node/lyrics_presenter.hpp"
#include "display_node/wifi_status_reader.hpp"
#include "xiaocai_msgs/msg/music_login.hpp"
#include "xiaocai_msgs/msg/music_lyrics.hpp"
#include "xiaocai_msgs/msg/music_state.hpp"
#include "xiaocai_msgs/msg/robot_state.hpp"

#include <memory>
#include <rclcpp/rclcpp.hpp>

namespace xiaocai_robot {

class DisplayNode : public rclcpp::Node {
public:
  DisplayNode();

private:
  // ROS communication and timer initialization.
  void InitSubscribers();
  void InitTimers();

  // ROS subscription callbacks.
  void OnRobotStateCallback(const xiaocai_msgs::msg::RobotState &message);
  void OnMusicStateCallback(const xiaocai_msgs::msg::MusicState &message);
  void OnMusicLyricsCallback(const xiaocai_msgs::msg::MusicLyrics &message);
  void OnMusicLoginCallback(const xiaocai_msgs::msg::MusicLogin &message);

  // Display state and X11 event handling.
  void ShowRobotState(common::RobotState state);
  void RefreshDisplay();
  void ProcessDisplayEvents();

  DisplayConfig display_config_;
  LogManager log_manager_;
  std::unique_ptr<DisplayWindow> display_window_;
  common::RobotState robot_state_;
  LyricsPresenter lyrics_presenter_;
  WifiStatusReader wifi_status_reader_;
  double last_wifi_update_;
  rclcpp::Subscription<xiaocai_msgs::msg::RobotState>::SharedPtr
      robot_state_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicState>::SharedPtr
      music_state_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicLyrics>::SharedPtr
      music_lyrics_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicLogin>::SharedPtr
      login_subscriber_;
  rclcpp::TimerBase::SharedPtr timer_;
};

} // namespace xiaocai_robot
