#pragma once

#include "defination/memory_operation.hpp"

#include <cstdint>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace xiaocai_robot {

struct MemoryCommandResult {
  bool ok{false};
  std::string error;
  std::vector<std::string> facts;
  std::string content;
  std::int32_t deleted{0};
  bool enabled{false};
};

struct DueReminder {
  std::int32_t id{0};
  std::string content;
};

class MemoryManager {
public:
  explicit MemoryManager(const std::string &database_path);
  ~MemoryManager();

  MemoryManager(const MemoryManager &) = delete;
  MemoryManager &operator=(const MemoryManager &) = delete;

  MemoryCommandResult HandleCommand(common::MemoryOperation operation,
                                    const std::string &content,
                                    std::int32_t delay_seconds, bool enabled);
  std::vector<DueReminder> TakeDueReminders();

private:
  void InitializeDatabase();
  void ExecuteSql(const char *query);
  sqlite3_stmt *PrepareSqlStatement(const char *query);

  sqlite3 *db_{nullptr};
};

} // namespace xiaocai_robot
