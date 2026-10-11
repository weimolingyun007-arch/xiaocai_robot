#include "common/common.hpp"
#include "common/mpv.hpp"
#include "defination/music_action.hpp"
#include "defination/music_state.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <rclcpp/rclcpp.hpp>
#include <thread>
#include <xiaocai_msgs/msg/audio_play.hpp>
#include <xiaocai_msgs/msg/music_command.hpp>
#include <xiaocai_msgs/msg/music_state.hpp>

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("music_integration_test");
  xiaocai_msgs::msg::MusicState state;
  bool received = false;
  auto last_action = xiaocai_robot::common::MusicAction::Play;
  unsigned command_count = 0;
  auto state_sub = node->create_subscription<xiaocai_msgs::msg::MusicState>(
      "/xiaocai_robot/music/state", rclcpp::QoS(1).transient_local(),
      [&](xiaocai_msgs::msg::MusicState::SharedPtr m) {
        state = *m;
        received = true;
      });
  auto command_sub = node->create_subscription<xiaocai_msgs::msg::MusicCommand>(
      "/xiaocai_robot/music/command", 10,
      [&](xiaocai_msgs::msg::MusicCommand::SharedPtr m) {
        last_action =
            static_cast<xiaocai_robot::common::MusicAction>(m->action);
        ++command_count;
      });
  auto text = node->create_publisher<std_msgs::msg::String>(
      "/xiaocai_robot/speech/text", 10);
  auto audio = node->create_publisher<xiaocai_msgs::msg::AudioPlay>(
      "/xiaocai_robot/audio/play", 10);
  auto pump = [&] {
    rclcpp::spin_some(node);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  };
  auto wait = [&](const std::function<bool()> &condition, double seconds,
                  const std::string &label) {
    const double until = xiaocai_robot::Monotonic() + seconds;
    while (xiaocai_robot::Monotonic() < until) {
      pump();
      if (condition()) {
        std::cout << "PASS: " << label << std::endl;
        return;
      }
    }
    throw std::runtime_error(
        label + " timed out; state=" + std::to_string(state.state) +
        " error=" + state.error);
  };
  auto hold = [&](double seconds) {
    const double until = xiaocai_robot::Monotonic() + seconds;
    while (xiaocai_robot::Monotonic() < until)
      pump();
  };
  auto command = [&](const std::string &phrase,
                     xiaocai_robot::common::MusicAction expected) {
    const auto before = command_count;
    text->publish(xiaocai_robot::Str(phrase));
    wait([&] { return command_count > before && last_action == expected; }, 3,
         "music action " + std::to_string(static_cast<unsigned>(expected)));
  };
  auto play_audio = [&](const std::string &path) {
    xiaocai_msgs::msg::AudioPlay m;
    m.timestamp = node->now();
    m.path = path;
    audio->publish(m);
  };
  using State = xiaocai_robot::common::MusicState;
  auto is = [&](State value) {
    return state.state == static_cast<std::uint8_t>(value);
  };
  int result = 0;
  try {
    wait(
        [&] {
          return received && text->get_subscription_count() > 0 &&
                 command_sub->get_publisher_count() > 0 &&
                 audio->get_subscription_count() > 1;
        },
        10, "node discovery");
    play_audio("/home/lckfb/xiaocai_robot/resource/speech/wake_ack.wav");
    hold(2);
    command("播放晴天", xiaocai_robot::common::MusicAction::Play);
    wait([&] { return is(State::Playing) && state.position_seconds > 1; }, 65,
         "search, playback and advancing position");
    hold(12);
    if (!is(State::Playing) || state.position_seconds < 10)
      throw std::runtime_error(
          "player did not survive idle subscriber timeout");
    command("播放两只老虎", xiaocai_robot::common::MusicAction::Play);
    wait(
        [&] {
          return is(State::Playing) && state.title == "两只老虎" &&
                 state.position_seconds > 1;
        },
        65, "second song through existing daemon");
    command("播放任意一首音乐", xiaocai_robot::common::MusicAction::Play);
    wait(
        [&] {
          return is(State::Playing) && state.title == "轻音乐" &&
                 state.position_seconds > 1;
        },
        65, "generic song request through existing daemon");
    auto ao = xiaocai_robot::common::MpvCommand({"get_property", "current-ao"});
    if (!xiaocai_robot::common::MpvOk(ao) ||
        ao.value("data", std::string{}) != "alsa")
      throw std::runtime_error("ALSA audio output was not active");
    std::cout << "PASS: real ALSA audio output" << std::endl;
    play_audio("/home/lckfb/xiaocai_robot/resource/speech/wake_ack.wav");
    wait([&] { return is(State::Stopped); }, 4,
         "wake response ends continuous music");
    hold(2);
    command("暂停音乐", xiaocai_robot::common::MusicAction::Pause);
    wait([&] { return is(State::Paused); }, 3, "pause");
    const auto position = state.position_seconds;
    hold(1.5);
    if (std::abs(state.position_seconds - position) > 0.3)
      throw std::runtime_error("playback position advanced while paused");
    command("继续播放", xiaocai_robot::common::MusicAction::Resume);
    wait(
        [&] {
          return is(State::Playing) && state.position_seconds > position + 0.5;
        },
        5, "resume");
    command("停止播放音乐", xiaocai_robot::common::MusicAction::Stop);
    wait([&] { return is(State::Stopped); }, 4, "stop");
    command("播放晴天", xiaocai_robot::common::MusicAction::Play);
    wait([&] { return is(State::Searching); }, 3, "search started");
    play_audio("");
    wait([&] { return is(State::Stopped); }, 4,
         "hush cancels in-flight search");
    hold(2);
    if (!is(State::Stopped))
      throw std::runtime_error("cancelled search restarted playback");
    command("继续播放", xiaocai_robot::common::MusicAction::Resume);
    hold(1);
    if (!is(State::Stopped))
      throw std::runtime_error("music resumed after hush without wake");
    std::cout << "PASS: hush blocks further commands until wake" << std::endl;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    result = 1;
  }
  play_audio("");
  hold(0.5);
  rclcpp::shutdown();
  return result;
}
