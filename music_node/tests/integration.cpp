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
#include <xiaocai_msgs/srv/get_volume.hpp>

int main(int argc, char **argv) {
  const bool ui = argc > 1 && std::string(argv[1]) == "--ui";
  const bool controls_only = argc > 1 && std::string(argv[1]) == "--controls";
  const bool music_only = argc > 1 && std::string(argv[1]) == "--music";
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
  auto volume = node->create_client<xiaocai_msgs::srv::GetVolume>(
      "/xiaocai_robot/audio/get_volume");
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
    using Action = xiaocai_robot::common::MusicAction;
    if (ui && (expected == Action::Next || expected == Action::Previous ||
               expected == Action::Pause || expected == Action::Resume)) {
      const auto x = expected == Action::Previous ? "172"
                     : expected == Action::Next   ? "628"
                                                  : "400";
      if (xiaocai_robot::Run(
              {"python3", "/tmp/xiaocai_x11_input.py", "click", x, "437"}) != 0)
        throw std::runtime_error("cannot click music page control");
    } else {
      text->publish(xiaocai_robot::Str(phrase));
    }
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
    if (!controls_only) {
      if (!ui) {
        command("播放晴天", xiaocai_robot::common::MusicAction::Play);
        wait(
            [&] {
              return is(State::Playing) && !state.song_id.empty() &&
                     state.position_seconds > 1;
            },
            90, "search, playback and advancing position");
        hold(12);
        if (!is(State::Playing) || state.position_seconds < 10)
          throw std::runtime_error(
              "player did not survive idle subscriber timeout");
      }
      command("播放两只老虎", xiaocai_robot::common::MusicAction::Play);
      wait(
          [&] {
            return is(State::Playing) && state.title == "两只老虎" &&
                   state.position_seconds > 1;
          },
          90, "requested Two Tigers song through real daemon");
      const auto tiger_id = state.song_id;
      command("下一首", xiaocai_robot::common::MusicAction::Next);
      wait(
          [&] {
            return is(State::Playing) && !state.song_id.empty() &&
                   state.song_id != tiger_id &&
                   state.title.find("两只老虎") == std::string::npos &&
                   state.position_seconds > 1;
          },
          90, "next plays another song instead of a Two Tigers cover");
      command("上一首", xiaocai_robot::common::MusicAction::Previous);
      wait(
          [&] {
            return is(State::Playing) && state.song_id == tiger_id &&
                   state.position_seconds > 1;
          },
          90, "previous restores the requested song and metadata");
      if (state.duration_seconds > 2 &&
          xiaocai_robot::common::MpvOk(xiaocai_robot::common::MpvCommand(
              {"set_property", "time-pos", state.duration_seconds - 0.5}))) {
        wait(
            [&] {
              return is(State::Playing) && !state.song_id.empty() &&
                     state.song_id != tiger_id &&
                     state.title.find("两只老虎") == std::string::npos &&
                     state.position_seconds > 1;
            },
            90, "natural song end advances to a different song");
      } else {
        throw std::runtime_error(
            "cannot seek near EOF for automatic next check");
      }
    }
    if (!music_only) {
      command("播放任意一首音乐", xiaocai_robot::common::MusicAction::Play);
      wait(
          [&] {
            return is(State::Playing) && state.title == "轻音乐" &&
                   state.position_seconds > 1;
          },
          65, "generic song request through existing daemon");
      auto ao =
          xiaocai_robot::common::MpvCommand({"get_property", "current-ao"});
      if (!xiaocai_robot::common::MpvOk(ao) ||
          ao.value("data", std::string{}) != "pcm")
        throw std::runtime_error("music decoder was not using PCM output");
      std::cout << "PASS: music decoder outputs PCM; audio_io owns the speaker"
                << std::endl;
      if (ui) {
        wait([&] { return volume->service_is_ready(); }, 5, "volume service");
        const auto get_volume = [&] {
          auto request =
              std::make_shared<xiaocai_msgs::srv::GetVolume::Request>();
          request->timestamp = node->now();
          auto result = volume->async_send_request(request);
          wait(
              [&] {
                return result.wait_for(std::chrono::seconds(0)) ==
                       std::future_status::ready;
              },
              4, "actual volume query");
          return result.get()->percent;
        };
        const auto saved_volume = get_volume();
        const auto target_y = std::to_string(
            370 - static_cast<int>(std::lround(saved_volume * 196.0 / 150.0)));
        if (xiaocai_robot::Run({"python3", "/tmp/xiaocai_x11_input.py", "drag",
                                "750", target_y, "305"}) != 0)
          throw std::runtime_error("cannot drag volume slider");
        hold(1);
        if (std::abs(get_volume() - 50) > 1)
          throw std::runtime_error(
              "slider did not change actual volume to 50%");
        if (xiaocai_robot::Run({"python3", "/tmp/xiaocai_x11_input.py", "drag",
                                "750", "305", target_y}) != 0)
          throw std::runtime_error("cannot restore volume slider");
        hold(1);
        if (std::abs(get_volume() - saved_volume) > 1)
          throw std::runtime_error("volume did not return to original level");
        std::cout
            << "PASS: volume drag changes and restores actual hardware volume"
            << std::endl;
      } else {
        play_audio("/home/lckfb/xiaocai_robot/resource/speech/wake_ack.wav");
        wait([&] { return is(State::Paused); }, 4,
             "wake response pauses music without discarding the song");
        hold(2);
      }
      command("暂停音乐", xiaocai_robot::common::MusicAction::Pause);
      wait([&] { return is(State::Paused); }, 3, "pause");
      const auto position = state.position_seconds;
      const auto paused_song_id = state.song_id;
      hold(25);
      if (std::abs(state.position_seconds - position) > 0.3)
        throw std::runtime_error("playback position advanced while paused");
      command("继续播放", xiaocai_robot::common::MusicAction::Resume);
      wait(
          [&] {
            return is(State::Playing) &&
                   state.position_seconds > position + 0.5;
          },
          5, "resume");
      hold(5);
      if (!is(State::Playing) || state.song_id != paused_song_id ||
          state.position_seconds < position + 4)
        throw std::runtime_error(
            "decoder did not survive a long pause and resume");
      std::cout
          << "PASS: 25-second pause retains samples and resumes stable playback"
          << std::endl;
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
    }
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    result = 1;
  }
  play_audio("");
  hold(0.5);
  rclcpp::shutdown();
  return result;
}
