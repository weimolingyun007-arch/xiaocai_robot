#include "display_node/display_node.hpp"
#include "defination/volume_action.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace xiaocai_robot {
namespace {
constexpr std::array<const char *, 7> kRequiredNodes{
    "audio_io_node", "speech_input_node", "intent_node", "dialogue_node",
    "memory_node",   "tts_node",          "music_node"};
constexpr double kReadinessTimeoutSeconds = 3.0;

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
      last_clock_update_(0.0), system_ready_(false),
      startup_music_published_(false), music_page_override_until_(0.0) {
  display_window_->ShowStartup(StartupIndicators(SteadySeconds()),
                               SteadySeconds());
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
          "/xiaocai_robot/audio/volume_silent_command", 10);
  ++initialized_count;
  startup_music_publisher_ = create_publisher<std_msgs::msg::Empty>(
      "/xiaocai_robot/audio/startup_music", 10);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void DisplayNode::InitSubscribers() {
  int initialized_count = 0;
  robot_state_subscriber_ = create_subscription<xiaocai_msgs::msg::RobotState>(
      "/xiaocai_robot/system/state", rclcpp::QoS(1).transient_local(),
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
      "/xiaocai_robot/audio/volume_state", rclcpp::QoS(1).transient_local(),
      [this](std_msgs::msg::Int32::SharedPtr message) {
        OnVolumeStateCallback(*message);
      });
  ++initialized_count;
  wifi_status_subscriber_ = create_subscription<xiaocai_msgs::msg::WifiStatus>(
      "/xiaocai_robot/wifi/status", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::WifiStatus::SharedPtr message) {
        OnWifiStatusCallback(*message);
      });
  ++initialized_count;
  ble_status_subscriber_ = create_subscription<xiaocai_msgs::msg::BleStatus>(
      "/xiaocai_robot/ble/status", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::BleStatus::SharedPtr message) {
        OnBleStatusCallback(*message);
      });
  ++initialized_count;
  readiness_subscriber_ = create_subscription<xiaocai_msgs::msg::NodeReadiness>(
      "/xiaocai_robot/system/readiness", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::NodeReadiness::SharedPtr message) {
        OnReadinessCallback(*message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void DisplayNode::InitClients() {
  volume_client_ = create_client<xiaocai_msgs::srv::GetVolume>(
      "/xiaocai_robot/audio/get_volume");
}

void DisplayNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::milliseconds(50),
                             [this] { ProcessDisplayEvents(); });
}

void DisplayNode::OnRobotStateCallback(
    const xiaocai_msgs::msg::RobotState &message) {
  if (message.state > static_cast<std::uint8_t>(common::RobotState::Muted)) {
    LOG_WARN("Ignoring unknown robot state value {}", message.state);
    return;
  }
  robot_state_ = static_cast<common::RobotState>(message.state);
  if (robot_state_ == common::RobotState::Listening)
    music_page_override_until_ = 0.0;
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

void DisplayNode::OnVolumeStateCallback(const std_msgs::msg::Int32 &message) {
  if (message.data >= 0 && message.data <= 150)
    display_window_->ShowVolume(message.data);
}

void DisplayNode::OnWifiStatusCallback(
    const xiaocai_msgs::msg::WifiStatus &message) {
  display_window_->ShowWifiStatus(message);
}

void DisplayNode::OnBleStatusCallback(
    const xiaocai_msgs::msg::BleStatus &message) {
  display_window_->ShowBleStatus(message);
}

void DisplayNode::OnReadinessCallback(
    const xiaocai_msgs::msg::NodeReadiness &message) {
  if (std::find(kRequiredNodes.begin(), kRequiredNodes.end(),
                message.node_name) == kRequiredNodes.end())
    return;
  if (message.ready)
    readiness_received_at_[message.node_name] = SteadySeconds();
  else
    readiness_received_at_.erase(message.node_name);
}

void DisplayNode::PublishMusicCommand(common::MusicAction action) {
  if (action == common::MusicAction::Next ||
      action == common::MusicAction::Previous)
    music_page_override_until_ = SteadySeconds() + 55.0;
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

void DisplayNode::PublishStartupMusic() {
  startup_music_publisher_->publish(std_msgs::msg::Empty{});
  startup_music_published_ = true;
  LOG_INFO("Published startup music request");
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

bool DisplayNode::IsNodeReady(const std::string &node_name, double now) const {
  const auto found = readiness_received_at_.find(node_name);
  return found != readiness_received_at_.end() &&
         now - found->second <= kReadinessTimeoutSeconds;
}

bool DisplayNode::AreCoreNodesReady(double now) const {
  return std::all_of(kRequiredNodes.begin(), kRequiredNodes.end(),
                     [this, now](const char *node_name) {
                       return IsNodeReady(node_name, now);
                     });
}

std::array<bool, 3> DisplayNode::StartupIndicators(double now) const {
  return {IsNodeReady("audio_io_node", now) &&
              IsNodeReady("speech_input_node", now),
          IsNodeReady("intent_node", now),
          IsNodeReady("dialogue_node", now) && IsNodeReady("tts_node", now)};
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
  const double now = SteadySeconds();
  if (!system_ready_) {
    if (!AreCoreNodesReady(now)) {
      display_window_->ShowStartup(StartupIndicators(now), now);
      return;
    }
    system_ready_ = true;
    LOG_INFO("Core nodes are ready; switching to the main display");
    if (!startup_music_published_)
      PublishStartupMusic();
  }

  const auto music = lyrics_presenter_.CurrentView(now);
  const bool preserve_music_page = now < music_page_override_until_ &&
                                   music.has_value() &&
                                   robot_state_ == common::RobotState::Thinking;
  if (!display_window_->IsVolumeOverlayActive()) {
    if (robot_state_ != common::RobotState::Idle && !preserve_music_page) {
      ShowRobotState(robot_state_);
    } else if (music) {
      display_window_->ShowMusic(*music);
    } else {
      ShowRobotState(robot_state_);
    }
  }
}

void DisplayNode::ProcessDisplayEvents() {
  const double now = SteadySeconds();
  RefreshVolume(now);
  if (now - last_clock_update_ >= 1.0) {
    display_window_->RefreshClock();
    last_clock_update_ = now;
  }
  RefreshDisplay();
  display_window_->ProcessEvents();
}

} // namespace xiaocai_robot
