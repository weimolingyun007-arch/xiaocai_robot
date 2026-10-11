#include "memory_node/memory_manager.hpp"

#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace xiaocai_robot {

MemoryManager::MemoryManager(const std::string &database_path) : db_(nullptr) {
  const std::filesystem::path path(database_path);
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());

  if (sqlite3_open(database_path.c_str(), &db_) != SQLITE_OK) {
    const std::string error =
        db_ ? sqlite3_errmsg(db_) : "unknown SQLite error";
    if (db_)
      sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("Cannot open memory database: " + error);
  }

  try {
    InitializeDatabase();
  } catch (...) {
    sqlite3_close(db_);
    db_ = nullptr;
    throw;
  }
}

MemoryManager::~MemoryManager() {
  if (db_)
    sqlite3_close(db_);
}

void MemoryManager::InitializeDatabase() {
  ExecuteSql("CREATE TABLE IF NOT EXISTS facts(id INTEGER PRIMARY KEY,content "
             "TEXT NOT NULL UNIQUE)");
  ExecuteSql("CREATE TABLE IF NOT EXISTS reminders(id INTEGER PRIMARY KEY,"
             "content TEXT NOT NULL,due_at INTEGER NOT NULL,delivered INTEGER "
             "NOT NULL DEFAULT 0)");
  ExecuteSql("CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value "
             "TEXT NOT NULL)");
}

void MemoryManager::ExecuteSql(const char *query) {
  char *error = nullptr;
  if (sqlite3_exec(db_, query, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "SQL error";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

sqlite3_stmt *MemoryManager::PrepareSqlStatement(const char *query) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(db_, query, -1, &statement, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db_));
  return statement;
}

MemoryCommandResult
MemoryManager::HandleCommand(common::MemoryOperation operation,
                             const std::string &content,
                             std::int32_t delay_seconds, bool enabled) {
  MemoryCommandResult result;
  result.ok = true;
  sqlite3_stmt *statement = nullptr;

  try {
    switch (operation) {
    case common::MemoryOperation::SaveFact:
    case common::MemoryOperation::ForgetFact: {
      if (content.empty())
        throw std::runtime_error("empty content");
      const bool forget = operation == common::MemoryOperation::ForgetFact;
      statement = PrepareSqlStatement(
          forget ? "DELETE FROM facts WHERE content LIKE ?"
                 : "INSERT OR IGNORE INTO facts(content) VALUES(?)");
      const std::string value = forget ? "%" + content + "%" : content;
      sqlite3_bind_text(statement, 1, value.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(statement) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
      if (forget)
        result.deleted = sqlite3_changes(db_);
      break;
    }
    case common::MemoryOperation::ListFacts:
      statement = PrepareSqlStatement(
          "SELECT content FROM facts ORDER BY id DESC LIMIT 10");
      while (sqlite3_step(statement) == SQLITE_ROW)
        result.facts.emplace_back(
            reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
      break;
    case common::MemoryOperation::AddReminder:
      if (content.empty() || delay_seconds < 1 || delay_seconds > 30 * 86400)
        throw std::runtime_error("invalid reminder");
      statement = PrepareSqlStatement(
          "INSERT INTO reminders(content,due_at) VALUES(?,?)");
      sqlite3_bind_text(statement, 1, content.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(statement, 2, std::time(nullptr) + delay_seconds);
      if (sqlite3_step(statement) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
      result.content = content;
      break;
    case common::MemoryOperation::SetQuiet:
      statement = PrepareSqlStatement(
          "INSERT INTO settings(key,value) VALUES('quiet',?) ON "
          "CONFLICT(key) DO UPDATE SET value=excluded.value");
      sqlite3_bind_text(statement, 1, enabled ? "1" : "0", -1, SQLITE_STATIC);
      if (sqlite3_step(statement) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
      result.enabled = enabled;
      break;
    case common::MemoryOperation::GetQuiet:
      statement =
          PrepareSqlStatement("SELECT value FROM settings WHERE key='quiet'");
      result.enabled = sqlite3_step(statement) == SQLITE_ROW &&
                       std::string(reinterpret_cast<const char *>(
                           sqlite3_column_text(statement, 0))) == "1";
      break;
    default:
      throw std::runtime_error("unknown operation");
    }
  } catch (const std::exception &error) {
    result.ok = false;
    result.error = error.what();
  }

  if (statement)
    sqlite3_finalize(statement);
  return result;
}

std::vector<DueReminder> MemoryManager::TakeDueReminders() {
  sqlite3_stmt *statement =
      PrepareSqlStatement("SELECT id,content FROM reminders WHERE delivered=0 "
                          "AND due_at<=? ORDER BY due_at LIMIT 10");
  sqlite3_bind_int64(statement, 1, std::time(nullptr));

  std::vector<DueReminder> reminders;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    reminders.push_back(
        {sqlite3_column_int(statement, 0),
         reinterpret_cast<const char *>(sqlite3_column_text(statement, 1))});
  }
  sqlite3_finalize(statement);

  for (const auto &reminder : reminders) {
    statement =
        PrepareSqlStatement("UPDATE reminders SET delivered=1 WHERE id=?");
    sqlite3_bind_int(statement, 1, reminder.id);
    if (sqlite3_step(statement) != SQLITE_DONE) {
      const std::string error = sqlite3_errmsg(db_);
      sqlite3_finalize(statement);
      throw std::runtime_error(error);
    }
    sqlite3_finalize(statement);
  }

  return reminders;
}

} // namespace xiaocai_robot
