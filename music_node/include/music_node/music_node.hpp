#pragma once

#include "common/log_manager.hpp"
#include "music_node/music_config.hpp"
#include "music_node/music_controller.hpp"
#include "music_node/music_login_manager.hpp"
#include "music_node/music_lyrics_loader.hpp"
#include <xiaocai_msgs/msg/music_login.hpp>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <xiaocai_msgs/msg/audio_play.hpp>
#include <xiaocai_msgs/msg/conversation_state.hpp>
#include <xiaocai_msgs/msg/music_command.hpp>
#include <xiaocai_msgs/msg/music_lyrics.hpp>
#include <xiaocai_msgs/msg/music_state.hpp>

#include <memory>
#include <string>

namespace xiaocai_robot {

class MusicNode : public rclcpp::Node {
public:
  MusicNode();
  ~MusicNode() override;

private:
  // ROS communication initialization.
  void InitPublishers();
  void InitSubscribers();
  void InitTimers();

  // Music controller initialization.
  void InitController();

  // ROS message publishing.
  void PublishState(const MusicPlaybackStatus &status);
  void PublishSpeech(const std::string &text);
  void PublishLyrics(const MusicLyrics &lyrics);
  void PublishLoginStatus(const MusicLoginStatus &status);

  // ROS subscription callbacks.
  void OnMusicCommandCallback(const xiaocai_msgs::msg::MusicCommand &message);
  void OnAudioPlayCallback(const xiaocai_msgs::msg::AudioPlay &message);
  void OnAudioPlayingCallback(const std_msgs::msg::Bool &message);

  void OnSpeechAwakeCallback(const std_msgs::msg::Bool &message);
  void OnConversationStateCallback(
      const xiaocai_msgs::msg::ConversationState &message);

  MusicConfig music_config_;
  LogManager log_manager_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicState>::SharedPtr state_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicLogin>::SharedPtr login_publisher_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr tts_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicLyrics>::SharedPtr
      lyrics_publisher_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicCommand>::SharedPtr
      music_command_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlay>::SharedPtr
      play_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr voice_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr awake_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::ConversationState>::SharedPtr
      conversation_subscriber_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::unique_ptr<MusicController> music_controller_;
  std::unique_ptr<MusicLyricsLoader> lyrics_loader_;
  std::unique_ptr<MusicLoginManager> login_manager_;
};

} // namespace xiaocai_robot
