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
        OnTextCallback(message);
      });
}

void IntentNode::OnTextCallback(std_msgs::msg::String::SharedPtr message) {
  const IntentResult result = intent_rules_.Classify(message->data);
  if (result.normalized_text.empty()) return;

  xiaocai_msgs::msg::Intent intent_message;
  intent_message.intent = result.intent;
  intent_message.text = result.normalized_text;
  intent_message.volume_percent = result.volume_percent;
  intent_message.content = result.content;
  intent_message.delay_seconds = result.delay_seconds;
  intent_publisher_->publish(intent_message);

  LOG_INFO("{}: {}", result.intent, result.normalized_text);
}
