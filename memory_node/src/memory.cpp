#include "memory_node/memory_node.hpp"
#include "defination/memory_operation.hpp"
#include <rclcpp/rclcpp.hpp>
#include <sqlite3.h>
#include <filesystem>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


Memory::Memory()
    : Node("memory_node"), memory_config_(),
      log_manager_("memory_node", memory_config_.logging()) {
    std::string path=declare_parameter<std::string>("db_path",memory_config_.settings().db_path);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    if(sqlite3_open(path.c_str(),&db_)!=SQLITE_OK)throw std::runtime_error("Cannot open memory database");
    sql("CREATE TABLE IF NOT EXISTS facts(id INTEGER PRIMARY KEY,content TEXT NOT NULL UNIQUE)");
    sql("CREATE TABLE IF NOT EXISTS reminders(id INTEGER PRIMARY KEY,content TEXT NOT NULL,due_at INTEGER NOT NULL,delivered INTEGER NOT NULL DEFAULT 0)");
    sql("CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL)");
    InitPublishers();
    InitSubscribers();
    InitTimers();
  }

void Memory::InitPublishers() {
    result_=create_publisher<xiaocai_msgs::msg::MemoryResult>("/xiaocai_robot/memory/result",10);
    due_=create_publisher<xiaocai_msgs::msg::Reminder>("/xiaocai_robot/reminder/due",10);
  }

void Memory::InitSubscribers() {
    sub_=create_subscription<xiaocai_msgs::msg::MemoryCommand>("/xiaocai_robot/memory/command",10,[this](xiaocai_msgs::msg::MemoryCommand::SharedPtr m){command(*m);});
  }

void Memory::InitTimers() {
    timer_=create_wall_timer(std::chrono::seconds(1),[this]{check_due();});
  }

Memory::~Memory() {if(db_)sqlite3_close(db_);}

void Memory::sql(const char *query) {char *err=nullptr;if(sqlite3_exec(db_,query,nullptr,nullptr,&err)!=SQLITE_OK){std::string e=err?err:"SQL error";sqlite3_free(err);throw std::runtime_error(e);}}

sqlite3_stmt *Memory::prepare(const char *query) {sqlite3_stmt *s=nullptr;if(sqlite3_prepare_v2(db_,query,-1,&s,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));return s;}

void Memory::command(const xiaocai_msgs::msg::MemoryCommand &message) {
    xiaocai_msgs::msg::MemoryResult result;
    result.operation=message.operation;
    result.request_id=message.request_id;
    result.ok=true;
    sqlite3_stmt *s=nullptr;
    try {
      const auto operation=static_cast<xiaocai_robot::common::MemoryOperation>(message.operation);
      if(operation==xiaocai_robot::common::MemoryOperation::SaveFact ||
         operation==xiaocai_robot::common::MemoryOperation::ForgetFact) {
        if(message.content.empty())throw std::runtime_error("empty content");
        const bool forget=operation==xiaocai_robot::common::MemoryOperation::ForgetFact;
        s=prepare(forget?"DELETE FROM facts WHERE content LIKE ?":"INSERT OR IGNORE INTO facts(content) VALUES(?)");
        std::string value=forget?"%"+message.content+"%":message.content;sqlite3_bind_text(s,1,value.c_str(),-1,SQLITE_TRANSIENT);
        if(sqlite3_step(s)!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));
        if(forget)result.deleted=sqlite3_changes(db_);
      } else if(operation==xiaocai_robot::common::MemoryOperation::ListFacts) {
        s=prepare("SELECT content FROM facts ORDER BY id DESC LIMIT 10");
        while(sqlite3_step(s)==SQLITE_ROW)result.facts.emplace_back((const char*)sqlite3_column_text(s,0));
      } else if(operation==xiaocai_robot::common::MemoryOperation::AddReminder) {
        int delay=message.delay_seconds;if(message.content.empty()||delay<1||delay>30*86400)throw std::runtime_error("invalid reminder");
        s=prepare("INSERT INTO reminders(content,due_at) VALUES(?,?)");
        sqlite3_bind_text(s,1,message.content.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,time(nullptr)+delay);
        if(sqlite3_step(s)!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));result.content=message.content;
      } else if(operation==xiaocai_robot::common::MemoryOperation::SetQuiet) {
        s=prepare("INSERT INTO settings(key,value) VALUES('quiet',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        sqlite3_bind_text(s,1,message.enabled?"1":"0",-1,SQLITE_STATIC);
        if(sqlite3_step(s)!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));result.enabled=message.enabled;
      } else if(operation==xiaocai_robot::common::MemoryOperation::GetQuiet) {
        s=prepare("SELECT value FROM settings WHERE key='quiet'");
        result.enabled=sqlite3_step(s)==SQLITE_ROW && std::string((const char*)sqlite3_column_text(s,0))=="1";
      } else throw std::runtime_error("unknown operation");
    } catch(const std::exception &e){result.ok=false;result.error=e.what();log_manager_.logger()->error("Memory command failed: {}",e.what());}
    if(s)sqlite3_finalize(s);result_->publish(result);
  }

void Memory::check_due() {
    auto s=prepare("SELECT id,content FROM reminders WHERE delivered=0 AND due_at<=? ORDER BY due_at LIMIT 10");
    sqlite3_bind_int64(s,1,time(nullptr));std::vector<std::pair<int,std::string>> rows;
    while(sqlite3_step(s)==SQLITE_ROW) rows.emplace_back(sqlite3_column_int(s,0),(const char*)sqlite3_column_text(s,1));
    sqlite3_finalize(s);
    for(auto &row:rows) {
      s=prepare("UPDATE reminders SET delivered=1 WHERE id=?");sqlite3_bind_int(s,1,row.first);sqlite3_step(s);sqlite3_finalize(s);
      xiaocai_msgs::msg::Reminder reminder;
      reminder.id=row.first;
      reminder.content=row.second;
      due_->publish(reminder);
    }
  }
