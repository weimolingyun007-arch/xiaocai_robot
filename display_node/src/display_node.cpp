#include "display_node/display_node.hpp"

#include <chrono>
#include <cstdint>

namespace xiaocai_robot {
namespace {
double SteadySeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace

DisplayNode::DisplayNode()
    : Node("display_node"), display_config_(),
      log_manager_("display_node", display_config_.Logging()),
      display_window_(std::make_unique<DisplayWindow>(
          display_config_.AssetDirectory(),
          display_config_.Settings().font_family)),
      robot_state_(common::RobotState::Idle), lyrics_presenter_(),
      wifi_status_reader_(display_config_.Settings().wifi_interface),
      last_wifi_update_(0.0) {
  ShowRobotState(robot_state_);
  InitSubscribers();
  InitTimers();
}

void DisplayNode::InitSubscribers() {
  int initialized_count = 0;
  robot_state_subscriber_ = create_subscription<xiaocai_msgs::msg::RobotState>(
      "/xiaocai_robot/state", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::RobotState::SharedPtr message) {
        OnRobotStateCallback(*message);
      });
  ++initialized_count;
  music_state_subscriber_ = create_subscription<xiaocai_msgs::msg::MusicState>(
      "/xiaocai_robot/music/state", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::MusicState::SharedPtr message) {
        OnMusicStateCallback(*message);
      });
  ++initialized_count;
  music_lyrics_subscriber_ =
      create_subscription<xiaocai_msgs::msg::MusicLyrics>(
          "/xiaocai_robot/music/lyrics", rclcpp::QoS(1).transient_local(),
          [this](xiaocai_msgs::msg::MusicLyrics::SharedPtr message) {
            OnMusicLyricsCallback(*message);
          });
  ++initialized_count;
  login_subscriber_ = create_subscription<xiaocai_msgs::msg::MusicLogin>(
      "/xiaocai_robot/music/login", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::MusicLogin::SharedPtr message) {
        OnMusicLoginCallback(*message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void DisplayNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::milliseconds(100),
                             [this] { ProcessDisplayEvents(); });
}

void DisplayNode::OnRobotStateCallback(
    const xiaocai_msgs::msg::RobotState &message) {
  if (message.state > static_cast<std::uint8_t>(common::RobotState::Muted)) {
    LOG_WARN("Ignoring unknown robot state value {}", message.state);
    return;
  }
  robot_state_ = static_cast<common::RobotState>(message.state);
  lyrics_presenter_.HandleRobotState(robot_state_);
  RefreshDisplay();
}

void DisplayNode::OnMusicStateCallback(
    const xiaocai_msgs::msg::MusicState &message) {
  if (!lyrics_presenter_.HandleMusicState(
          {static_cast<common::MusicState>(message.state), message.song_id,
           message.title, message.artist, message.position_seconds,
           message.duration_seconds},
          SteadySeconds())) {
    LOG_WARN("Ignoring invalid music display state");
    return;
  }
  RefreshDisplay();
}

void DisplayNode::OnMusicLyricsCallback(
    const xiaocai_msgs::msg::MusicLyrics &message) {
  if (!lyrics_presenter_.HandleLyrics({message.song_id, message.loading,
                                       message.start_seconds, message.lines})) {
    LOG_WARN("Ignoring invalid music lyrics timeline");
    return;
  }
  RefreshDisplay();
}

void DisplayNode::OnMusicLoginCallback(
    const xiaocai_msgs::msg::MusicLogin &message) {
  if (message.url.size() > 512 || message.error.size() > 1024 ||
      (!message.url.empty() &&
       message.url.rfind("https://163cn.tv/", 0) != 0)) {
    LOG_WARN("Ignoring invalid music login display state");
    return;
  }
  display_window_->ShowLogin(message.required, message.loading, message.url,
                             message.error);
  RefreshDisplay();
}

void DisplayNode::ShowRobotState(common::RobotState state) {
  const char *image_name = nullptr;
  switch (state) {
  case common::RobotState::Idle:
    image_name = "idle";
    break;
  case common::RobotState::Listening:
    image_name = "listening";
    break;
  case common::RobotState::Thinking:
    image_name = "thinking";
    break;
  case common::RobotState::Speaking:
    image_name = "speaking";
    break;
  case common::RobotState::Quiet:
    image_name = "quiet";
    break;
  case common::RobotState::Muted:
    image_name = "muted";
    break;
  }
  if (image_name)
    display_window_->ShowImage(image_name);
}

void DisplayNode::RefreshDisplay() {
  const auto music = lyrics_presenter_.CurrentView(SteadySeconds());
  if (music)
    display_window_->ShowMusic(*music);
  else
    ShowRobotState(robot_state_);
}

void DisplayNode::ProcessDisplayEvents() {
  const double now = SteadySeconds();
  if (now - last_wifi_update_ >= 2.0) {
    display_window_->ShowWifiStatus(wifi_status_reader_.Read());
    last_wifi_update_ = now;
  }
  RefreshDisplay();
  display_window_->ProcessEvents();
}

} // namespace xiaocai_robot
