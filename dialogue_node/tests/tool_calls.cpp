#include "defination/music_action.hpp"
#include "dialogue_node/dialogue_node.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <xiaocai_msgs/msg/dialogue_command.hpp>
#include <xiaocai_msgs/msg/music_command.hpp>
#include <xiaocai_msgs/msg/tts_request.hpp>

using namespace std::chrono_literals;

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  int status = 0;
  try {
    auto node = std::make_shared<rclcpp::Node>("tool_test_peer");
    auto dialogue = std::make_shared<xiaocai_robot::DialogueNode>();
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    executor.add_node(dialogue);
    int volume = 60, volume_commands = 0, memory_writes = 0, music_commands = 0;
    std::vector<std::string> answers;
    auto dialogue_command =
        node->create_publisher<xiaocai_msgs::msg::DialogueCommand>(
            "/xiaocai_robot/dialogue/command", 10);
    auto awake = node->create_publisher<std_msgs::msg::Bool>(
        "/xiaocai_robot/speech/awake", 10);
    auto interrupt = node->create_publisher<xiaocai_msgs::msg::AudioPlay>(
        "/xiaocai_robot/audio/play", 10);
    auto volume_state = node->create_publisher<std_msgs::msg::Int32>(
        "/xiaocai_robot/audio/volume_state", rclcpp::QoS(1).transient_local());
    auto memory_result =
        node->create_publisher<xiaocai_msgs::msg::MemoryResult>(
            "/xiaocai_robot/memory/result", 10);
    auto music_state = node->create_publisher<xiaocai_msgs::msg::MusicState>(
        "/xiaocai_robot/music/state", rclcpp::QoS(1).transient_local());
    auto volume_sub =
        node->create_subscription<xiaocai_msgs::msg::VolumeCommand>(
            "/xiaocai_robot/audio/volume_command", 10,
            [&](xiaocai_msgs::msg::VolumeCommand::SharedPtr m) {
              ++volume_commands;
              volume = m->action == 3
                           ? m->percent
                           : std::clamp(volume + (m->action == 1 ? 10 : -10), 0,
                                        100);
              std_msgs::msg::Int32 state;
              state.data = volume;
              volume_state->publish(state);
            });
    auto memory_sub =
        node->create_subscription<xiaocai_msgs::msg::MemoryCommand>(
            "/xiaocai_robot/memory/command", 10,
            [&](xiaocai_msgs::msg::MemoryCommand::SharedPtr m) {
              xiaocai_msgs::msg::MemoryResult result;
              result.timestamp = node->now();
              result.request_id = m->request_id;
              result.operation = m->operation;
              result.ok = true;
              result.content = m->content;
              if (m->operation == 1 || m->operation == 2 || m->operation == 4)
                ++memory_writes;
              if (m->operation == 2)
                result.deleted = 1;
              if (m->operation == 3)
                result.facts = {"我喜欢茶"};
              memory_result->publish(result);
            });
    rclcpp::TimerBase::SharedPtr music_timer;
    auto music_sub = node->create_subscription<xiaocai_msgs::msg::MusicCommand>(
        "/xiaocai_robot/music/command", 10,
        [&](xiaocai_msgs::msg::MusicCommand::SharedPtr m) {
          ++music_commands;
          // An unrelated stale playing notification must not finish this tool.
          xiaocai_msgs::msg::MusicState stale;
          stale.timestamp = node->now();
          stale.state = 2;
          stale.title = "旧歌曲";
          stale.request_id = 999;
          music_state->publish(stale);
          const auto id = m->request_id;
          music_timer = node->create_wall_timer(1200ms, [&, id] {
            xiaocai_msgs::msg::MusicState result;
            result.timestamp = node->now();
            result.state = 2;
            result.title = "晴天";
            result.request_id = id;
            music_state->publish(result);
            music_timer->cancel();
          });
        });
    auto tts = node->create_subscription<xiaocai_msgs::msg::TtsRequest>(
        "/xiaocai_robot/tts/request", 10,
        [&](xiaocai_msgs::msg::TtsRequest::SharedPtr m) {
          answers.push_back(m->text);
        });
    auto spin_for = [&](std::chrono::milliseconds duration) {
      const auto until = std::chrono::steady_clock::now() + duration;
      while (std::chrono::steady_clock::now() < until) {
        executor.spin_some();
        std::this_thread::sleep_for(10ms);
      }
    };
    auto require = [](bool ok, const std::string &label) {
      if (!ok)
        throw std::runtime_error(label);
      std::cout << "PASS " << label << std::endl;
    };
    auto send = [&](const char *text) {
      xiaocai_msgs::msg::DialogueCommand m;
      m.timestamp = node->now();
      m.type =
          static_cast<std::uint8_t>(xiaocai_robot::common::IntentType::Chat);
      m.text = text;
      dialogue_command->publish(m);
    };
    auto wait_answer = [&](const char *expected) {
      const auto deadline = std::chrono::steady_clock::now() + 8s;
      while (std::chrono::steady_clock::now() < deadline) {
        spin_for(30ms);
        if (std::find(answers.begin(), answers.end(), expected) !=
            answers.end())
          return;
      }
      throw std::runtime_error(std::string("missing reply: ") + expected);
    };
    spin_for(1000ms);
    xiaocai_msgs::msg::MusicState stopped;
    stopped.timestamp = node->now();
    music_state->publish(stopped);
    spin_for(100ms);
    std_msgs::msg::Int32 initial_volume;
    initial_volume.data = volume;
    volume_state->publish(initial_volume);
    spin_for(100ms);
    send("all_tools");
    spin_for(50ms);
    std_msgs::msg::Bool standby;
    standby.data = false;
    awake->publish(standby);
    wait_answer("all_tools_ok");
    require(volume == 73 && volume_commands == 1 && memory_writes == 3,
            "8 tools execute through live ROS callbacks; standby preserves "
            "request");
    require(answers.size() == 1, "tool volume callback does not speak twice");
    send("invalid_tools");
    wait_answer("invalid_tools_ok");
    require(volume_commands == 1 && memory_writes == 3,
            "unknown/malformed/out-of-range tools perform no action");
    send("duplicate_call");
    wait_answer("duplicate_call_ok");
    require(volume_commands == 2,
            "repeated tool_call_id does not execute twice");
    const auto answer_count = answers.size();
    send("music_tool");
    spin_for(2200ms);
    require(
        music_commands == 1 && answers.size() == answer_count,
        "music command correlates result; no speech pauses successful song");
    send("hush_cancel");
    spin_for(350ms);
    xiaocai_msgs::msg::AudioPlay hush;
    hush.timestamp = node->now();
    hush.path.clear();
    interrupt->publish(hush);
    spin_for(2500ms);
    require(volume_commands == 2 && answers.size() == answer_count,
            "hush cancels in-flight HTTP before tool execution and reply");
    send("recovery");
    spin_for(200ms);
    require(answers.size() == answer_count,
            "hush ignores further requests until a new wake");
    std_msgs::msg::Bool wake;
    wake.data = true;
    awake->publish(wake);
    spin_for(100ms);
    send("recovery");
    wait_answer("recovered_ok");
    require(answers.size() == answer_count + 1,
            "worker recovers after cancellation");
    executor.remove_node(dialogue);
    dialogue.reset();
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << std::endl;
    status = 1;
  }
  rclcpp::shutdown();
  return status;
}
