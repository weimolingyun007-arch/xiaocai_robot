#pragma once

#include "common/log_manager.hpp"
#include "intent_node/intent_config.hpp"
#include "intent_node/intent_rules.hpp"
#include "xiaocai_msgs/msg/intent.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <string>

class IntentNode : public rclcpp::Node {
public:
  IntentNode();

private:
  void InitPublishers();
  void InitSubscribers();
  void OnTextCallback(std_msgs::msg::String::SharedPtr message);

  IntentConfig intent_config_;
  xc::LogManager log_manager_;
  IntentRules intent_rules_;
  rclcpp::Publisher<xiaocai_msgs::msg::Intent>::SharedPtr intent_publisher_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr text_subscription_;
};
