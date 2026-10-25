#include "dialogue_node/dialogue_node.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

namespace xiaocai_robot {

DialogueNode::DialogueNode()
    : Node("dialogue_node"), dialogue_config_(),
      log_manager_("dialogue_node", dialogue_config_.Logging()),
      readiness_publisher_(*this, "dialogue_node"), dialogue_controller_() {
  InitPublishers();
  InitSubscribers();
  InitController();
  InitTimers();
  PublishState(common::RobotState::Idle);
  readiness_publisher_.MarkReady();
}

DialogueNode::~DialogueNode() { dialogue_controller_.reset(); }

void DialogueNode::InitPublishers() {
  int initialized_count = 0;
  tts_publisher_ = create_publisher<xiaocai_msgs::msg::TtsRequest>(
      "/xiaocai_robot/tts/request", 10);
  ++initialized_count;
  state_publisher_ = create_publisher<xiaocai_msgs::msg::RobotState>(
      "/xiaocai_robot/system/state", rclcpp::QoS(1).transient_local());
  ++initialized_count;
  memory_publisher_ = create_publisher<xiaocai_msgs::msg::MemoryCommand>(
      "/xiaocai_robot/memory/command", 10);
  ++initialized_count;
  volume_command_publisher_ =
      create_publisher<xiaocai_msgs::msg::VolumeCommand>(
          "/xiaocai_robot/audio/volume_command", 10);
  ++initialized_count;
  music_command_publisher_ = create_publisher<xiaocai_msgs::msg::MusicCommand>(
      "/xiaocai_robot/music/command", 10);
  ++initialized_count;
  conversation_publisher_ =
      create_publisher<xiaocai_msgs::msg::ConversationState>(
          "/xiaocai_robot/dialogue/state", rclcpp::QoS(1).transient_local());
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void DialogueNode::InitSubscribers() {
  int initialized_count = 0;
  dialogue_command_subscriber_ =
      create_subscription<xiaocai_msgs::msg::DialogueCommand>(
          "/xiaocai_robot/dialogue/command", 10,
          [this](xiaocai_msgs::msg::DialogueCommand::SharedPtr message) {
            OnDialogueCommandCallback(*message);
          });
  ++initialized_count;
  result_subscriber_ = create_subscription<xiaocai_msgs::msg::MemoryResult>(
      "/xiaocai_robot/memory/result", 10,
      [this](xiaocai_msgs::msg::MemoryResult::SharedPtr message) {
        OnMemoryResultCallback(*message);
      });
  ++initialized_count;
  due_subscriber_ = create_subscription<xiaocai_msgs::msg::Reminder>(
      "/xiaocai_robot/reminder/due", 10,
      [this](xiaocai_msgs::msg::Reminder::SharedPtr message) {
        OnReminderDueCallback(*message);
      });
  ++initialized_count;
  playing_subscriber_ = create_subscription<std_msgs::msg::Bool>(
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
  volume_state_subscriber_ = create_subscription<std_msgs::msg::Int32>(
      "/xiaocai_robot/audio/volume_state", rclcpp::QoS(1).transient_local(),
      [this](std_msgs::msg::Int32::SharedPtr message) {
        OnVolumeStateCallback(*message);
      });
  ++initialized_count;
  music_subscriber_ = create_subscription<xiaocai_msgs::msg::MusicState>(
      "/xiaocai_robot/music/state", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::MusicState::SharedPtr message) {
        OnMusicStateCallback(*message);
      });
  ++initialized_count;
  interrupt_subscriber_ = create_subscription<xiaocai_msgs::msg::AudioPlay>(
      "/xiaocai_robot/audio/play", 10,
      [this](xiaocai_msgs::msg::AudioPlay::SharedPtr message) {
        OnAudioPlayCallback(*message);
      });
  ++initialized_count;
  tts_result_subscriber_ = create_subscription<xiaocai_msgs::msg::TtsResult>(
      "/xiaocai_robot/tts/result", 100,
      [this](xiaocai_msgs::msg::TtsResult::SharedPtr message) {
        OnTtsResultCallback(*message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void DialogueNode::InitController() {
  auto settings = dialogue_config_.Settings();
  settings.api_url =
      declare_parameter<std::string>("api_url", settings.api_url);
  settings.model = declare_parameter<std::string>("model", settings.model);

  DialogueCallbacks callbacks;
  callbacks.speak = [this](const std::string &text, std::uint64_t request_id) {
    PublishSpeech(text, request_id);
  };
  callbacks.publish_conversation = [this](common::ConversationPhase phase,
                                          bool expect_reply) {
    PublishConversation(phase, expect_reply);
  };
  callbacks.publish_state = [this](common::RobotState state) {
    PublishState(state);
  };
  callbacks.publish_memory_command =
      [this](const DialogueMemoryCommand &command) {
        PublishMemoryCommand(command);
      };
  callbacks.publish_volume_command = [this](common::VolumeAction action,
                                            int percent) {
    PublishVolumeCommand(action, percent);
  };
  callbacks.publish_music_command = [this](common::MusicAction action,
                                           const std::string &query,
                                           std::uint64_t request_id) {
    PublishMusicCommand(action, query, request_id);
  };
  dialogue_controller_ = std::make_unique<DialogueController>(
      std::move(settings), log_manager_, std::move(callbacks));
}

void DialogueNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::seconds(3), [this] {
    dialogue_controller_->HandleTimerTick();
  });
}

void DialogueNode::PublishState(common::RobotState state) {
  xiaocai_msgs::msg::RobotState message;
  message.timestamp = now();
  message.state = static_cast<std::uint8_t>(state);
  state_publisher_->publish(message);
}

void DialogueNode::PublishSpeech(const std::string &text,
                                 std::uint64_t request_id) {
  xiaocai_msgs::msg::TtsRequest message;
  message.timestamp = now();
  message.request_id = request_id;
  message.text = text;
  tts_publisher_->publish(message);
}

void DialogueNode::PublishConversation(common::ConversationPhase phase,
                                       bool expect_reply) {
  xiaocai_msgs::msg::ConversationState message;
  message.timestamp = now();
  message.phase = static_cast<std::uint8_t>(phase);
  message.expect_reply = expect_reply;
  conversation_publisher_->publish(message);
}

void DialogueNode::PublishMemoryCommand(const DialogueMemoryCommand &command) {
  xiaocai_msgs::msg::MemoryCommand message;
  message.timestamp = now();
  message.operation = static_cast<std::uint8_t>(command.operation);
  message.request_id = command.request_id;
  message.content = command.content;
  message.delay_seconds = command.delay_seconds;
  message.enabled = command.enabled;
  memory_publisher_->publish(message);
}

void DialogueNode::PublishVolumeCommand(common::VolumeAction action,
                                        int percent) {
  xiaocai_msgs::msg::VolumeCommand message;
  message.timestamp = now();
  message.action = static_cast<std::uint8_t>(action);
  message.percent = percent;
  volume_command_publisher_->publish(message);
}

void DialogueNode::PublishMusicCommand(common::MusicAction action,
                                       const std::string &query,
                                       std::uint64_t request_id) {
  xiaocai_msgs::msg::MusicCommand message;
  message.timestamp = now();
  message.action = static_cast<std::uint8_t>(action);
  message.query = query;
  message.request_id = request_id;
  music_command_publisher_->publish(message);
}

void DialogueNode::OnDialogueCommandCallback(
    const xiaocai_msgs::msg::DialogueCommand &message) {
  dialogue_controller_->HandleIntent(
      static_cast<common::IntentType>(message.type), message.text);
}

void DialogueNode::OnMemoryResultCallback(
    const xiaocai_msgs::msg::MemoryResult &message) {
  DialogueMemoryResult result;
  result.request_id = message.request_id;
  result.operation = static_cast<common::MemoryOperation>(message.operation);
  result.ok = message.ok;
  result.error = message.error;
  result.facts = message.facts;
  result.content = message.content;
  result.deleted = message.deleted;
  result.enabled = message.enabled;
  dialogue_controller_->HandleMemoryResult(result);
}

void DialogueNode::OnReminderDueCallback(
    const xiaocai_msgs::msg::Reminder &message) {
  dialogue_controller_->HandleReminderDue(message.content);
}

void DialogueNode::OnAudioPlayingCallback(const std_msgs::msg::Bool &message) {
  dialogue_controller_->HandleAudioPlaying(message.data);
}

void DialogueNode::OnSpeechAwakeCallback(const std_msgs::msg::Bool &message) {
  dialogue_controller_->HandleSpeechAwake(message.data);
}

void DialogueNode::OnVolumeStateCallback(const std_msgs::msg::Int32 &message) {
  dialogue_controller_->HandleVolumeState(message.data);
}

void DialogueNode::OnMusicStateCallback(
    const xiaocai_msgs::msg::MusicState &message) {
  DialogueMusicStatus status;
  status.state = static_cast<common::MusicState>(message.state);
  status.title = message.title;
  status.position_seconds = message.position_seconds;
  status.duration_seconds = message.duration_seconds;
  status.error = message.error;
  status.request_id = message.request_id;
  dialogue_controller_->HandleMusicState(status);
}

void DialogueNode::OnAudioPlayCallback(
    const xiaocai_msgs::msg::AudioPlay &message) {
  dialogue_controller_->HandleAudioPlay(message.path);
}

void DialogueNode::OnTtsResultCallback(
    const xiaocai_msgs::msg::TtsResult &message) {
  dialogue_controller_->HandleTtsResult(message.request_id, message.success);
}

} // namespace xiaocai_robot
