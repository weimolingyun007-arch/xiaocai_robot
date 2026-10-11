#include "speech_input_node/speech_input_node.hpp"
#include "common/common.hpp"
#include <iostream>
#include <stdexcept>

// Exercise the production ROS callbacks with completed ASR results. This test
// does not measure microphone recognition accuracy or synthesize speech.
class SpeechInputTestPeer {
public:
  static void Wake(SpeechInputNode &node) { node.AcknowledgeWake(); }
  static std::uint64_t Generation(SpeechInputNode &node) { return node.recognition_generation_; }
  static bool Listening(SpeechInputNode &node) { return node.armed_until_ > 0; }
  static void Result(SpeechInputNode &node, const std::string &text, bool armed, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(node.queue_mutex_);
    node.results_.push_back({text, armed, generation});
  }
  static void Drain(SpeechInputNode &node) { node.DrainResults(); }
  static void PlaybackEnded(SpeechInputNode &node) { node.OnPlaying(false); }
  static void ResetCooldown(SpeechInputNode &node) { node.last_wake_time_ = 0; }
  static void Timeout(SpeechInputNode &node) {
    node.ignore_until_ = 0;
    node.armed_until_ = xc::Monotonic() - 1;
    std_msgs::msg::UInt8MultiArray silence;
    silence.data.resize(3200);
    node.OnAudio(silence);
  }
};

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  int status = 0;
  {
    auto speech = std::make_shared<SpeechInputNode>();
    auto observer = std::make_shared<rclcpp::Node>("single_request_test");
    std::vector<std::string> requests, sounds;
    bool awake = false;
    auto text_sub = observer->create_subscription<std_msgs::msg::String>("/xiaocai_robot/speech/text", 10,
      [&](std_msgs::msg::String::SharedPtr m) { requests.push_back(m->data); });
    auto sound_sub = observer->create_subscription<xiaocai_msgs::msg::AudioPlay>("/xiaocai_robot/audio/play", 10,
      [&](xiaocai_msgs::msg::AudioPlay::SharedPtr m) { sounds.push_back(m->path); });
    auto awake_sub = observer->create_subscription<std_msgs::msg::Bool>("/xiaocai_robot/speech/awake", 10,
      [&](std_msgs::msg::Bool::SharedPtr m) { awake = m->data; });
    auto pump = [&](double seconds) {
      const double until = xc::Monotonic() + seconds;
      do {
        rclcpp::spin_some(speech);
        rclcpp::spin_some(observer);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } while (xc::Monotonic() < until);
    };
    auto check = [](bool condition, const std::string &label) {
      if (!condition) throw std::runtime_error(label);
      std::cout << "PASS: " << label << std::endl;
    };
    using Peer = SpeechInputTestPeer;
    try {
      pump(2);
      Peer::Result(*speech, "外界讲话", true, Peer::Generation(*speech));
      Peer::Drain(*speech); pump(0.2);
      check(requests.empty(), "standby ignores ambient request");

      Peer::Wake(*speech); pump(0.2);
      check(awake && Peer::Listening(*speech), "wake opens request window");
      const auto first = Peer::Generation(*speech);
      Peer::Result(*speech, "现在音量是多少", true, first);
      Peer::Result(*speech, "播放两只老虎", true, first);
      Peer::Drain(*speech); pump(0.3);
      check(requests.size() == 1 && requests.back() == "现在音量是多少" && !awake && !Peer::Listening(*speech),
        "one request closes session and discards queued second request");
      check(std::find(sounds.begin(), sounds.end(), "wake_ack_ding.wav") == sounds.end() &&
        std::find(sounds.begin(), sounds.end(), "__stop__") == sounds.end(),
        "accepted speech does not queue a ding or stop the response");

      Peer::PlaybackEnded(*speech);
      Peer::Result(*speech, "继续说话", true, Peer::Generation(*speech));
      Peer::Drain(*speech); pump(0.2);
      check(requests.size() == 1 && !Peer::Listening(*speech), "reply completion does not reopen listening");

      Peer::Wake(*speech); pump(0.2);
      const auto second = Peer::Generation(*speech);
      Peer::Result(*speech, "小蔡播放旧歌曲", true, first);
      Peer::Drain(*speech); pump(0.2);
      check(requests.size() == 1 && Peer::Listening(*speech), "late old ASR result cannot consume new wake session");
      Peer::Result(*speech, "", true, second);
      Peer::Drain(*speech);
      check(Peer::Listening(*speech), "empty ASR result does not consume request");
      Peer::Result(*speech, "播放晴天", true, second);
      Peer::Drain(*speech); pump(0.2);
      check(requests.size() == 2 && requests.back() == "播放晴天" && !awake,
        "next wake accepts exactly one fresh request");

      Peer::ResetCooldown(*speech);
      Peer::Result(*speech, "小蔡播放两只老虎", false, Peer::Generation(*speech));
      Peer::Drain(*speech); pump(0.2);
      check(requests.size() == 3 && requests.back() == "播放两只老虎" && !awake,
        "wake and request in one utterance also closes session");

      Peer::Wake(*speech);
      Peer::Result(*speech, "闭嘴", true, Peer::Generation(*speech));
      Peer::Drain(*speech); pump(0.2);
      check(requests.size() == 3 && !awake && sounds.back() == "__stop__", "hush retains immediate stop behavior");
      Peer::Wake(*speech);
      Peer::Timeout(*speech); pump(0.2);
      check(!Peer::Listening(*speech) && !awake && requests.size() == 3, "unused wake window still times out");
    } catch (const std::exception &error) {
      std::cerr << "FAIL: " << error.what() << std::endl;
      status = 1;
    }
  }
  rclcpp::shutdown();
  return status;
}
