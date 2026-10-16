#pragma once

#include "common/log_manager.hpp"
#include "defination/robot_state.hpp"
#include "display_node/display_config.hpp"
#include "display_node/display_window.hpp"
#include "display_node/lyrics_presenter.hpp"
#include "xiaocai_msgs/msg/ble_status.hpp"
#include "xiaocai_msgs/msg/music_command.hpp"
#include "xiaocai_msgs/msg/music_login.hpp"
#include "xiaocai_msgs/msg/music_lyrics.hpp"
#include "xiaocai_msgs/msg/music_state.hpp"
#include "xiaocai_msgs/msg/robot_state.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include "xiaocai_msgs/msg/wifi_status.hpp"
#include "xiaocai_msgs/srv/get_volume.hpp"
#include <std_msgs/msg/int32.hpp>

#include <memory>
#include <rclcpp/rclcpp.hpp>

namespace xiaocai_robot {

class DisplayNode : public rclcpp::Node {
public:
  DisplayNode();

private:
  // ROS communication and timer initialization.
  void InitPublishers();
  void InitSubscribers();
  void InitClients();
  void InitTimers();

  // ROS subscription callbacks.
  void OnRobotStateCallback(const xiaocai_msgs::msg::RobotState &message);
  void OnMusicStateCallback(const xiaocai_msgs::msg::MusicState &message);
  void OnMusicLyricsCallback(const xiaocai_msgs::msg::MusicLyrics &message);
  void OnMusicLoginCallback(const xiaocai_msgs::msg::MusicLogin &message);
  void OnVolumeStateCallback(const std_msgs::msg::Int32 &message);
  void OnWifiStatusCallback(const xiaocai_msgs::msg::WifiStatus &message);
  void OnBleStatusCallback(const xiaocai_msgs::msg::BleStatus &message);
  void PublishMusicCommand(common::MusicAction action);
  void PublishVolumeCommand(int percent);
  void RefreshVolume(double now);

  // Display state and X11 event handling.
  void ShowRobotState(common::RobotState state);
  void RefreshDisplay();
  void ProcessDisplayEvents();

  DisplayConfig display_config_;
  LogManager log_manager_;
  std::unique_ptr<DisplayWindow> display_window_;
  common::RobotState robot_state_;
  LyricsPresenter lyrics_presenter_;
  double last_clock_update_;
  double last_volume_query_{-5.0};
  bool volume_query_pending_{false};
  rclcpp::Publisher<xiaocai_msgs::msg::MusicCommand>::SharedPtr
      music_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      volume_command_publisher_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr
      volume_state_subscriber_;
  rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedPtr volume_client_;
  rclcpp::Subscription<xiaocai_msgs::msg::RobotState>::SharedPtr
      robot_state_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicState>::SharedPtr
      music_state_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicLyrics>::SharedPtr
      music_lyrics_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicLogin>::SharedPtr
      login_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::WifiStatus>::SharedPtr
      wifi_status_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::BleStatus>::SharedPtr
      ble_status_subscriber_;
  rclcpp::TimerBase::SharedPtr timer_;
};

} // namespace xiaocai_robot
