#pragma once
#include "common/common.hpp"
#include "common/log_manager.hpp"
#include "defination/memory_operation.hpp"
#include "defination/robot_state.hpp"
#include "dialogue_node/dialogue_config.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include "xiaocai_msgs/msg/intent.hpp"
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/memory_result.hpp"
#include "xiaocai_msgs/msg/music_command.hpp"
#include "xiaocai_msgs/msg/music_state.hpp"
#include "xiaocai_msgs/msg/reminder.hpp"
#include "xiaocai_msgs/msg/robot_state.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include "xiaocai_msgs/srv/get_volume.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <curl/curl.h>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>
#include <thread>

namespace xiaocai_robot {

class DialogueNode : public rclcpp::Node {
public:
  DialogueNode();
  ~DialogueNode() override;

private:
  void InitPublishers();
  void InitSubscribers();
  void InitClients();
  void InitTimers();
  void Speak(const std::string &text);
  void Request(common::MemoryOperation operation,
               const std::string &content = {}, int32_t delay_seconds = 0,
               bool enabled = false);
  void PublishState(common::RobotState state);
  void OnIntentCallback(const xiaocai_msgs::msg::Intent &message);
  void OnMemoryResultCallback(const xiaocai_msgs::msg::MemoryResult &message);
  void OnReminderDueCallback(const xiaocai_msgs::msg::Reminder &message);
  void OnAudioPlayingCallback(const std_msgs::msg::Bool &message);
  void OnSpeechAwakeCallback(const std_msgs::msg::Bool &message);
  void OnVolumeStateCallback(const std_msgs::msg::Int32 &message);
  void OnMusicStateCallback(const xiaocai_msgs::msg::MusicState &message);
  void OnAudioPlayCallback(const xiaocai_msgs::msg::AudioPlay &message);
  void Chat(const std::string &text, const std::string &facts,
            std::uint64_t generation);
  void EnqueueChat(const std::string &text);
  void ChatWorker();
  bool Cancelled(std::uint64_t generation) const;
  json ExecuteTool(const std::string &name, const json &arguments,
                   std::uint64_t generation, bool &music_started);
  json MemoryTool(common::MemoryOperation operation, const std::string &content,
                  int delay, std::uint64_t generation);
  json Completion(const json &messages, std::uint64_t generation);
  static json ToolDefinitions();
  DialogueConfig dialogue_config_;
  LogManager log_manager_;
  std::string api_url_, model_;
  std::atomic<bool> quiet_{false};
  bool loaded_ = false;
  int next_id_ = 0;
  struct ChatJob {
    std::string text;
    std::uint64_t generation;
  };
  std::atomic<bool> stopping_{false};
  std::atomic<bool> hushed_{false};
  std::atomic<std::uint64_t> generation_{0}, tool_id_{0};
  std::thread worker_;
  std::mutex tool_mutex_;
  std::condition_variable tool_cv_;
  std::deque<ChatJob> jobs_;
  std::map<std::string, json> tool_results_;
  bool tool_volume_pending_ = false;
  json tool_volume_result_;
  std::uint64_t music_sequence_ = 0;
  xiaocai_msgs::msg::MusicState music_state_;
  std::map<std::string, common::MemoryOperation> pending_;
  std::vector<std::string> held_;
  std::vector<json> history_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr tts_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::RobotState>::SharedPtr state_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryCommand>::SharedPtr
      memory_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::VolumeCommand>::SharedPtr
      volume_command_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::MusicCommand>::SharedPtr
      music_command_publisher_;
  rclcpp::Subscription<xiaocai_msgs::msg::Intent>::SharedPtr intent_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MemoryResult>::SharedPtr
      result_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::Reminder>::SharedPtr due_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr playing_subscriber_,
      awake_subscriber_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr
      volume_state_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicState>::SharedPtr
      music_subscriber_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlay>::SharedPtr
      interrupt_subscriber_;
  rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedPtr volume_get_client_;
  rclcpp::TimerBase::SharedPtr timer_;
};

} // namespace xiaocai_robot
