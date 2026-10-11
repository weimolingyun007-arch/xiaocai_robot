#pragma once
#include "xiaocai_cpp/common.hpp"
#include "xiaocai_msgs/msg/intent.hpp"
#include <rclcpp/rclcpp.hpp>
#include <regex>

class Intent : public rclcpp::Node {
public:
  Intent();
private:
  void on_text(std::string text);
  rclcpp::Publisher<xiaocai_msgs::msg::Intent>::SharedPtr pub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_;
};
