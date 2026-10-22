#pragma once
#include "common/log_manager.hpp"
#include "common/node_readiness_publisher.hpp"
#include "memory_node/memory_config.hpp"
#include "memory_node/memory_manager.hpp"
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/memory_result.hpp"
#include "xiaocai_msgs/msg/reminder.hpp"
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <string>

namespace xiaocai_robot {

class MemoryNode : public rclcpp::Node {
public:
  MemoryNode();

private:
  void InitPublishers();
  void InitSubscribers();
  void InitTimers();
  void OnMemoryCommandCallback(const xiaocai_msgs::msg::MemoryCommand &message);
  void CheckDue();

  MemoryConfig memory_config_;
  LogManager log_manager_;
  common::NodeReadinessPublisher readiness_publisher_;
  std::unique_ptr<MemoryManager> memory_manager_;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryResult>::SharedPtr
      result_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::Reminder>::SharedPtr due_publisher_;
  rclcpp::Subscription<xiaocai_msgs::msg::MemoryCommand>::SharedPtr
      memory_command_subscriber_;
  rclcpp::TimerBase::SharedPtr timer_;
};

} // namespace xiaocai_robot
