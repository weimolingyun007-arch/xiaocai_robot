#pragma once
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace xiaocai_robot::common {
inline std::string MusicSocket() {
  const char *home = std::getenv("HOME");
  return std::string(home ? home : "/home/lckfb") + "/.config/ncm-cli/mpv.sock";
}

// Each connection has its own request id. Ignore asynchronous mpv events.
inline nlohmann::json MpvCommand(const nlohmann::json &command,
                                 const std::string &path = MusicSocket(),
                                 int timeout_ms = 2000) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return {};
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } closer{fd};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path))
    return {};
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    return {};
  const std::string request =
      nlohmann::json{{"command", command}, {"request_id", 1}}.dump() + "\n";
  if (send(fd, request.data(), request.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(request.size()))
    return {};
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string input;
  while (std::chrono::steady_clock::now() < until && input.size() < 65536) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now())
            .count();
    pollfd event{fd, POLLIN, 0};
    if (poll(&event, 1, static_cast<int>(remaining)) <= 0)
      break;
    char buffer[4096];
    const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0)
      break;
    input.append(buffer, count);
    size_t newline;
    while ((newline = input.find('\n')) != std::string::npos) {
      auto response =
          nlohmann::json::parse(input.substr(0, newline), nullptr, false);
      input.erase(0, newline + 1);
      if (response.is_object() && response.value("request_id", 0) == 1)
        return response;
    }
  }
  return {};
}

// Query playback properties in one core command. PCM file output writes large
// blocks; repeated separate IPC queries contend with its audio lock.
inline nlohmann::json
MpvPlaybackSnapshot(const std::string &path = MusicSocket(),
                    bool include_position = true) {
  const char *expression =
      include_position
          ? "${=idle-active}|${=pause}|${=time-pos:0}|${=duration:0}"
          : "${=idle-active}|${=pause}|0|${=duration:0}";
  const auto response = MpvCommand({"expand-text", expression}, path);
  if (!response.is_object() ||
      response.value("error", std::string{}) != "success" ||
      !response.contains("data") || !response["data"].is_string())
    return {};
  std::istringstream input(response["data"].get<std::string>());
  std::string idle, pause, position, duration;
  if (!std::getline(input, idle, '|') || !std::getline(input, pause, '|') ||
      !std::getline(input, position, '|') || !std::getline(input, duration))
    return {};
  if ((idle != "yes" && idle != "no") || (pause != "yes" && pause != "no"))
    return {};
  try {
    return {{"idle", idle == "yes"},
            {"paused", pause == "yes"},
            {"position", std::stod(position)},
            {"duration", std::stod(duration)}};
  } catch (const std::exception &) {
    return {};
  }
}

inline bool MpvOk(const nlohmann::json &response) {
  return response.is_object() &&
         response.value("error", std::string{}) == "success";
}
} // namespace xiaocai_robot::common
