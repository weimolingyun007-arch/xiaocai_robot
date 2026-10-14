#pragma once

#include "common/log_manager.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xiaocai_robot {

struct MusicLoginStatus {
  bool required{false};
  bool loading{false};
  std::string url;
  std::string error;
};

class MusicLoginManager {
public:
  using StatusCallback = std::function<void(const MusicLoginStatus &)>;
  using SpeechCallback = std::function<void(const std::string &)>;

  MusicLoginManager(std::string cli_path, LogManager &log_manager,
                    StatusCallback status_callback,
                    SpeechCallback speech_callback);
  ~MusicLoginManager();
  void RequestLogin();

private:
  // Login status polling and QR renewal.
  void WorkerLoop();
  void PublishStatus(const MusicLoginStatus &status);

  // Bounded, cancellable CLI invocation.
  std::string RunCommand(const std::vector<std::string> &arguments);

  std::string cli_path_;
  LogManager &log_manager_;
  StatusCallback status_callback_;
  SpeechCallback speech_callback_;
  std::atomic<bool> stop_;
  std::mutex mutex_;
  std::condition_variable ready_;
  bool requested_;
  MusicLoginStatus status_;
  std::thread worker_;
};

} // namespace xiaocai_robot
