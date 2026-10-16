#include "common/mpv.hpp"
#include "music_node/music_controller.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace xiaocai_robot;
using Action = common::MusicAction;
using State = common::MusicState;

namespace {
json Song(const std::string &id, const std::string &title) {
  return {{"id", id},
          {"originalId", id},
          {"name", title},
          {"playFlag", true},
          {"artists", {{{"name", "测试歌手"}}}}};
}

// The test executable also acts as the CLI child. No real player or network
// service is used by these regression cases.
int RunFakeCli(int argc, char **argv) {
  const std::string command = argv[1];
  if (command == "search") {
    std::string keyword;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--keyword")
        keyword = argv[i + 1];
    const auto records = keyword == "两只老虎"
                             ? json::array({Song("tiger", "两只老虎"),
                                            Song("cover", "两只老虎（钢琴版）"),
                                            Song("cover2", "两只老虎"),
                                            Song("cover3", "两只老虎 伴奏")})
                             : json::array({Song("cover", "两只老虎（钢琴版）"),
                                            Song("star", "小星星"),
                                            Song("star-cover", "小星星 (伴奏)"),
                                            Song("windmill", "大风车")});
    std::cout << json{{"code", 200}, {"data", {{"records", records}}}}.dump()
              << std::endl;
    return 0;
  }
  if (command == "play") {
    std::string id;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--encrypted-id")
        id = argv[i + 1];
    const auto result = common::MpvCommand(
        {"fixture-play", id}, std::getenv("XIAOCAI_MUSIC_TEST_SOCKET"));
    std::cout << json{{"success", common::MpvOk(result)}}.dump() << std::endl;
    return 0;
  }
  return 1;
}

class FakePlayer {
public:
  FakePlayer() {
    char directory[] = "/tmp/xiaocai-music-test-XXXXXX";
    if (!::mkdtemp(directory))
      throw std::runtime_error("mkdtemp failed");
    directory_ = directory;
    socket_ = directory_ + "/mpv.sock";
    mpv_fd_ = Listen(socket_);
    backend_fd_ = Listen(directory_ + "/player-daemon.sock");
    worker_ = std::thread([this] { Serve(); });
  }

  ~FakePlayer() {
    stop_ = true;
    worker_.join();
    for (const auto fd : observers_)
      ::close(fd);
    ::close(mpv_fd_);
    ::close(backend_fd_);
    std::filesystem::remove_all(directory_);
  }

  const std::string &Socket() const { return socket_; }

private:
  static int Listen(const std::string &path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(path.begin(), path.end(), address.sun_path);
    if (fd < 0 ||
        ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
            0 ||
        ::listen(fd, 8) != 0)
      throw std::runtime_error("fixture socket setup failed");
    return fd;
  }

  void Serve() {
    bool idle = true, paused = false, ignore_play = false;
    std::string id;
    while (!stop_) {
      pollfd listeners[]{{mpv_fd_, POLLIN, 0}, {backend_fd_, POLLIN, 0}};
      if (::poll(listeners, 2, 20) <= 0)
        continue;
      for (int i = 0; i < 2; ++i) {
        if (!(listeners[i].revents & POLLIN))
          continue;
        const int fd =
            ::accept4(listeners[i].fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
          continue;
        std::string input;
        char c;
        while (::recv(fd, &c, 1, 0) == 1 && c != '\n')
          input.push_back(c);
        if (i == 1) {
          const std::string snapshot =
              "{\"event\":\"stateSnapshot\",\"state\":{}}\n";
          ::send(fd, snapshot.data(), snapshot.size(), MSG_NOSIGNAL);
          observers_.push_back(fd);
          continue;
        }
        const auto request = json::parse(input);
        const auto &command = request["command"];
        const auto name = command[0].get<std::string>();
        json data;
        if (name == "fixture-play") {
          if (!ignore_play)
            id = command[1].get<std::string>();
          ignore_play = false;
          idle = false;
        } else if (name == "fixture-end" || name == "stop") {
          idle = true;
        } else if (name == "fixture-ignore-play") {
          ignore_play = true;
        } else if (name == "get_property") {
          const auto property = command[1].get<std::string>();
          if (property == "idle-active")
            data = idle;
          else if (property == "pause")
            data = paused;
          else if (property == "path")
            data = "fixture://" + id;
          else if (property == "audio-out-params")
            data = idle ? json{} : json{{"samplerate", 44100}};
          else if (property == "audio-device")
            data = "alsa/xiaocai_speaker";
          else if (property == "time-pos")
            data = 2.0;
          else if (property == "duration")
            data = 120.0;
        } else if (name == "set_property" && command[1] == "pause") {
          paused = command[2].get<bool>();
        }
        const auto response =
            json{{"request_id", request.value("request_id", 1)},
                 {"error", "success"},
                 {"data", data}}
                .dump() +
            "\n";
        ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
        ::close(fd);
      }
    }
  }

  std::string directory_, socket_;
  int mpv_fd_, backend_fd_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
  std::vector<int> observers_;
};
} // namespace

int main(int argc, char **argv) {
  if (argc > 1)
    return RunFakeCli(argc, argv);
  try {
    FakePlayer player;
    ::setenv("XIAOCAI_MUSIC_TEST_SOCKET", player.Socket().c_str(), 1);
    MusicSettings settings;
    settings.mpv_socket = player.Socket();
    settings.cli_path = std::filesystem::canonical("/proc/self/exe").string();
    LogManagerConfig config;
    config.directory = std::filesystem::path(player.Socket()).parent_path();
    LogManager log("music_controller_test", config);
    std::mutex mutex;
    MusicPlaybackStatus status;
    std::vector<MusicPlaybackStatus> events;
    MusicController controller(
        settings, log,
        [&](const MusicPlaybackStatus &value) {
          std::lock_guard<std::mutex> lock(mutex);
          status = value;
          events.push_back(value);
        },
        [](const std::string &) {}, [] {});
    const auto current = [&] {
      std::lock_guard<std::mutex> lock(mutex);
      return status;
    };
    const auto wait = [&](const std::function<bool()> &condition,
                          const std::string &label) {
      const auto until = Monotonic() + 6;
      do {
        controller.HandleStateUpdate();
        if (condition()) {
          std::cout << "PASS: " << label << std::endl;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      } while (Monotonic() < until);
      throw std::runtime_error(label + " timed out");
    };
    const auto playing = [&](const std::string &id, std::uint64_t request_id) {
      const auto state = current();
      const auto path =
          common::MpvCommand({"get_property", "path"}, player.Socket());
      return state.state == State::Playing && state.song_id == id &&
             state.request_id == request_id &&
             path.value("data", std::string{}) == "fixture://" + id;
    };
    controller.HandleMusicCommand(Action::Play, "两只老虎", 1);
    wait([&] { return playing("tiger", 1); }, "requested song and metadata");
    controller.HandleMusicCommand(Action::Next, {}, 2);
    wait([&] { return playing("star", 2); },
         "next broadens search and skips all versions of current song");
    controller.HandleMusicCommand(Action::Next, {}, 3);
    wait([&] { return playing("windmill", 3); }, "next skips cover versions");
    controller.HandleMusicCommand(Action::Previous, {}, 4);
    wait([&] { return playing("star", 4); }, "previous restores distinct song");
    controller.HandleSpeechAwake(true);
    wait([&] { return current().state == State::Paused; }, "wake pauses music");
    controller.HandleSpeechAwake(false);
    controller.HandleConversation(common::ConversationPhase::Idle);
    wait([&] { return playing("star", 4); }, "dialogue resumes same song");
    common::MpvCommand({"fixture-end"}, player.Socket());
    wait([&] { return playing("windmill", 0); }, "EOF advances to other song");
    controller.HandleMusicCommand(Action::Next, {}, 5);
    wait([&] { return playing("tiger", 5); }, "queue wraps to first song");
    common::MpvCommand({"fixture-ignore-play"}, player.Socket());
    controller.HandleMusicCommand(Action::Next, {}, 6);
    const auto until = Monotonic() + 0.7;
    while (Monotonic() < until) {
      controller.HandleStateUpdate();
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto &event : events)
        if (event.request_id == 6 && event.state == State::Playing)
          throw std::runtime_error(
              "unchanged audio reported a successful skip");
    }
    controller.HandleAudioPlay("");
    wait([&] { return current().state == State::Stopped; },
         "unchanged audio cannot pass skip confirmation; hush cancels loading");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
