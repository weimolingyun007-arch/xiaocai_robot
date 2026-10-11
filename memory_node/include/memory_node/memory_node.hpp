#pragma once
#include "common/log_manager.hpp"
#include "memory_node/memory_config.hpp"
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/memory_result.hpp"
#include "xiaocai_msgs/msg/reminder.hpp"
#include <filesystem>
#include <rclcpp/rclcpp.hpp>
#include <sqlite3.h>
#include <string>

namespace xiaocai_robot {

class MemoryNode : public rclcpp::Node {
public:
  MemoryNode();
  ~MemoryNode() override;

private:
  void InitPublishers();
  void InitSubscribers();
  void InitTimers();
  void Sql(const char *query);
  sqlite3_stmt *Prepare(const char *query);
  void OnMemoryCommandCallback(const xiaocai_msgs::msg::MemoryCommand &message);
  void CheckDue();
  MemoryConfig memory_config_;
  LogManager log_manager_;
  sqlite3 *db_ = nullptr;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryResult>::SharedPtr
      result_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::Reminder>::SharedPtr due_publisher_;
  rclcpp::Subscription<xiaocai_msgs::msg::MemoryCommand>::SharedPtr
      memory_command_subscriber_;
  rclcpp::TimerBase::SharedPtr timer_;
};

} // namespace xiaocai_robot
