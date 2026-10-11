#pragma once
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/memory_result.hpp"
#include "xiaocai_msgs/msg/reminder.hpp"
#include <rclcpp/rclcpp.hpp>
#include <sqlite3.h>
#include <filesystem>
#include <string>

class Memory:public rclcpp::Node {
public:
  Memory();
  ~Memory()override;
private:
  void sql(const char *query);
  sqlite3_stmt *prepare(const char *query);
  void command(const xiaocai_msgs::msg::MemoryCommand &message);
  void check_due();
  sqlite3 *db_=nullptr;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryResult>::SharedPtr result_;
  rclcpp::Publisher<xiaocai_msgs::msg::Reminder>::SharedPtr due_;
  rclcpp::Subscription<xiaocai_msgs::msg::MemoryCommand>::SharedPtr sub_;
  rclcpp::TimerBase::SharedPtr timer_;
};
