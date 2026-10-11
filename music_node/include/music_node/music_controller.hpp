#pragma once

#include "common/common.hpp"
#include "common/log_manager.hpp"
#include "defination/music_action.hpp"
#include "defination/music_state.hpp"
#include "music_node/music_config.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xiaocai_robot {

struct MusicPlaybackStatus {
  common::MusicState state{common::MusicState::Stopped};
  std::string error;
  std::uint64_t request_id{0};
  std::string title;
  double position_seconds{0.0};
  double duration_seconds{0.0};
};

class MusicController {
public:
  using StatusCallback = std::function<void(const MusicPlaybackStatus &)>;
  using SpeechCallback = std::function<void(const std::string &)>;

  MusicController(MusicSettings settings, LogManager &log_manager,
                  StatusCallback status_callback,
                  SpeechCallback speech_callback);
  ~MusicController();

  void HandleMusicCommand(common::MusicAction action, const std::string &query,
                          std::uint64_t request_id);
  void HandleAudioPlay(const std::string &path);
  void HandleVoicePlaying(bool playing);
  void HandleStateUpdate();

private:
  struct Job {
    common::MusicAction action{common::MusicAction::Play};
    std::string query;
    std::uint64_t revision{0};
    std::uint64_t request_id{0};
  };
  struct CliResult {
    bool ok{false};
    json data;
    std::string output;
  };

  // Background worker loop and per-job execution.
  void WorkerLoop();
  void ExecuteJob(const Job &job);

  // Job validity and playback status.
  bool IsJobCurrent(std::uint64_t revision) const;
  void SetPlaybackStatus(common::MusicState state,
                         const std::string &error = {},
                         std::uint64_t request_id = 0);

  // ncm-cli backend subscription management.
  bool EnsureBackendSubscription(std::uint64_t revision);
  void MaintainBackendSubscription();
  void CloseBackendSubscription();

  // Command execution, CLI calls, and failure handling.
  CliResult RunCliCommand(const std::vector<std::string> &arguments,
                          std::uint64_t revision);
  bool WaitForSpeechToFinish(std::uint64_t revision);
  void FailJob(const Job &job, const std::string &message);

  MusicSettings settings_;
  LogManager &log_manager_;
  StatusCallback status_callback_;
  SpeechCallback speech_callback_;
  std::atomic<bool> stop_, silenced_, voice_playing_, busy_;
  std::atomic<double> last_voice_at_;
  std::atomic<std::uint64_t> revision_;
  std::mutex jobs_mutex_, state_mutex_, backend_mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  std::thread worker_;
  int backend_fd_;
  MusicPlaybackStatus status_;
};

} // namespace xiaocai_robot
