#include "dialogue_node/dialogue_node.hpp"
#include "common/common.hpp"
#include "defination/intent_type.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <map>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>

namespace xiaocai_robot {

DialogueNode::DialogueNode()
    : Node("dialogue_node"), dialogue_config_(),
      log_manager_("dialogue_node", dialogue_config_.Logging()) {
  const auto &settings = dialogue_config_.Settings();
  api_url_ = declare_parameter<std::string>("api_url", settings.api_url);
  model_ = declare_parameter<std::string>("model", settings.model);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  InitPublishers();
  InitSubscribers();
  InitClients();
  InitTimers();
  PublishState(common::RobotState::Idle);
  worker_ = std::thread([this] { ChatWorker(); });
}

void DialogueNode::InitPublishers() {
  int initialized_count = 0;
  tts_publisher_ =
      create_publisher<std_msgs::msg::String>("/xiaocai_robot/tts/text", 10);
  ++initialized_count;
  state_publisher_ = create_publisher<xiaocai_msgs::msg::RobotState>(
      "/xiaocai_robot/state", 10);
  ++initialized_count;
  memory_publisher_ = create_publisher<xiaocai_msgs::msg::MemoryCommand>(
      "/xiaocai_robot/memory/command", 10);
  ++initialized_count;
  volume_command_publisher_ =
      create_publisher<xiaocai_msgs::msg::VolumeCommand>(
          "/xiaocai_robot/audio/volume/command", 10);
  ++initialized_count;
  music_command_publisher_ = create_publisher<xiaocai_msgs::msg::MusicCommand>(
      "/xiaocai_robot/music/command", 10);
  ++initialized_count;
  LOG_INFO("Initialized {} publishers", initialized_count);
}

void DialogueNode::InitSubscribers() {
  int initialized_count = 0;
  intent_subscriber_ = create_subscription<xiaocai_msgs::msg::Intent>(
      "/xiaocai_robot/intent", 10,
      [this](xiaocai_msgs::msg::Intent::SharedPtr message) {
        OnIntentCallback(*message);
      });
  ++initialized_count;
  result_subscriber_ = create_subscription<xiaocai_msgs::msg::MemoryResult>(
      "/xiaocai_robot/memory/result", 10,
      [this](xiaocai_msgs::msg::MemoryResult::SharedPtr message) {
        OnMemoryResultCallback(*message);
      });
  ++initialized_count;
  due_subscriber_ = create_subscription<xiaocai_msgs::msg::Reminder>(
      "/xiaocai_robot/reminder/due", 10,
      [this](xiaocai_msgs::msg::Reminder::SharedPtr message) {
        OnReminderDueCallback(*message);
      });
  ++initialized_count;
  playing_subscriber_ = create_subscription<std_msgs::msg::Bool>(
      "/xiaocai_robot/audio/playing", 10,
      [this](std_msgs::msg::Bool::SharedPtr message) {
        OnAudioPlayingCallback(*message);
      });
  ++initialized_count;
  awake_subscriber_ = create_subscription<std_msgs::msg::Bool>(
      "/xiaocai_robot/speech/awake", 10,
      [this](std_msgs::msg::Bool::SharedPtr message) {
        OnSpeechAwakeCallback(*message);
      });
  ++initialized_count;
  volume_state_subscriber_ = create_subscription<std_msgs::msg::Int32>(
      "/xiaocai_robot/audio/volume/state", 10,
      [this](std_msgs::msg::Int32::SharedPtr message) {
        OnVolumeStateCallback(*message);
      });
  ++initialized_count;
  music_subscriber_ = create_subscription<xiaocai_msgs::msg::MusicState>(
      "/xiaocai_robot/music/state", rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::MusicState::SharedPtr message) {
        OnMusicStateCallback(*message);
      });
  ++initialized_count;
  interrupt_subscriber_ = create_subscription<xiaocai_msgs::msg::AudioPlay>(
      "/xiaocai_robot/audio/play", 10,
      [this](xiaocai_msgs::msg::AudioPlay::SharedPtr message) {
        OnAudioPlayCallback(*message);
      });
  ++initialized_count;
  LOG_INFO("Initialized {} subscribers", initialized_count);
}

void DialogueNode::InitClients() {
  volume_get_client_ = create_client<xiaocai_msgs::srv::GetVolume>(
      "/xiaocai_robot/audio/get_volume");
}

void DialogueNode::InitTimers() {
  timer_ = create_wall_timer(std::chrono::seconds(3), [this] {
    if (!loaded_)
      Request(common::MemoryOperation::GetQuiet);
  });
}

DialogueNode::~DialogueNode() {
  stopping_ = true;
  tool_cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
  curl_global_cleanup();
}

void DialogueNode::PublishState(common::RobotState state) {
  xiaocai_msgs::msg::RobotState message;
  message.timestamp = now();
  message.state = static_cast<std::uint8_t>(state);
  state_publisher_->publish(message);
}

void DialogueNode::Speak(const std::string &text) {
  if (hushed_ || stopping_)
    return;
  PublishState(common::RobotState::Thinking);
  tts_publisher_->publish(Str(text));
}

void DialogueNode::Request(common::MemoryOperation operation,
                           const std::string &content, int32_t delay_seconds,
                           bool enabled) {
  std::string id = std::to_string(++next_id_);
  pending_[id] = operation;
  xiaocai_msgs::msg::MemoryCommand message;
  message.timestamp = now();
  message.operation = static_cast<std::uint8_t>(operation);
  message.request_id = id;
  message.content = content;
  message.delay_seconds = delay_seconds;
  message.enabled = enabled;
  memory_publisher_->publish(message);
}

void DialogueNode::OnIntentCallback(const xiaocai_msgs::msg::Intent &message) {
  using Intent = common::IntentType;
  const auto intent = static_cast<Intent>(message.type);
  if (hushed_)
    return;
  if (!common::IsKnownIntent(intent)) {
    LOG_WARN("Ignoring unknown intent code: {}", static_cast<unsigned>(intent));
    return;
  }
  PublishState(common::RobotState::Thinking);
  if (intent == Intent::VolumeGet) {
    if (!volume_get_client_->service_is_ready()) {
      Speak("音量查询服务暂时不可用。");
      return;
    }
    auto request = std::make_shared<xiaocai_msgs::srv::GetVolume::Request>();
    request->timestamp = now();
    volume_get_client_->async_send_request(
        request,
        [this](
            rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedFuture future) {
          auto response = future.get();
          Speak("当前音量是百分之" + std::to_string(response->percent) + "。");
        });
  } else if (intent == Intent::QuietOn) {
    quiet_ = true;
    Request(common::MemoryOperation::SetQuiet, {}, 0, true);
    Speak("好的，我会安静地陪着你。需要我时叫小蔡。");
  } else if (intent == Intent::QuietOff) {
    quiet_ = false;
    Request(common::MemoryOperation::SetQuiet, {}, 0, false);
    Speak("好的，我在。");
    for (auto &s : held_)
      Speak("提醒你，" + s);
    held_.clear();
  } else if (intent == Intent::ReminderClarify) {
    Speak("请说清楚多久后提醒你，以及要提醒什么。比如十分钟后提醒我喝水。");
  } else if (intent == Intent::Chat) {
    EnqueueChat(message.text);
  }
}

void DialogueNode::OnMemoryResultCallback(
    const xiaocai_msgs::msg::MemoryResult &message) {
  if (message.request_id.rfind("tool-", 0) == 0) {
    std::lock_guard<std::mutex> lock(tool_mutex_);
    auto entry = tool_results_.find(message.request_id);
    if (entry != tool_results_.end())
      entry->second = {
          {"ok", message.ok},           {"error", message.error},
          {"facts", message.facts},     {"content", message.content},
          {"deleted", message.deleted}, {"enabled", message.enabled}};
    tool_cv_.notify_all();
    return;
  }
  common::MemoryOperation operation;
  if (message.request_id.rfind("intent-", 0) == 0) {
    operation = static_cast<common::MemoryOperation>(message.operation);
  } else {
    auto it = pending_.find(message.request_id);
    if (it == pending_.end())
      return;
    operation = it->second;
    pending_.erase(it);
  }
  if (operation == common::MemoryOperation::GetQuiet) {
    if (message.ok) {
      quiet_ = message.enabled;
      loaded_ = true;
    }
    return;
  }
  if (operation == common::MemoryOperation::SetQuiet)
    return;
  if (!message.ok) {
    Speak("这件事刚才没保存成功，请再说一次。");
    return;
  }
  if (operation == common::MemoryOperation::SaveFact)
    Speak("好，我记住了。");
  else if (operation == common::MemoryOperation::ForgetFact)
    Speak(message.deleted > 0 ? "已经忘记了。" : "我没有找到这条记忆。");
  else if (operation == common::MemoryOperation::ListFacts) {
    std::string facts;
    for (const auto &fact : message.facts) {
      if (!facts.empty())
        facts += "；";
      facts += fact;
    }
    Speak(facts.empty() ? "你还没有让我记住什么。" : "我记得：" + facts);
  } else if (operation == common::MemoryOperation::AddReminder)
    Speak("好的，我会提醒你" + message.content + "。");
}

void DialogueNode::OnReminderDueCallback(
    const xiaocai_msgs::msg::Reminder &message) {
  if (message.content.empty())
    return;
  if (quiet_)
    held_.push_back(message.content);
  else
    Speak("提醒你，" + message.content);
}

void DialogueNode::OnAudioPlayingCallback(const std_msgs::msg::Bool &message) {
  PublishState(message.data ? common::RobotState::Speaking
               : quiet_     ? common::RobotState::Quiet
                            : common::RobotState::Idle);
}

void DialogueNode::OnSpeechAwakeCallback(const std_msgs::msg::Bool &message) {
  if (message.data) {
    hushed_ = false;
    ++generation_;
    tool_cv_.notify_all();
  }
  PublishState(message.data ? common::RobotState::Listening
               : quiet_     ? common::RobotState::Quiet
                            : common::RobotState::Idle);
}

void DialogueNode::OnVolumeStateCallback(const std_msgs::msg::Int32 &message) {
  {
    std::lock_guard<std::mutex> lock(tool_mutex_);
    if (tool_volume_pending_) {
      tool_volume_result_ = {{"ok", true}, {"percent", message.data}};
      tool_cv_.notify_all();
      return;
    }
  }
  if (message.data > 0)
    Speak("音量调到百分之" + std::to_string(message.data) + "。");
  else
    PublishState(common::RobotState::Muted);
}

void DialogueNode::OnMusicStateCallback(
    const xiaocai_msgs::msg::MusicState &message) {
  std::lock_guard<std::mutex> lock(tool_mutex_);
  music_state_ = message;
  ++music_sequence_;
  tool_cv_.notify_all();
}

void DialogueNode::OnAudioPlayCallback(
    const xiaocai_msgs::msg::AudioPlay &message) {
  if (message.path.empty()) {
    hushed_ = true;
    ++generation_;
    tool_cv_.notify_all();
  }
}

} // namespace xiaocai_robot
