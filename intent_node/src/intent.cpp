#include "intent_node/intent.hpp"
#include "intent_node/music_intent.hpp"
#include "xiaocai_cpp/common.hpp"
#include <rclcpp/rclcpp.hpp>
#include <algorithm>
#include <cctype>
#include <regex>

static int chinese_number(const std::string &s) {
  if(!s.empty() && std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isdigit(c);})) return std::stoi(s);
  if(s=="一百"||s=="百") return 100;
  const std::vector<std::string> d={"零","一","二","三","四","五","六","七","八","九"};
  auto digit=[&](const std::string &x){if(x=="两")return 2;for(int i=0;i<10;++i)if(x==d[i])return i;return 0;};
  size_t p=s.find("十");
  if(p!=std::string::npos) return (p==0?1:digit(s.substr(0,p)))*10 + (p+3>=s.size()?0:digit(s.substr(p+3)));
  return digit(s);
}

Intent::Intent():Node("intent_node") {
    pub_=create_publisher<xiaocai_msgs::msg::Intent>("/xiaocai_robot/intent",10);
    sub_=create_subscription<std_msgs::msg::String>("/xiaocai_robot/speech/text",10,[this](std_msgs::msg::String::SharedPtr m){on_text(m->data);});
  }

void Intent::on_text(std::string text) {
    text=xc::trim(text); if(text.empty())return;
    text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c==' '||c=='\t'||c=='\n'||c=='\r';}),text.end());
    std::string intent="chat",content;int volume_percent=0,delay_seconds=0;std::smatch match;
    const auto music=parse_music_intent(text);
    // Keep simple commands local. Compound requests need the complete sentence
    // so DeepSeek can call several tools instead of losing the other action.
    const bool compound=std::regex_search(text,std::regex("顺便|并且|同时|另外|然后|还有|再帮我|以及"))||
      (std::regex_search(text,std::regex("几点|时间|日期|星期|几号"))&&
       std::regex_search(text,std::regex("音量|喇叭|播放|音乐|提醒|记住|忘记")));
    if(compound)intent="chat";
    else if(music) {intent=music->intent;content=music->content;}
    else if(std::regex_search(text,std::regex("(?:我)?(?:现在|当前|目前)?(?:的)?(?:音量|声音)(?:现在|当前|目前)?(?:是)?(?:多少|多大|几(?:档|成|分贝)?)(?:吗|呢|啊|呀)?|(?:多少|多大)(?:音量|声音)"))) intent="volume_get";
    else if(std::regex_search(text,match,std::regex("(?:音量|声音|喇叭)(?:调到|调成|设为|设置到|到)?(?:百分之)?([0-9]{1,3}|一百|(?:一|二|两|三|四|五|六|七|八|九)?十(?:一|二|三|四|五|六|七|八|九)?|零|一|二|两|三|四|五|六|七|八|九)(?:%|％)?"))) {
      intent="volume_set";volume_percent=std::clamp(chinese_number(match[1]),0,150);
    }
    else if(text.find("静音")!=std::string::npos && text.find("取消") == std::string::npos) {intent="volume_set";volume_percent=0;}
    else if(std::regex_match(text,std::regex("(?:你俩|你量|音量)(?:再)?大(?:一点|点)?"))) intent="volume_up";
    else if(std::regex_match(text,std::regex("(?:你俩|你量|音量)(?:再)?小(?:一点|点)?"))) intent="volume_down";
    else if(std::regex_search(text,std::regex("(?:音量|声音|喇叭).*?(?:调大|加大|提高|增大|调高|大一点|大点|更大)|(?:帮我)?(?:把)?(?:调大|加大|提高|增大|调高)(?:一下|一点|点)?(?:音量|声音|喇叭)|大声一点|大声点|声音太小"))) intent="volume_up";
    else if(std::regex_search(text,std::regex("(?:音量|声音|喇叭).*?(?:调小|减小|降低|调低|小一点|小点|更小)|(?:帮我)?(?:把)?(?:调小|减小|降低|调低)(?:一下|一点|点)?(?:音量|声音|喇叭)|小声一点|小声点|声音太大"))) intent="volume_down";
    else if(std::regex_search(text,std::regex("取消免打扰|关闭免打扰|可以提醒我|恢复提醒|不再安静"))) intent="quiet_off";
    else if(std::regex_search(text,std::regex("别打扰我|不要打扰|免打扰|安静一会|保持安静"))) intent="quiet_on";
    else if(std::regex_search(text,std::regex("你记得我|你记住了什么|我喜欢什么"))) intent="memory_list";
    else if(std::regex_search(text,match,std::regex("(?:忘记|忘掉|不要记得)(.+)"))) {intent="memory_forget";content=xc::trim(match[1]);}
    else if(std::regex_search(text,match,std::regex("(?:记住|记得)(.+)"))) {intent="memory_save";content=xc::trim(match[1]);}
    else if(std::regex_search(text,match,std::regex("([0-9]+|[一二两三四五六七八九十]+)(?:个)?(秒钟?|分钟?|小时|天)后(?:再)?提醒我(.+)"))) {
      int n=chinese_number(match[1]);std::string unit=match[2],reminder_content=xc::trim(match[3]);
      int factor=unit.find("秒")==0?1:unit.find("分")==0?60:unit.find("小")==0?3600:86400;
      if(n>0 && n*factor<=30*86400 && !reminder_content.empty()) {intent="create_reminder";content=reminder_content;delay_seconds=n*factor;}
      else intent="reminder_clarify";
    } else if(text.find("提醒我")!=std::string::npos) intent="reminder_clarify";
    xiaocai_msgs::msg::Intent message;
    message.intent=intent;message.text=text;message.volume_percent=volume_percent;
    message.content=content;message.delay_seconds=delay_seconds;
    pub_->publish(message);
    RCLCPP_INFO(get_logger(),"%s: %s",intent.c_str(),text.c_str());
  }

int main(int argc,char**argv){rclcpp::init(argc,argv);rclcpp::spin(std::make_shared<Intent>());rclcpp::shutdown();}
