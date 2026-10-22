#pragma once

#include "xiaocai_msgs/msg/node_readiness.hpp"

#include <chrono>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <utility>

namespace xiaocai_robot::common {

class NodeReadinessPublisher {
public:
  NodeReadinessPublisher(rclcpp::Node &node, std::string node_name)
      : node_(node), node_name_(std::move(node_name)), ready_(false),
        publisher_(node_.create_publisher<xiaocai_msgs::msg::NodeReadiness>(
            "/xiaocai_robot/system/readiness",
            rclcpp::QoS(1).transient_local())),
        timer_(node_.create_wall_timer(std::chrono::seconds(1),
                                       [this] { PublishIfReady(); })) {}

  void MarkReady() {
    ready_ = true;
    Publish();
  }

private:
  void PublishIfReady() {
    if (ready_)
      Publish();
  }

  void Publish() {
    xiaocai_msgs::msg::NodeReadiness message;
    message.timestamp = node_.now();
    message.node_name = node_name_;
    message.ready = true;
    publisher_->publish(message);
  }

  rclcpp::Node &node_;
  std::string node_name_;
  bool ready_;
  rclcpp::Publisher<xiaocai_msgs::msg::NodeReadiness>::SharedPtr publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
};

} // namespace xiaocai_robot::common
