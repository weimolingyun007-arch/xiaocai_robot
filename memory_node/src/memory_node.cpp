#include "memory_node/memory_node.hpp"
#include "defination/memory_operation.hpp"
#include <ctime>
#include <filesystem>
#include <rclcpp/rclcpp.hpp>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace xiaocai_robot {

MemoryNode::MemoryNode()
    : Node("memory_node"), memory_config_(),
      log_manager_("memory_node", memory_config_.Logging()) {
  std::string path = declare_parameter<std::string>(
      "db_path", memory_config_.Settings().db_path);
  std::filesystem::create_directories(
      std::filesystem::path(path).parent_path());
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
    throw std::runtime_error("Cannot open memory database");
  Sql("CREATE TABLE IF NOT EXISTS facts(id INTEGER PRIMARY KEY,content TEXT "
      "NOT NULL UNIQUE)");
  Sql("CREATE TABLE IF NOT EXISTS reminders(id INTEGER PRIMARY KEY,content "
      "TEXT NOT NULL,due_at INTEGER NOT NULL,delivered INTEGER NOT NULL "
      "DEFAULT 0)");
  Sql("CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT "
      "NULL)");
  InitPublishers();
  InitSubscribers();
  InitTimers();
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

MemoryNode::~MemoryNode() {
  if (db_)
    sqlite3_close(db_);
}

void MemoryNode::Sql(const char *query) {
  char *err = nullptr;
  if (sqlite3_exec(db_, query, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string e = err ? err : "SQL error";
    sqlite3_free(err);
    throw std::runtime_error(e);
  }
}

sqlite3_stmt *MemoryNode::Prepare(const char *query) {
  sqlite3_stmt *s = nullptr;
  if (sqlite3_prepare_v2(db_, query, -1, &s, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db_));
  return s;
}

void MemoryNode::OnMemoryCommandCallback(
    const xiaocai_msgs::msg::MemoryCommand &message) {
  xiaocai_msgs::msg::MemoryResult result;
  result.timestamp = now();
  result.operation = message.operation;
  result.request_id = message.request_id;
  result.ok = true;
  sqlite3_stmt *s = nullptr;
  try {
    const auto operation =
        static_cast<common::MemoryOperation>(message.operation);
    if (operation == common::MemoryOperation::SaveFact ||
        operation == common::MemoryOperation::ForgetFact) {
      if (message.content.empty())
        throw std::runtime_error("empty content");
      const bool forget = operation == common::MemoryOperation::ForgetFact;
      s = Prepare(forget ? "DELETE FROM facts WHERE content LIKE ?"
                         : "INSERT OR IGNORE INTO facts(content) VALUES(?)");
      std::string value =
          forget ? "%" + message.content + "%" : message.content;
      sqlite3_bind_text(s, 1, value.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(s) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
      if (forget)
        result.deleted = sqlite3_changes(db_);
    } else if (operation == common::MemoryOperation::ListFacts) {
      s = Prepare("SELECT content FROM facts ORDER BY id DESC LIMIT 10");
      while (sqlite3_step(s) == SQLITE_ROW)
        result.facts.emplace_back((const char *)sqlite3_column_text(s, 0));
    } else if (operation == common::MemoryOperation::AddReminder) {
      int delay = message.delay_seconds;
      if (message.content.empty() || delay < 1 || delay > 30 * 86400)
        throw std::runtime_error("invalid reminder");
      s = Prepare("INSERT INTO reminders(content,due_at) VALUES(?,?)");
      sqlite3_bind_text(s, 1, message.content.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(s, 2, time(nullptr) + delay);
      if (sqlite3_step(s) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
      result.content = message.content;
    } else if (operation == common::MemoryOperation::SetQuiet) {
      s = Prepare("INSERT INTO settings(key,value) VALUES('quiet',?) ON "
                  "CONFLICT(key) DO UPDATE SET value=excluded.value");
      sqlite3_bind_text(s, 1, message.enabled ? "1" : "0", -1, SQLITE_STATIC);
      if (sqlite3_step(s) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
      result.enabled = message.enabled;
    } else if (operation == common::MemoryOperation::GetQuiet) {
      s = Prepare("SELECT value FROM settings WHERE key='quiet'");
      result.enabled =
          sqlite3_step(s) == SQLITE_ROW &&
          std::string((const char *)sqlite3_column_text(s, 0)) == "1";
    } else
      throw std::runtime_error("unknown operation");
  } catch (const std::exception &e) {
    result.ok = false;
    result.error = e.what();
    LOG_ERROR("MemoryNode command failed: {}", e.what());
  }
  if (s)
    sqlite3_finalize(s);
  result_publisher_->publish(result);
}

void MemoryNode::CheckDue() {
  auto s = Prepare("SELECT id,content FROM reminders WHERE delivered=0 AND "
                   "due_at<=? ORDER BY due_at LIMIT 10");
  sqlite3_bind_int64(s, 1, time(nullptr));
  std::vector<std::pair<int, std::string>> rows;
  while (sqlite3_step(s) == SQLITE_ROW)
    rows.emplace_back(sqlite3_column_int(s, 0),
                      (const char *)sqlite3_column_text(s, 1));
  sqlite3_finalize(s);
  for (auto &row : rows) {
    s = Prepare("UPDATE reminders SET delivered=1 WHERE id=?");
    sqlite3_bind_int(s, 1, row.first);
    sqlite3_step(s);
    sqlite3_finalize(s);
    xiaocai_msgs::msg::Reminder reminder;
    reminder.timestamp = now();
    reminder.id = row.first;
    reminder.content = row.second;
    due_publisher_->publish(reminder);
  }
}

} // namespace xiaocai_robot
