#include "memory_node/memory_node.hpp"
#include "defination/memory_operation.hpp"

#include <chrono>
#include <rclcpp/rclcpp.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace xiaocai_robot {

MemoryNode::MemoryNode()
    : Node("memory_node"), memory_config_(),
      log_manager_("memory_node", memory_config_.Logging()),
      readiness_publisher_(*this, "memory_node") {
  const std::string path = declare_parameter<std::string>(
      "db_path", memory_config_.Settings().db_path);
  memory_manager_ = std::make_unique<MemoryManager>(path);
  InitPublishers();
  InitSubscribers();
  InitTimers();
  readiness_publisher_.MarkReady();
}

void MemoryNode::InitPublishers() {
  int initialized_count = 0;
  result_publisher_ = create_publisher<xiaocai_msgs::msg::MemoryResult>(
      "/xiaocai_robot/memory/result", 10);
  ++initialized_count;
  due_publisher_ = create_publisher<xiaocai_msgs::msg::Reminder>(
      "/xiaocai_robot/reminder/due", 10);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void MemoryNode::InitSubscribers() {
  int initialized_count = 0;
  memory_command_subscriber_ =
      create_subscription<xiaocai_msgs::msg::MemoryCommand>(
          "/xiaocai_robot/memory/command", 10,
          [this](xiaocai_msgs::msg::MemoryCommand::SharedPtr message) {
            OnMemoryCommandCallback(*message);
          });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void MemoryNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::seconds(1), [this] { CheckDue(); });
}

void MemoryNode::OnMemoryCommandCallback(
    const xiaocai_msgs::msg::MemoryCommand &message) {
  const auto operation =
      static_cast<common::MemoryOperation>(message.operation);
  const auto command_result = memory_manager_->HandleCommand(
      operation, message.content, message.delay_seconds, message.enabled);

  xiaocai_msgs::msg::MemoryResult result;
  result.timestamp = now();
  result.operation = message.operation;
  result.request_id = message.request_id;
  result.ok = command_result.ok;
  result.error = command_result.error;
  result.facts = command_result.facts;
  result.content = command_result.content;
  result.deleted = command_result.deleted;
  result.enabled = command_result.enabled;
  if (!result.ok)
    LOG_ERROR("MemoryNode command failed: {}", result.error);
  result_publisher_->publish(result);
}

void MemoryNode::CheckDue() {
  try {
    for (const auto &due_reminder : memory_manager_->TakeDueReminders()) {
      xiaocai_msgs::msg::Reminder reminder;
      reminder.timestamp = now();
      reminder.id = due_reminder.id;
      reminder.content = due_reminder.content;
      due_publisher_->publish(reminder);
    }
  } catch (const std::exception &error) {
    LOG_ERROR("Failed to check due reminders: {}", error.what());
  }
}

} // namespace xiaocai_robot
