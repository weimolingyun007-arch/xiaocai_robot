#include "intent_node/intent_feedback_store.hpp"

#include <cmath>
#include <filesystem>
#include <utility>

namespace xiaocai_robot {

IntentFeedbackStore::IntentFeedbackStore(std::string database_path)
    : database_path_(std::move(database_path)), database_(nullptr) {}

IntentFeedbackStore::~IntentFeedbackStore() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (database_ != nullptr)
    sqlite3_close(database_);
}

bool IntentFeedbackStore::Initialize(std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (database_ != nullptr)
    return true;

  try {
    const std::filesystem::path database_path(database_path_);
    if (database_path.empty()) {
      error = "feedback database path is empty";
      return false;
    }
    if (!database_path.parent_path().empty())
      std::filesystem::create_directories(database_path.parent_path());
  } catch (const std::exception &exception) {
    error = exception.what();
    return false;
  }

  const int open_result = sqlite3_open_v2(
      database_path_.c_str(), &database_,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  if (open_result != SQLITE_OK) {
    error = database_ != nullptr ? sqlite3_errmsg(database_)
                                 : sqlite3_errstr(open_result);
    if (database_ != nullptr) {
      sqlite3_close(database_);
      database_ = nullptr;
    }
    return false;
  }

  sqlite3_busy_timeout(database_, 3000);
  constexpr char kCreateSchema[] =
      "CREATE TABLE IF NOT EXISTS intent_feedback_samples ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "timestamp_ns INTEGER NOT NULL CHECK(timestamp_ns > 0),"
      "text TEXT NOT NULL CHECK(length(trim(text)) > 0),"
      "correction_text TEXT NOT NULL "
      "CHECK(length(trim(correction_text)) > 0),"
      "predicted_label TEXT NOT NULL CHECK(length(trim(predicted_label)) > 0),"
      "corrected_label TEXT NOT NULL CHECK(length(trim(corrected_label)) > 0),"
      "predicted_score REAL NOT NULL CHECK(predicted_score >= 0.0 AND "
      "predicted_score <= 1.0),"
      "status TEXT NOT NULL DEFAULT 'pending' "
      "CHECK(status IN ('pending', 'accepted', 'rejected'))"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_intent_feedback_status_timestamp "
      "ON intent_feedback_samples(status, timestamp_ns);";
  char *sqlite_error = nullptr;
  const int schema_result =
      sqlite3_exec(database_, kCreateSchema, nullptr, nullptr, &sqlite_error);
  if (schema_result != SQLITE_OK) {
    error = sqlite_error != nullptr ? sqlite_error : sqlite3_errmsg(database_);
    sqlite3_free(sqlite_error);
    sqlite3_close(database_);
    database_ = nullptr;
    return false;
  }

  return true;
}

bool IntentFeedbackStore::SaveCorrection(
    std::int64_t timestamp_ns, const std::string &text,
    const std::string &correction_text, const std::string &predicted_label,
    const std::string &corrected_label, double predicted_score,
    std::int64_t &sample_id, std::string &error) {
  if (timestamp_ns <= 0 || text.empty() || correction_text.empty() ||
      predicted_label.empty() || corrected_label.empty() ||
      predicted_label == corrected_label || !std::isfinite(predicted_score) ||
      predicted_score < 0.0 || predicted_score > 1.0) {
    error = "correction sample fields are invalid";
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (database_ == nullptr) {
    error = "feedback database is not initialized";
    return false;
  }

  constexpr char kInsertSample[] =
      "INSERT INTO intent_feedback_samples "
      "(timestamp_ns, text, correction_text, predicted_label, "
      "corrected_label, predicted_score) VALUES (?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *statement = nullptr;
  const int prepare_result =
      sqlite3_prepare_v2(database_, kInsertSample, -1, &statement, nullptr);
  if (prepare_result != SQLITE_OK) {
    error = sqlite3_errmsg(database_);
    return false;
  }

  sqlite3_bind_int64(statement, 1, timestamp_ns);
  sqlite3_bind_text(statement, 2, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 3, correction_text.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 4, predicted_label.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 5, corrected_label.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_double(statement, 6, predicted_score);

  const int step_result = sqlite3_step(statement);
  if (step_result == SQLITE_DONE) {
    sample_id = sqlite3_last_insert_rowid(database_);
    sqlite3_finalize(statement);
    return true;
  }

  error = sqlite3_errmsg(database_);
  sqlite3_finalize(statement);
  return false;
}

} // namespace xiaocai_robot
