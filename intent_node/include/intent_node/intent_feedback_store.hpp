#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace xiaocai_robot {

class IntentFeedbackStore {
public:
  explicit IntentFeedbackStore(std::string database_path);
  ~IntentFeedbackStore();

  IntentFeedbackStore(const IntentFeedbackStore &) = delete;
  IntentFeedbackStore &operator=(const IntentFeedbackStore &) = delete;

  bool Initialize(std::string &error);
  bool SaveCorrection(std::int64_t timestamp_ns, const std::string &text,
                      const std::string &correction_text,
                      const std::string &predicted_label,
                      const std::string &corrected_label,
                      double predicted_score, std::int64_t &sample_id,
                      std::string &error);

private:
  std::string database_path_;
  sqlite3 *database_;
  std::mutex mutex_;
};

} // namespace xiaocai_robot
