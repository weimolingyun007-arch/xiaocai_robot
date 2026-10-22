#include "intent_node/intent_node.hpp"
#include "defination/intent_type.hpp"
#include "defination/memory_operation.hpp"
#include "defination/music_action.hpp"
#include "defination/volume_action.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace xiaocai_robot {

namespace {

constexpr auto kCorrectionContextTimeout = std::chrono::seconds(60);

std::string ResolveModelPath(const std::string &configured_path) {
  const std::filesystem::path model_path(configured_path);
  if (model_path.is_absolute())
    return model_path.string();
  const char *resource_root = std::getenv("XIAOCAI_RESOURCE_ROOT");
  if (resource_root != nullptr && resource_root[0] != '\0')
    return (std::filesystem::path(resource_root) / model_path).string();
  return model_path.string();
}

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
      readiness_publisher_(*this, "intent_node"),
      intent_classifier_(ResolveModelPath(intent_config_.Settings().model_path),
                         intent_config_.Settings().confidence_threshold,
                         intent_config_.Settings().correction_threshold),
      intent_feedback_store_(intent_config_.Settings().feedback_database_path),
      next_memory_request_id_(0), feedback_store_ready_(false),
      last_intent_text_(), last_predicted_label_(), last_predicted_score_(0.0),
      last_intent_at_() {
  if (intent_config_.Settings().feedback_collection_enabled) {
    std::string error;
    feedback_store_ready_ = intent_feedback_store_.Initialize(error);
    if (feedback_store_ready_)
      LOG_INFO("Intent feedback database initialized at {}",
               intent_config_.Settings().feedback_database_path);
    else
      LOG_ERROR("Unable to initialize intent feedback database: {}", error);
  }

  InitPublishers();
  InitSubscribers();
  InitServers();
  readiness_publisher_.MarkReady();
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
  turn_publisher_ = create_publisher<xiaocai_msgs::msg::ConversationTurn>(
      "/xiaocai_robot/dialogue/turn", 10);
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

void IntentNode::InitServers() {
  intent_correction_server_ = create_service<
      xiaocai_msgs::srv::SubmitIntentCorrection>(
      "/xiaocai_robot/intent/feedback",
      [this](
          std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Request>
              request,
          std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Response>
              response) {
        OnSubmitIntentCorrectionCallback(request, response);
      });
}

void IntentNode::OnTextCallback(std_msgs::msg::String::SharedPtr message) {
  IntentPrediction prediction = intent_classifier_.Classify(message->data);
  IntentResult result = std::move(prediction.result);
  if (result.normalized_text.empty())
    return;

  if (prediction.is_correction) {
    const std::string corrected_label = prediction.label;
    const bool has_recent_intent =
        !last_intent_text_.empty() &&
        std::chrono::steady_clock::now() - last_intent_at_ <=
            kCorrectionContextTimeout;
    if (has_recent_intent && !last_predicted_label_.empty() &&
        last_predicted_label_ != corrected_label && feedback_store_ready_) {
      std::string error;
      std::int64_t sample_id = 0;
      if (intent_feedback_store_.SaveCorrection(
              now().nanoseconds(), last_intent_text_, result.normalized_text,
              last_predicted_label_, corrected_label, last_predicted_score_,
              sample_id, error)) {
        LOG_INFO("Saved correction for '{}' as pending sample {}",
                 last_intent_text_, sample_id);
      } else {
        LOG_WARN("Unable to save intent correction: {}", error);
      }
    }

    LOG_INFO("Model identified a user intent correction: {} -> {} ({:.3f})",
             last_predicted_label_, corrected_label,
             prediction.correction_confidence);
  }

  last_intent_text_ = result.normalized_text;
  last_predicted_label_ = prediction.label;
  last_predicted_score_ = prediction.confidence;
  last_intent_at_ = std::chrono::steady_clock::now();

  xiaocai_msgs::msg::ConversationTurn turn;
  turn.timestamp = now();
  turn.expects_speech = !std::holds_alternative<MusicIntent>(result.payload);
  turn_publisher_->publish(turn);

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

void IntentNode::OnSubmitIntentCorrectionCallback(
    const std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Request>
        request,
    std::shared_ptr<xiaocai_msgs::srv::SubmitIntentCorrection::Response>
        response) {
  if (!intent_config_.Settings().feedback_collection_enabled) {
    response->error = "intent feedback collection is disabled";
    return;
  }
  if (!feedback_store_ready_) {
    response->error = "intent feedback database is unavailable";
    return;
  }
  if (request->timestamp.sec < 0 || request->timestamp.nanosec >= 1000000000U) {
    response->error = "timestamp is invalid";
    return;
  }

  const std::int64_t timestamp_ns =
      static_cast<std::int64_t>(request->timestamp.sec) * 1000000000LL +
      request->timestamp.nanosec;
  std::string error;
  response->accepted = intent_feedback_store_.SaveCorrection(
      timestamp_ns, request->text, request->correction_text,
      request->predicted_label, request->corrected_label,
      request->predicted_score, response->sample_id, error);
  if (!response->accepted) {
    response->error = std::move(error);
    LOG_WARN("Rejected intent correction sample: {}", response->error);
    return;
  }

  LOG_INFO("Saved pending intent correction sample {}", response->sample_id);
}

} // namespace xiaocai_robot
