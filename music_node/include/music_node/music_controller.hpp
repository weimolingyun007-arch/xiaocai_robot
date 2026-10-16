#pragma once

#include "common/common.hpp"
#include "common/log_manager.hpp"
#include "defination/conversation_phase.hpp"
#include "defination/music_action.hpp"
#include "defination/music_state.hpp"
#include "music_node/music_config.hpp"
#include "music_node/music_lyrics.hpp"

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
  std::string song_id;
  std::string title;
  std::string artist;
  double position_seconds{0.0};
  double duration_seconds{0.0};
};

class MusicController {
public:
  using StatusCallback = std::function<void(const MusicPlaybackStatus &)>;
  using SpeechCallback = std::function<void(const std::string &)>;
  using LoginCallback = std::function<void()>;

  MusicController(MusicSettings settings, LogManager &log_manager,
                  StatusCallback status_callback,
                  SpeechCallback speech_callback, LoginCallback login_callback);
  ~MusicController();

  void HandleMusicCommand(common::MusicAction action, const std::string &query,
                          std::uint64_t request_id);
  void HandleAudioPlay(const std::string &path);
  void HandleVoicePlaying(bool playing);
  void HandleSpeechAwake(bool awake);
  void HandleConversation(common::ConversationPhase phase);
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
    int exit_code{-1};
    bool timed_out{false};
  };
  struct QueueTrack {
    MusicTrack track;
    std::string original_id;
  };

  // Background worker loop and per-job execution.
  void WorkerLoop();
  void ExecuteJob(const Job &job);
  void AppendPlaylist(const json &songs);
  void PlayTrack(const Job &job, const QueueTrack &track, std::size_t index);

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
  LoginCallback login_callback_;
  std::atomic<bool> stop_, silenced_, voice_playing_, busy_;
  std::atomic<double> last_voice_at_, last_conversation_at_;
  std::atomic<bool> awake_, conversation_active_, resume_after_dialogue_,
      manually_paused_;
  std::atomic<std::uint64_t> revision_;
  std::mutex jobs_mutex_, state_mutex_, backend_mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  std::thread worker_;
  int backend_fd_;
  MusicPlaybackStatus status_;
  std::vector<QueueTrack> playlist_;
  std::size_t playlist_index_{0};
};

} // namespace xiaocai_robot
