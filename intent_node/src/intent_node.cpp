#include "intent_node/intent_node.hpp"

#include <utility>

IntentNode::IntentNode()
    : Node("intent_node"), intent_config_(),
      log_manager_("intent_node", intent_config_.Logging()) {
  InitPublishers();
  InitSubscribers();
}

void IntentNode::InitPublishers() {
  intent_publisher_ = create_publisher<xiaocai_msgs::msg::Intent>(
      "/xiaocai_robot/intent", 10);
}

void IntentNode::InitSubscribers() {
  text_subscription_ = create_subscription<std_msgs::msg::String>(
      "/xiaocai_robot/speech/text", 10,
      [this](std_msgs::msg::String::SharedPtr message) {
        OnText(message->data);
      });
}

void IntentNode::OnText(std::string text) {
  const IntentResult result = intent_rules_.Classify(std::move(text));
  if (result.normalized_text.empty()) return;

  xiaocai_msgs::msg::Intent message;
  message.intent = result.intent;
  message.text = result.normalized_text;
  message.volume_percent = result.volume_percent;
  message.content = result.content;
  message.delay_seconds = result.delay_seconds;
  intent_publisher_->publish(message);

  LOG_INFO("{}: {}", result.intent, result.normalized_text);
}
