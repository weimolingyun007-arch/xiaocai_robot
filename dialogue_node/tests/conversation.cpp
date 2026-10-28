#include "dialogue_node/dialogue_controller.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
void Require(bool condition, const char *label) {
  if (!condition)
    throw std::runtime_error(label);
}
} // namespace

int main(int argc, char **argv) {
  using namespace std::chrono_literals;
  try {
    Require(argc == 2, "mock port required");
    xiaocai_robot::DialogueSettings settings;
    const auto base = std::string("http://127.0.0.1:") + argv[1];
    settings.local_api_url = base + "/local";
    settings.api_url = base + "/cloud";
    settings.weather_default_city.clear();
    xiaocai_robot::LogManagerConfig logging;
    logging.directory = "/tmp/xiaocai_conversation_test";
    logging.console = false;
    xiaocai_robot::LogManager log("conversation_test", logging);
    std::mutex mutex;
    std::condition_variable ready;
    std::string answer;
    std::uint64_t request_id = 0;
    int answer_count = 0;
    xiaocai_robot::DialogueController *controller = nullptr;
    xiaocai_robot::DialogueCallbacks callbacks;
    callbacks.speak = [&](const std::string &text, std::uint64_t id) {
      std::lock_guard<std::mutex> lock(mutex);
      answer = text;
      request_id = id;
      ++answer_count;
      ready.notify_one();
    };
    callbacks.publish_memory_command = [&](const auto &command) {
      xiaocai_robot::DialogueMemoryResult result;
      result.request_id = command.request_id;
      result.operation = command.operation;
      result.ok = true;
      controller->HandleMemoryResult(result);
    };
    xiaocai_robot::DialogueController instance(settings, log, callbacks);
    controller = &instance;
    auto send = [&](const char *text, const char *expected) {
      int before;
      {
        std::lock_guard<std::mutex> lock(mutex);
        before = answer_count;
      }
      instance.HandleIntent(xiaocai_robot::common::IntentType::Chat, text);
      std::unique_lock<std::mutex> lock(mutex);
      Require(ready.wait_for(lock, 8s, [&] { return answer_count > before; }),
              "no reply from model pipeline");
      Require(answer == expected,
              "unexpected TTS reply or control JSON leaked");
      return request_id;
    };
    instance.HandleSpeechAwake(true);
    auto id = send("weather_location", "请问你想查询哪个城市？");
    instance.HandleTtsResult(id, true);
    instance.HandleSpeechAwake(true);
    id = send("深圳", "city_context_ok");
    instance.HandleTtsResult(id, true);
    instance.HandleSpeechAwake(true);
    id = send("new_session", "new_session_ok");
    instance.HandleTtsResult(id, true);
    instance.HandleSpeechAwake(true);
    id = send("timeout_question", "还有哪个城市？");
    instance.HandleTtsResult(id, true);
    instance.HandleSpeechAwake(true);
    instance.HandleSpeechAwake(false);
    instance.HandleSpeechAwake(true);
    id = send("after_timeout", "after_timeout_ok");
    instance.HandleTtsResult(id, true);
    instance.HandleSpeechAwake(true);
    id = send("invalid_response", "format_fallback_ok");
    instance.HandleTtsResult(id, true);
    instance.HandleSpeechAwake(true);
    id = send("after_invalid", "after_invalid_ok");
    instance.HandleTtsResult(id, true);
    instance.HandleSpeechAwake(true);
    id = send("repair_response", "format_repair_ok");
    instance.HandleTtsResult(id, true);
    std::cout
        << "PASS weather tool round trip, canonical follow-up history, new "
           "sessions, timeout, invalid local output fallback, local retry and "
           "one bounded formatting pass without tools\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
