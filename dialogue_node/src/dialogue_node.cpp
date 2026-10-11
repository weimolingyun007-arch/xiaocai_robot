#include "dialogue_node/dialogue_node.hpp"
#include "common/common.hpp"
#include "defination/volume_action.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <filesystem>
#include <map>
#include <curl/curl.h>



DialogueNode::DialogueNode()
    : Node("dialogue_node"), dialogue_config_(),
      log_manager_("dialogue_node", dialogue_config_.Logging()) {
    const auto &settings = dialogue_config_.Settings();
    api_url_=declare_parameter<std::string>("api_url",settings.api_url);
    model_=declare_parameter<std::string>("model",settings.model);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    InitPublishers();
    InitSubscribers();
    InitClients();
    InitTimers();
    PublishState(xiaocai_robot::common::RobotState::Idle);
    worker_=std::thread([this]{ChatWorker();});
  }

void DialogueNode::InitPublishers() {
    tts_=create_publisher<std_msgs::msg::String>("/xiaocai_robot/tts/text",10);
    state_=create_publisher<xiaocai_msgs::msg::RobotState>("/xiaocai_robot/state",10);
    memory_=create_publisher<xiaocai_msgs::msg::MemoryCommand>("/xiaocai_robot/memory/command",10);
    volume_command_=create_publisher<xiaocai_msgs::msg::VolumeCommand>("/xiaocai_robot/audio/volume/command",10);
    music_command_=create_publisher<xiaocai_msgs::msg::Intent>("/xiaocai_robot/intent",10);
  }

void DialogueNode::InitSubscribers() {
    intent_sub_=create_subscription<xiaocai_msgs::msg::Intent>("/xiaocai_robot/intent",10,[this](xiaocai_msgs::msg::Intent::SharedPtr m){OnIntent(*m);});
    result_sub_=create_subscription<xiaocai_msgs::msg::MemoryResult>("/xiaocai_robot/memory/result",10,[this](xiaocai_msgs::msg::MemoryResult::SharedPtr m){OnResult(*m);});
    due_sub_=create_subscription<xiaocai_msgs::msg::Reminder>("/xiaocai_robot/reminder/due",10,[this](xiaocai_msgs::msg::Reminder::SharedPtr m){OnDue(*m);});
    playing_sub_=create_subscription<std_msgs::msg::Bool>("/xiaocai_robot/audio/playing",10,[this](std_msgs::msg::Bool::SharedPtr m){PublishState(m->data?xiaocai_robot::common::RobotState::Speaking:quiet_?xiaocai_robot::common::RobotState::Quiet:xiaocai_robot::common::RobotState::Idle);});
    awake_sub_=create_subscription<std_msgs::msg::Bool>("/xiaocai_robot/speech/awake",10,[this](std_msgs::msg::Bool::SharedPtr m){if(m->data){hushed_=false;++generation_;tool_cv_.notify_all();}PublishState(m->data?xiaocai_robot::common::RobotState::Listening:quiet_?xiaocai_robot::common::RobotState::Quiet:xiaocai_robot::common::RobotState::Idle);});
    volume_state_sub_=create_subscription<std_msgs::msg::Int32>("/xiaocai_robot/audio/volume/state",10,
      [this](std_msgs::msg::Int32::SharedPtr m){
        {std::lock_guard<std::mutex> lock(tool_mutex_);
         if(tool_volume_pending_){tool_volume_result_={{"ok",true},{"percent",m->data}};tool_cv_.notify_all();return;}}
        if(m->data>0)Speak("音量调到百分之"+std::to_string(m->data)+"。");else PublishState(xiaocai_robot::common::RobotState::Muted);});
    music_sub_=create_subscription<xiaocai_msgs::msg::MusicState>("/xiaocai_robot/music/state",rclcpp::QoS(1).transient_local(),
      [this](xiaocai_msgs::msg::MusicState::SharedPtr m){std::lock_guard<std::mutex> lock(tool_mutex_);music_state_=*m;++music_sequence_;tool_cv_.notify_all();});
    interrupt_sub_=create_subscription<xiaocai_msgs::msg::AudioPlay>("/xiaocai_robot/audio/play",10,
      [this](xiaocai_msgs::msg::AudioPlay::SharedPtr m){if(m->path=="__stop__"){hushed_=true;++generation_;tool_cv_.notify_all();}});
  }

void DialogueNode::InitClients() {
    volume_get_client_=create_client<xiaocai_msgs::srv::GetVolume>("/xiaocai_robot/audio/get_volume");
  }

void DialogueNode::InitTimers() {
    timer_=create_wall_timer(std::chrono::seconds(3),[this]{if(!loaded_)Request(xiaocai_robot::common::MemoryOperation::GetQuiet);});
  }

DialogueNode::~DialogueNode() {stopping_=true;tool_cv_.notify_all();if(worker_.joinable())worker_.join();curl_global_cleanup();}

void DialogueNode::PublishState(xiaocai_robot::common::RobotState state) {
    xiaocai_msgs::msg::RobotState message;
    message.state=static_cast<std::uint8_t>(state);
    state_->publish(message);
  }

void DialogueNode::Speak(const std::string &text) {if(hushed_||stopping_)return;PublishState(xiaocai_robot::common::RobotState::Thinking);tts_->publish(xc::Str(text));}

void DialogueNode::Request(xiaocai_robot::common::MemoryOperation operation,
    const std::string &content,int32_t delay_seconds,bool enabled) {
    std::string id=std::to_string(++next_id_);
    pending_[id]=operation;
    xiaocai_msgs::msg::MemoryCommand message;
    message.operation=static_cast<std::uint8_t>(operation);
    message.request_id=id;
    message.content=content;
    message.delay_seconds=delay_seconds;
    message.enabled=enabled;
    memory_->publish(message);
  }

void DialogueNode::OnIntent(const xiaocai_msgs::msg::Intent &message) {
    const auto &intent=message.intent;const auto &text=message.text;
    if(intent.rfind("music_",0)==0)return;
    if(hushed_)return;
    PublishState(xiaocai_robot::common::RobotState::Thinking);
    if(intent=="volume_get") {
      if(!volume_get_client_->service_is_ready()) {Speak("音量查询服务暂时不可用。");return;}
      auto request=std::make_shared<xiaocai_msgs::srv::GetVolume::Request>();
      volume_get_client_->async_send_request(request,[this](rclcpp::Client<xiaocai_msgs::srv::GetVolume>::SharedFuture future) {
        auto response=future.get();Speak("当前音量是百分之"+std::to_string(response->percent)+"。");
      });
    }
    else if(intent=="volume_up"||intent=="volume_down") {
      xiaocai_msgs::msg::VolumeCommand command;
      command.action=static_cast<std::uint8_t>(intent=="volume_up"
        ? xiaocai_robot::common::VolumeAction::Up
        : xiaocai_robot::common::VolumeAction::Down);
      volume_command_->publish(command);
    }
    else if(intent=="volume_set") {
      xiaocai_msgs::msg::VolumeCommand command;
      command.action=static_cast<std::uint8_t>(xiaocai_robot::common::VolumeAction::Set);
      command.percent=message.volume_percent;
      volume_command_->publish(command);
    }
    else if(intent=="quiet_on") {quiet_=true;Request(xiaocai_robot::common::MemoryOperation::SetQuiet,{},0,true);Speak("好的，我会安静地陪着你。需要我时叫小蔡。");}
    else if(intent=="quiet_off") {quiet_=false;Request(xiaocai_robot::common::MemoryOperation::SetQuiet,{},0,false);Speak("好的，我在。");for(auto &s:held_)Speak("提醒你，"+s);held_.clear();}
    else if(intent=="memory_save")Request(xiaocai_robot::common::MemoryOperation::SaveFact,message.content);
    else if(intent=="memory_forget")Request(xiaocai_robot::common::MemoryOperation::ForgetFact,message.content);
    else if(intent=="memory_list")Request(xiaocai_robot::common::MemoryOperation::ListFacts);
    else if(intent=="create_reminder")Request(xiaocai_robot::common::MemoryOperation::AddReminder,message.content,message.delay_seconds);
    else if(intent=="reminder_clarify")Speak("请说清楚多久后提醒你，以及要提醒什么。比如十分钟后提醒我喝水。");
    else EnqueueChat(text);
  }

void DialogueNode::OnResult(const xiaocai_msgs::msg::MemoryResult &message) {
    if(message.request_id.rfind("tool-",0)==0) {
      std::lock_guard<std::mutex> lock(tool_mutex_);
      auto entry=tool_results_.find(message.request_id);
      if(entry!=tool_results_.end())entry->second={{"ok",message.ok},{"error",message.error},
        {"facts",message.facts},{"content",message.content},{"deleted",message.deleted},{"enabled",message.enabled}};
      tool_cv_.notify_all();return;
    }
    auto it=pending_.find(message.request_id);
    if(it==pending_.end())return;
    const auto operation=it->second;
    pending_.erase(it);
    if(operation==xiaocai_robot::common::MemoryOperation::GetQuiet) {
      if(message.ok){quiet_=message.enabled;loaded_=true;}
      return;
    }
    if(operation==xiaocai_robot::common::MemoryOperation::SetQuiet)return;
    if(!message.ok){Speak("这件事刚才没保存成功，请再说一次。");return;}
    if(operation==xiaocai_robot::common::MemoryOperation::SaveFact)Speak("好，我记住了。");
    else if(operation==xiaocai_robot::common::MemoryOperation::ForgetFact)
      Speak(message.deleted>0?"已经忘记了。":"我没有找到这条记忆。");
    else if(operation==xiaocai_robot::common::MemoryOperation::ListFacts) {
      std::string facts;
      for(const auto &fact:message.facts){if(!facts.empty())facts+="；";facts+=fact;}
      Speak(facts.empty()?"你还没有让我记住什么。":"我记得："+facts);
    } else if(operation==xiaocai_robot::common::MemoryOperation::AddReminder)
      Speak("好的，我会提醒你"+message.content+"。");
  }

void DialogueNode::OnDue(const xiaocai_msgs::msg::Reminder &message) {
    if(message.content.empty())return;
    if(quiet_)held_.push_back(message.content);
    else Speak("提醒你，"+message.content);
  }
