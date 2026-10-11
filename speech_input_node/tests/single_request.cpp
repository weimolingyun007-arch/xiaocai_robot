#include "common/log_manager.hpp"
#include "speech_input_node/speech_input_config.hpp"
#include "speech_input_node/speech_input_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

int main() {
  int status = 0;
  try {
    xiaocai_robot::SpeechInputConfig config;
    xiaocai_robot::LogManager log_manager("speech_input_test",
                                          config.Logging());
    auto settings = config.Settings();
    settings.inactivity_timeout_sec = 1.0;

    std::vector<std::string> requests;
    std::vector<std::string> sounds;
    bool awake = false;
    xiaocai_robot::SpeechInputCallbacks callbacks;
    callbacks.publish_text = [&requests](const std::string &text) {
      requests.push_back(text);
    };
    callbacks.publish_audio_play = [&sounds](const std::string &path) {
      sounds.push_back(path);
    };
    callbacks.publish_awake = [&awake](bool value) { awake = value; };
    xiaocai_robot::SpeechInputController controller(
        std::move(settings), log_manager, std::move(callbacks));

    auto check = [](bool condition, const std::string &label) {
      if (!condition)
        throw std::runtime_error(label);
      std::cout << "PASS: " << label << std::endl;
    };
    auto result = [&controller](const std::string &text, bool was_armed,
                                std::uint64_t generation) {
      controller.HandleRecognitionResult({text, was_armed, generation});
    };

    result("外界讲话", true, 0);
    check(requests.empty(), "standby ignores ambient request");

    const auto first_generation = controller.AcknowledgeWake();
    check(awake, "wake opens request window");
    result("现在音量是多少", true, first_generation);
    result("播放两只老虎", true, first_generation);
    check(requests.size() == 1 && requests.back() == "现在音量是多少" && !awake,
          "one request closes session and discards queued second request");
    check(std::find(sounds.begin(), sounds.end(), "wake_ack_ding.wav") ==
                  sounds.end() &&
              std::find(sounds.begin(), sounds.end(), "") == sounds.end(),
          "accepted speech does not queue a ding or stop the response");

    controller.HandleAudioPlaying(false);
    result("继续说话", true, first_generation + 1);
    check(requests.size() == 1 && !awake,
          "reply completion does not reopen listening");

    const auto second_generation = controller.AcknowledgeWake();
    result("小蔡播放旧歌曲", true, first_generation);
    check(requests.size() == 1 && awake,
          "late old ASR result cannot consume new wake session");
    result("", true, second_generation);
    check(awake, "empty ASR result does not consume request");
    result("播放晴天", true, second_generation);
    check(requests.size() == 2 && requests.back() == "播放晴天" && !awake,
          "next wake accepts exactly one fresh request");

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const auto third_generation = second_generation + 1;
    result("小蔡播放两只老虎", false, third_generation);
    check(requests.size() == 3 && requests.back() == "播放两只老虎" && !awake,
          "wake and request in one utterance also closes session");

    const auto fourth_generation = controller.AcknowledgeWake();
    result("闭嘴", true, fourth_generation);
    check(requests.size() == 3 && !awake && sounds.back().empty(),
          "hush stops audio and waits for the next wake word");

    controller.AcknowledgeWake();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    controller.HandleAudioData(std::vector<std::uint8_t>(3200, 0));
    check(!awake && requests.size() == 3, "unused wake window still times out");
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    status = 1;
  }
  return status;
}
