#pragma once

#include "common/common.hpp"
#include "common/log_manager.hpp"
#include "defination/audio_stream_action.hpp"
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

struct AudioPcmFrame {
  common::AudioStreamAction action{common::AudioStreamAction::Data};
  std::uint64_t stream_id{0};
  std::uint64_t sequence{0};
  std::uint32_t sample_rate{48000};
  std::uint8_t channels{2};
  std::vector<std::uint8_t> data;
};

class MusicController {
public:
  using StatusCallback = std::function<void(const MusicPlaybackStatus &)>;
  using SpeechCallback = std::function<void(const std::string &)>;
  using LoginCallback = std::function<void()>;
  using AudioOutputCallback = std::function<void(const AudioPcmFrame &)>;

  MusicController(MusicSettings settings, LogManager &log_manager,
                  StatusCallback status_callback,
                  SpeechCallback speech_callback, LoginCallback login_callback,
                  AudioOutputCallback audio_output_callback = {});
  ~MusicController();

  void HandleMusicCommand(common::MusicAction action, const std::string &query,
                          std::uint64_t request_id);
  void HandleAudioPlay(const std::string &path);
  void HandleVoicePlaying(bool playing);
  void HandleSpeechAwake(bool awake);
  void HandleConversation(common::ConversationPhase phase);
  void HandleStateUpdate();
  void HandleAudioOutputFailure(std::uint64_t stream_id);

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
    double duration_seconds{0.0};
  };

  // Background worker loop and per-job execution.
  void WorkerLoop();
  void PcmReaderLoop();
  void PcmPublishLoop();
  void PausePcmStream(bool paused);
  void ResetPcmStream(bool active);
  void StopPcmDecoder(bool next_stream = false);
  void HandlePcmStateUpdate();
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
  AudioOutputCallback audio_output_callback_;
  std::atomic<bool> stop_, silenced_, voice_playing_, busy_;
  std::atomic<double> last_voice_at_, last_conversation_at_;
  std::atomic<bool> awake_, conversation_active_, resume_after_dialogue_,
      manually_paused_;
  std::atomic<std::uint64_t> revision_;
  std::mutex jobs_mutex_, state_mutex_, backend_mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  std::thread worker_, pcm_reader_, pcm_publisher_;
  std::mutex pcm_mutex_;
  std::condition_variable pcm_ready_;
  int pcm_fd_{-1};
  bool pcm_active_{false}, pcm_paused_{false}, pcm_eof_{false};
  std::deque<std::vector<std::uint8_t>> pcm_queue_;
  std::uint64_t pcm_generation_{0};
  double pcm_position_{0.0};
  std::uint64_t pcm_stream_id_{0}, pcm_sequence_{0};
  int backend_fd_;
  MusicPlaybackStatus status_;
  std::vector<QueueTrack> playlist_;
  std::size_t playlist_index_{0};
};

} // namespace xiaocai_robot
