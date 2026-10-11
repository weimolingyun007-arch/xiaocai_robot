#include "tts_node/tts_node.hpp"
#include "common/common.hpp"
#include <c-api.h>
#include <rclcpp/rclcpp.hpp>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <vector>


Tts::Tts()
    : Node("tts_node"), tts_config_(),
      log_manager_("tts_node", tts_config_.logging()) {
    const auto &settings = tts_config_.settings();
    std::string dir=declare_parameter<std::string>("model_dir",settings.model_dir);
    speaker_=declare_parameter<int>("speaker_id",static_cast<int>(settings.speaker_id));
    model_=dir+"/model.onnx";lexicon_=dir+"/lexicon.txt";tokens_=dir+"/tokens.txt";
    fsts_=dir+"/phone.fst,"+dir+"/date.fst,"+dir+"/number.fst";
    SherpaOnnxOfflineTtsConfig config{};
    config.model.vits.model=model_.c_str();config.model.vits.lexicon=lexicon_.c_str();config.model.vits.tokens=tokens_.c_str();
    config.model.vits.length_scale=1.0f;config.model.vits.noise_scale=0.667f;config.model.vits.noise_scale_w=0.8f;
    config.model.num_threads=2;config.model.provider="cpu";config.rule_fsts=fsts_.c_str();config.max_num_sentences=1;
    tts_=SherpaOnnxCreateOfflineTts(&config);if(!tts_)throw std::runtime_error("Cannot load sherpa TTS model");
    std::filesystem::create_directories("/tmp/xiaocai_tts");
    InitPublishers();
    InitSubscribers();
    log_manager_.logger()->info("sherpa Chinese TTS loaded");
  }

void Tts::InitPublishers() {
    play_=create_publisher<xiaocai_msgs::msg::AudioPlay>("/xiaocai_robot/audio/play",10);
  }

void Tts::InitSubscribers() {
    sub_=create_subscription<std_msgs::msg::String>("/xiaocai_robot/tts/text",10,[this](std_msgs::msg::String::SharedPtr m){synthesize(m->data);});
  }

Tts::~Tts() {if(tts_)SherpaOnnxDestroyOfflineTts(tts_);}

void Tts::synthesize(std::string text) {
    text=xc::utf8_prefix(xc::trim(text),400);if(text.empty())return;
    auto audio=SherpaOnnxOfflineTtsGenerate(tts_,text.c_str(),speaker_,0.95f);
    if(!audio||audio->n<=0){log_manager_.logger()->error("TTS generation failed");return;}
    auto path="/tmp/xiaocai_tts/"+std::to_string(++id_)+".wav";
    std::vector<float> samples(audio->samples,audio->samples+audio->n);
    float peak=0.0f;
    for(float sample:samples) peak=std::max(peak,std::abs(sample));
    if(peak>0.0f) {
      float gain=std::min(3.0f,0.8f/peak);
      for(float &sample:samples) sample=std::clamp(sample*gain,-0.95f,0.95f);
    }
    int ok=SherpaOnnxWriteWave(samples.data(),audio->n,audio->sample_rate,path.c_str());
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    if(ok) {
      xiaocai_msgs::msg::AudioPlay play;
      play.path=path;
      play_->publish(play);
    } else log_manager_.logger()->error("Cannot write TTS wave");
  }
