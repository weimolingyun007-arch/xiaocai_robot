#pragma once
#include "common/log_manager.hpp"
#include "dialogue_node/dialogue_config.hpp"
#include "common/common.hpp"
#include "xiaocai_msgs/msg/intent.hpp"
#include "xiaocai_msgs/msg/volume_command.hpp"
#include "xiaocai_msgs/msg/memory_command.hpp"
#include "xiaocai_msgs/msg/memory_result.hpp"
#include "xiaocai_msgs/msg/reminder.hpp"
#include "xiaocai_msgs/msg/robot_state.hpp"
#include "xiaocai_msgs/srv/get_volume.hpp"
#include "xiaocai_msgs/msg/music_state.hpp"
#include "xiaocai_msgs/msg/audio_play.hpp"
#include "defination/memory_operation.hpp"
#include "defination/robot_state.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>
#include <filesystem>
#include <cstdint>
#include <map>
#include <curl/curl.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class DialogueNode:public rclcpp::Node {
public:
  DialogueNode();
  ~DialogueNode()override;
private:
  void InitPublishers();
  void InitSubscribers();
  void InitClients();
  void InitTimers();
  void Speak(const std::string &text);
  void Request(xiaocai_robot::common::MemoryOperation operation,
      const std::string &content={},int32_t delay_seconds=0,bool enabled=false);
  void PublishState(xiaocai_robot::common::RobotState state);
  void OnIntentCallback(const xiaocai_msgs::msg::Intent &message);
  void OnMemoryResultCallback(const xiaocai_msgs::msg::MemoryResult &message);
  void OnReminderDueCallback(const xiaocai_msgs::msg::Reminder &message);
  void OnAudioPlayingCallback(const std_msgs::msg::Bool &message);
  void OnSpeechAwakeCallback(const std_msgs::msg::Bool &message);
  void OnVolumeStateCallback(const std_msgs::msg::Int32 &message);
  void OnMusicStateCallback(const xiaocai_msgs::msg::MusicState &message);
  void OnAudioPlayCallback(const xiaocai_msgs::msg::AudioPlay &message);
  void Chat(const std::string &text,const std::string &facts,std::uint64_t generation);
  void EnqueueChat(const std::string &text);
  void ChatWorker();
  bool Cancelled(std::uint64_t generation) const;
  xc::json ExecuteTool(const std::string &name,const xc::json &arguments,
                       std::uint64_t generation,bool &music_started);
  xc::json MemoryTool(xiaocai_robot::common::MemoryOperation operation,
                       const std::string &content,int delay,std::uint64_t generation);
  xc::json Completion(const xc::json &messages,std::uint64_t generation);
  static xc::json ToolDefinitions();
  DialogueConfig dialogue_config_;
  xc::LogManager log_manager_;
  std::string api_url_,model_;std::atomic<bool> quiet_{false};bool loaded_=false;int next_id_=0;
  struct ChatJob {std::string text;std::uint64_t generation;};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> hushed_{false};
  std::atomic<std::uint64_t> generation_{0},tool_id_{0};
  std::thread worker_;
  std::mutex tool_mutex_;
  std::condition_variable tool_cv_;
  std::deque<ChatJob> jobs_;
  std::map<std::string,xc::json> tool_results_;
  bool tool_volume_pending_=false;
  xc::json tool_volume_result_;
  std::uint64_t music_sequence_=0;
  xiaocai_msgs::msg::MusicState music_state_;
  std::map<std::string,xiaocai_robot::common::MemoryOperation> pending_;std::vector<std::string> held_;std::vector<xc::json> history_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr tts_;
  rclcpp::Publisher<xiaocai_msgs::msg::RobotState>::SharedPtr state_;
  rclcpp::Publisher<xiaocai_msgs::msg::MemoryCommand>::SharedPtr memory_;
  rclcpp::Publisher<xiaocai_msgs::msg::VolumeCommand>::SharedPtr volume_command_;
  rclcpp::Publisher<xiaocai_msgs::msg::Intent>::SharedPtr music_command_;
  rclcpp::Subscription<xiaocai_msgs::msg::Intent>::SharedPtr intent_sub_;
  rclcpp::Subscription<xiaocai_msgs::msg::MemoryResult>::SharedPtr result_sub_;
  rclcpp::Subscription<xiaocai_msgs::msg::Reminder>::SharedPtr due_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr playing_sub_,awake_sub_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr volume_state_sub_;
  rclcpp::Subscription<xiaocai_msgs::msg::MusicState>::SharedPtr music_sub_;
  rclcpp::Subscription<xiaocai_msgs::msg::AudioPlay>::SharedPtr interrupt_sub_;
  rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedPtr volume_get_client_;
  rclcpp::TimerBase::SharedPtr timer_;
};
