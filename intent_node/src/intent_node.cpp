#include "intent_node/intent_node.hpp"
#include "defination/intent_type.hpp"
#include "defination/memory_operation.hpp"
#include "defination/music_action.hpp"
#include "defination/volume_action.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

namespace xiaocai_robot {

namespace {

const char *IntentResultName(const IntentResult &result) {
  return std::visit(
      [](const auto &intent) -> const char * {
        using T = std::decay_t<decltype(intent)>;
        if constexpr (std::is_same_v<T, ChatIntent>) {
          return common::IntentName(common::IntentType::Chat);
        } else if constexpr (std::is_same_v<T, DialogueIntent>) {
          return common::IntentName(intent.type);
        } else if constexpr (std::is_same_v<T, MusicIntent>) {
          return common::MusicActionName(intent.action);
        } else if constexpr (std::is_same_v<T, VolumeIntent>) {
          switch (intent.action) {
          case common::VolumeAction::Up:
            return "volume_up";
          case common::VolumeAction::Down:
            return "volume_down";
          case common::VolumeAction::Set:
            return "volume_set";
          }
          return "unknown_volume_action";
        } else {
          switch (intent.operation) {
          case common::MemoryOperation::SaveFact:
            return "memory_save";
          case common::MemoryOperation::ForgetFact:
            return "memory_forget";
          case common::MemoryOperation::ListFacts:
            return "memory_list";
          case common::MemoryOperation::AddReminder:
            return "reminder_create";
          case common::MemoryOperation::SetQuiet:
          case common::MemoryOperation::GetQuiet:
            return "unknown_memory_operation";
          }
          return "unknown_memory_operation";
        }
      },
      result.payload);
}

} // namespace

IntentNode::IntentNode()
    : Node("intent_node"), intent_config_(),
      log_manager_("intent_node", intent_config_.Logging()),
      next_memory_request_id_(0) {
  InitPublishers();
  InitSubscribers();
}

void IntentNode::InitPublishers() {
  int initialized_count = 0;
  intent_publisher_ =
      create_publisher<xiaocai_msgs::msg::Intent>("/xiaocai_robot/intent", 10);
  ++initialized_count;
  music_command_publisher_ = create_publisher<xiaocai_msgs::msg::MusicCommand>(
      "/xiaocai_robot/music/command", 10);
  ++initialized_count;
  volume_command_publisher_ =
      create_publisher<xiaocai_msgs::msg::VolumeCommand>(
          "/xiaocai_robot/audio/volume/command", 10);
  ++initialized_count;
  memory_command_publisher_ =
      create_publisher<xiaocai_msgs::msg::MemoryCommand>(
          "/xiaocai_robot/memory/command", 10);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void IntentNode::InitSubscribers() {
  int initialized_count = 0;
  text_subscriber_ = create_subscription<std_msgs::msg::String>(
      "/xiaocai_robot/speech/text", 10,
      [this](std_msgs::msg::String::SharedPtr message) {
        OnTextCallback(message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void IntentNode::OnTextCallback(std_msgs::msg::String::SharedPtr message) {
  const IntentResult result = intent_rules_.Classify(message->data);
  if (result.normalized_text.empty())
    return;

  std::visit(
      [this, &result](const auto &intent) {
        using T = std::decay_t<decltype(intent)>;
        if constexpr (std::is_same_v<T, ChatIntent>) {
          xiaocai_msgs::msg::Intent message;
          message.timestamp = now();
          message.type = static_cast<std::uint8_t>(common::IntentType::Chat);
          message.text = result.normalized_text;
          intent_publisher_->publish(message);
        } else if constexpr (std::is_same_v<T, DialogueIntent>) {
          xiaocai_msgs::msg::Intent message;
          message.timestamp = now();
          message.type = static_cast<std::uint8_t>(intent.type);
          message.text = result.normalized_text;
          intent_publisher_->publish(message);
        } else if constexpr (std::is_same_v<T, MusicIntent>) {
          xiaocai_msgs::msg::MusicCommand message;
          message.timestamp = now();
          message.action = static_cast<std::uint8_t>(intent.action);
          message.query = intent.query;
          music_command_publisher_->publish(message);
        } else if constexpr (std::is_same_v<T, VolumeIntent>) {
          xiaocai_msgs::msg::VolumeCommand message;
          message.timestamp = now();
          message.action = static_cast<std::uint8_t>(intent.action);
          message.percent = intent.percent;
          volume_command_publisher_->publish(message);
        } else if constexpr (std::is_same_v<T, MemoryIntent>) {
          xiaocai_msgs::msg::MemoryCommand message;
          message.timestamp = now();
          message.operation = static_cast<std::uint8_t>(intent.operation);
          message.request_id =
              "intent-" + std::to_string(++next_memory_request_id_);
          message.content = intent.content;
          message.delay_seconds = intent.delay_seconds;
          memory_command_publisher_->publish(message);
        }
      },
      result.payload);

  LOG_INFO("{}: {}", IntentResultName(result), result.normalized_text);
}

} // namespace xiaocai_robot
