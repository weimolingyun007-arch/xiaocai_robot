#include "music_node/music_node.hpp"

#include "common/common.hpp"
#include "defination/music_action.hpp"

#include <chrono>
#include <cstdint>

namespace xiaocai_robot {

MusicNode::MusicNode()
    : Node("music_node"), music_config_(),
      log_manager_("music_node", music_config_.Logging()),
      readiness_publisher_(*this, "music_node") {
  InitPublishers();
  InitSubscribers();
  InitTimers();
  InitController();
  readiness_publisher_.MarkReady();
}

MusicNode::~MusicNode() {
  timer_.reset();
  music_controller_.reset();
  login_manager_.reset();
  lyrics_loader_.reset();
}

void MusicNode::InitPublishers() {
  int initialized_count = 0;
  state_publisher_ = create_publisher<xiaocai_msgs::msg::MusicState>(
      "/xiaocai_robot/music/state", rclcpp::QoS(1).transient_local());
  ++initialized_count;
  tts_publisher_ =
      create_publisher<std_msgs::msg::String>("/xiaocai_robot/tts/text", 10);
  ++initialized_count;
  lyrics_publisher_ = create_publisher<xiaocai_msgs::msg::MusicLyrics>(
      "/xiaocai_robot/music/lyrics", rclcpp::QoS(1).transient_local());
  ++initialized_count;
  login_publisher_ = create_publisher<xiaocai_msgs::msg::MusicLogin>(
      "/xiaocai_robot/music/login", rclcpp::QoS(1).transient_local());
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void MusicNode::InitSubscribers() {
  int initialized_count = 0;
  music_command_subscriber_ =
      create_subscription<xiaocai_msgs::msg::MusicCommand>(
          "/xiaocai_robot/music/command", 10,
          [this](xiaocai_msgs::msg::MusicCommand::SharedPtr message) {
            OnMusicCommandCallback(*message);
          });
  ++initialized_count;
  play_subscriber_ = create_subscription<xiaocai_msgs::msg::AudioPlay>(
      "/xiaocai_robot/audio/play", 10,
      [this](xiaocai_msgs::msg::AudioPlay::SharedPtr message) {
        OnAudioPlayCallback(*message);
      });
  ++initialized_count;
  voice_subscriber_ = create_subscription<std_msgs::msg::Bool>(
      "/xiaocai_robot/audio/playing", 10,
      [this](std_msgs::msg::Bool::SharedPtr message) {
        OnAudioPlayingCallback(*message);
      });
  ++initialized_count;
  awake_subscriber_ = create_subscription<std_msgs::msg::Bool>(
      "/xiaocai_robot/speech/awake", 10,
      [this](std_msgs::msg::Bool::SharedPtr message) {
        OnSpeechAwakeCallback(*message);
      });
  ++initialized_count;
  conversation_subscriber_ =
      create_subscription<xiaocai_msgs::msg::ConversationState>(
          "/xiaocai_robot/dialogue/state", rclcpp::QoS(1).transient_local(),
          [this](xiaocai_msgs::msg::ConversationState::SharedPtr message) {
            OnConversationStateCallback(*message);
          });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void MusicNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::milliseconds(500), [this] {
    if (music_controller_)
      music_controller_->HandleStateUpdate();
  });
}

void MusicNode::InitController() {
  lyrics_loader_ = std::make_unique<MusicLyricsLoader>(
      music_config_.Settings().cli_path, log_manager_,
      [this](const MusicLyrics &lyrics) { PublishLyrics(lyrics); });
  PublishLyrics({});
  music_controller_ = std::make_unique<MusicController>(
      music_config_.Settings(), log_manager_,
      [this](const MusicPlaybackStatus &status) { PublishState(status); },
      [this](const std::string &text) { PublishSpeech(text); },
      [this] {
        if (login_manager_)
          login_manager_->RequestLogin();
      });
  PublishLoginStatus({});
  login_manager_ = std::make_unique<MusicLoginManager>(
      music_config_.Settings().cli_path, log_manager_,
      [this](const MusicLoginStatus &status) { PublishLoginStatus(status); },
      [this](const std::string &text) { PublishSpeech(text); });
  LOG_INFO("NetEase voice music ready; CLI: {}",
           music_config_.Settings().cli_path);
}

void MusicNode::PublishState(const MusicPlaybackStatus &status) {
  xiaocai_msgs::msg::MusicState message;
  message.timestamp = now();
  message.state = static_cast<std::uint8_t>(status.state);
  message.song_id = status.song_id;
  message.title = status.title;
  message.artist = status.artist;
  message.position_seconds = status.position_seconds;
  message.duration_seconds = status.duration_seconds;
  message.error = status.error;
  message.request_id = status.request_id;
  state_publisher_->publish(message);
  if (lyrics_loader_) {
    const bool active = status.state == common::MusicState::Playing ||
                        status.state == common::MusicState::Paused;
    lyrics_loader_->HandleTrack(
        active ? MusicTrack{status.song_id, status.title, status.artist}
               : MusicTrack{});
  }
}

void MusicNode::PublishSpeech(const std::string &text) {
  std_msgs::msg::String message;
  message.data = text;
  tts_publisher_->publish(message);
}

void MusicNode::PublishLyrics(const MusicLyrics &lyrics) {
  xiaocai_msgs::msg::MusicLyrics message;
  message.timestamp = now();
  message.song_id = lyrics.track.song_id;
  message.title = lyrics.track.title;
  message.artist = lyrics.track.artist;
  message.loading = lyrics.loading;
  for (const auto &line : lyrics.lines) {
    message.start_seconds.push_back(line.start_seconds);
    message.lines.push_back(line.text);
  }
  lyrics_publisher_->publish(message);
}

void MusicNode::PublishLoginStatus(const MusicLoginStatus &status) {
  xiaocai_msgs::msg::MusicLogin message;
  message.timestamp = now();
  message.required = status.required;
  message.loading = status.loading;
  message.url = status.url;
  message.error = status.error;
  login_publisher_->publish(message);
}

void MusicNode::OnMusicCommandCallback(
    const xiaocai_msgs::msg::MusicCommand &message) {
  music_controller_->HandleMusicCommand(
      static_cast<common::MusicAction>(message.action), message.query,
      message.request_id);
}

void MusicNode::OnAudioPlayCallback(
    const xiaocai_msgs::msg::AudioPlay &message) {
  music_controller_->HandleAudioPlay(message.path);
}

void MusicNode::OnAudioPlayingCallback(const std_msgs::msg::Bool &message) {
  music_controller_->HandleVoicePlaying(message.data);
}

void MusicNode::OnSpeechAwakeCallback(const std_msgs::msg::Bool &message) {
  music_controller_->HandleSpeechAwake(message.data);
}

void MusicNode::OnConversationStateCallback(
    const xiaocai_msgs::msg::ConversationState &message) {
  music_controller_->HandleConversation(
      static_cast<common::ConversationPhase>(message.phase));
}

} // namespace xiaocai_robot
