#include "display_node/display_node.hpp"
#include "defination/volume_action.hpp"

#include <algorithm>
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
      last_wifi_update_(0.0), last_clock_update_(0.0) {
  ShowRobotState(robot_state_);
  InitPublishers();
  InitSubscribers();
  InitClients();
  display_window_->SetMusicControlCallback(
      [this](common::MusicAction action) { PublishMusicCommand(action); });
  display_window_->SetVolumeControlCallback(
      [this](int percent) { PublishVolumeCommand(percent); });
  InitTimers();
}

void DisplayNode::InitPublishers() {
  int initialized_count = 0;
  music_command_publisher_ = create_publisher<xiaocai_msgs::msg::MusicCommand>(
      "/xiaocai_robot/music/command", 10);
  ++initialized_count;
  volume_command_publisher_ =
      create_publisher<xiaocai_msgs::msg::VolumeCommand>(
          "/xiaocai_robot/audio/volume/silent_command", 10);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
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
  volume_state_subscriber_ = create_subscription<std_msgs::msg::Int32>(
      "/xiaocai_robot/audio/volume/state", 10,
      [this](std_msgs::msg::Int32::SharedPtr message) {
        OnVolumeStateCallback(*message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void DisplayNode::InitClients() {
  volume_client_ = create_client<xiaocai_msgs::srv::GetVolume>(
      "/xiaocai_robot/audio/get_volume");
}

void DisplayNode::OnVolumeStateCallback(const std_msgs::msg::Int32 &message) {
  if (message.data >= 0 && message.data <= 150)
    display_window_->ShowVolume(message.data);
}

void DisplayNode::PublishMusicCommand(common::MusicAction action) {
  xiaocai_msgs::msg::MusicCommand message;
  message.timestamp = now();
  message.action = static_cast<std::uint8_t>(action);
  music_command_publisher_->publish(message);
  LOG_INFO("Music page button: {}", common::MusicActionName(action));
}

void DisplayNode::PublishVolumeCommand(int percent) {
  xiaocai_msgs::msg::VolumeCommand message;
  message.timestamp = now();
  message.action = static_cast<std::uint8_t>(common::VolumeAction::Set);
  message.percent = std::clamp(percent, 0, 150);
  volume_command_publisher_->publish(message);
}

void DisplayNode::RefreshVolume(double now) {
  if (volume_query_pending_ && now - last_volume_query_ > 3.0) {
    volume_client_->prune_pending_requests();
    volume_query_pending_ = false;
  }
  if (volume_query_pending_ || now - last_volume_query_ < 5.0 ||
      !volume_client_->service_is_ready())
    return;
  last_volume_query_ = now;
  volume_query_pending_ = true;
  auto request = std::make_shared<xiaocai_msgs::srv::GetVolume::Request>();
  request->timestamp = this->now();
  volume_client_->async_send_request(
      request,
      [this](
          rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedFuture future) {
        volume_query_pending_ = false;
        const auto percent = future.get()->percent;
        if (percent >= 0 && percent <= 150)
          display_window_->ShowVolume(percent);
      });
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
  else if (!display_window_->IsVolumeOverlayActive())
    ShowRobotState(robot_state_);
}

void DisplayNode::ProcessDisplayEvents() {
  const double now = SteadySeconds();
  RefreshVolume(now);
  if (now - last_wifi_update_ >= 1.0) {
    display_window_->ShowWifiStatus(wifi_status_reader_.Read());
    last_wifi_update_ = now;
  }
  if (now - last_clock_update_ >= 1.0) {
    display_window_->RefreshClock();
    last_clock_update_ = now;
  }
  RefreshDisplay();
  display_window_->ProcessEvents();
}

} // namespace xiaocai_robot
