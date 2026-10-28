#pragma once

#include "common/log_manager.hpp"
#include "defination/conversation_phase.hpp"
#include "defination/intent_type.hpp"
#include "defination/memory_operation.hpp"
#include "defination/music_action.hpp"
#include "defination/music_state.hpp"
#include "defination/robot_state.hpp"
#include "defination/volume_action.hpp"
#include "dialogue_node/dialogue_config.hpp"
#include "dialogue_node/weather_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xiaocai_robot {

struct DialogueMemoryCommand {
  common::MemoryOperation operation{common::MemoryOperation::GetQuiet};
  std::string request_id;
  std::string content;
  std::int32_t delay_seconds{0};
  bool enabled{false};
};

struct DialogueMemoryResult {
  std::string request_id;
  common::MemoryOperation operation{common::MemoryOperation::GetQuiet};
  bool ok{false};
  std::string error;
  std::vector<std::string> facts;
  std::string content;
  std::int32_t deleted{0};
  bool enabled{false};
};

struct DialogueMusicStatus {
  common::MusicState state{common::MusicState::Stopped};
  std::string title;
  double position_seconds{0.0};
  double duration_seconds{0.0};
  std::string error;
  std::uint64_t request_id{0};
};

struct DialogueCallbacks {
  std::function<void(const std::string &, std::uint64_t)> speak;
  std::function<void(common::ConversationPhase, bool)> publish_conversation;
  std::function<void(common::RobotState)> publish_state;
  std::function<void(const DialogueMemoryCommand &)> publish_memory_command;
  std::function<void(common::VolumeAction, int)> publish_volume_command;
  std::function<void(common::MusicAction, const std::string &, std::uint64_t)>
      publish_music_command;
};

class DialogueController {
public:
  DialogueController(DialogueSettings settings, LogManager &log_manager,
                     DialogueCallbacks callbacks);
  ~DialogueController();

  DialogueController(const DialogueController &) = delete;
  DialogueController &operator=(const DialogueController &) = delete;

  // Events forwarded from ROS subscriptions and timers.
  void HandleIntent(common::IntentType type, const std::string &text);
  void HandleMemoryResult(const DialogueMemoryResult &result);
  void HandleReminderDue(const std::string &content);
  void HandleAudioPlaying(bool playing);
  void HandleSpeechAwake(bool awake);
  void HandleVolumeState(int percent);
  void HandleMusicState(const DialogueMusicStatus &status);
  void HandleAudioPlay(const std::string &path);
  void HandleTimerTick();
  void HandleTtsResult(std::uint64_t request_id, bool success);

private:
  struct ChatJob {
    std::string text;
    std::uint64_t generation;
    std::uint64_t session;
  };

  // Conversation lifecycle and user-facing responses.
  void Speak(const std::string &text, bool expect_reply = false);
  void SetConversation(common::ConversationPhase phase,
                       bool expect_reply = false);
  void PublishCurrentState();
  void RequestMemory(common::MemoryOperation operation,
                     const std::string &content = {},
                     std::int32_t delay_seconds = 0, bool enabled = false);
  void HandleMemoryCommandResult(const DialogueMemoryResult &result);

  // Model tool definitions and chat worker lifecycle.
  static nlohmann::json ToolDefinitions();
  bool IsGenerationCancelled(std::uint64_t generation) const;
  void EnqueueChat(const std::string &text);
  void WorkerLoop();

  // Model tool execution and completion processing.
  nlohmann::json ExecuteMemoryTool(common::MemoryOperation operation,
                                   const std::string &content, int delay,
                                   std::uint64_t generation);
  nlohmann::json QueryVolumeTool(std::uint64_t generation);
  nlohmann::json ExecuteTool(const std::string &name,
                             const nlohmann::json &arguments,
                             std::uint64_t generation, bool &music_started);
  nlohmann::json
  RequestCompletion(const std::string &api_url, const std::string &model,
                    const nlohmann::json &messages, std::uint64_t generation,
                    bool local_model, long connect_timeout, long timeout,
                    std::int32_t max_tokens, const std::string &provider,
                    bool final_reply_only = false);
  nlohmann::json Completion(const nlohmann::json &messages,
                            std::uint64_t generation);
  void HandleChat(const std::string &text, const std::string &facts,
                  std::uint64_t generation);

  DialogueSettings settings_;
  LogManager &log_manager_;
  DialogueCallbacks callbacks_;
  std::shared_ptr<std::atomic<bool>> alive_;
  std::atomic<bool> quiet_;
  bool loaded_;
  std::uint64_t next_memory_request_id_;
  std::atomic<bool> stopping_;
  std::atomic<bool> hushed_;
  std::atomic<std::uint64_t> generation_;
  std::atomic<std::uint64_t> local_fallback_generation_;
  std::atomic<std::uint64_t> session_;
  std::uint64_t history_session_;
  std::atomic<std::uint64_t> tool_id_;
  WeatherClient weather_client_;
  std::thread worker_;
  std::mutex tool_mutex_;
  std::condition_variable tool_cv_;
  std::deque<ChatJob> jobs_;
  std::map<std::string, nlohmann::json> tool_results_;
  bool tool_volume_pending_;
  nlohmann::json tool_volume_result_;
  bool volume_known_;
  int volume_percent_;
  std::uint64_t music_sequence_;
  DialogueMusicStatus music_state_;
  std::map<std::string, common::MemoryOperation> pending_;
  std::vector<std::string> held_;
  std::vector<nlohmann::json> history_;
  std::mutex conversation_mutex_;
  common::ConversationPhase conversation_phase_;
  bool expect_reply_;
  bool audio_playing_;
  bool music_searching_;
  std::uint64_t next_reply_id_;
  std::map<std::uint64_t, bool> pending_replies_;
  std::string clarification_context_;
  std::uint64_t clarification_session_;
};

} // namespace xiaocai_robot
