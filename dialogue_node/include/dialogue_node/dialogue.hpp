#pragma once
#include "xiaocai_cpp/common.hpp"
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

class Dialogue:public rclcpp::Node {
public:
  Dialogue();
  ~Dialogue()override;
private:
  void speak(const std::string &text);
  void request(xiaocai_robot::common::MemoryOperation operation,
      const std::string &content={},int32_t delay_seconds=0,bool enabled=false);
  void publish_state(xiaocai_robot::common::RobotState state);
  void on_intent(const xiaocai_msgs::msg::Intent &message);
  void on_result(const xiaocai_msgs::msg::MemoryResult &message);
  void on_due(const xiaocai_msgs::msg::Reminder &message);
  void chat(const std::string &text,const std::string &facts,std::uint64_t generation);
  void enqueue_chat(const std::string &text);
  void chat_worker();
  bool cancelled(std::uint64_t generation) const;
  xc::json execute_tool(const std::string &name,const xc::json &arguments,
                       std::uint64_t generation,bool &music_started);
  xc::json memory_tool(xiaocai_robot::common::MemoryOperation operation,
                       const std::string &content,int delay,std::uint64_t generation);
  xc::json completion(const xc::json &messages,std::uint64_t generation);
  static xc::json tool_definitions();
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
