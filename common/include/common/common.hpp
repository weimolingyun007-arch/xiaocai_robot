#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <std_msgs/msg/string.hpp>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace xiaocai_robot {
using json = nlohmann::json;
inline std_msgs::msg::String Str(const std::string &value) {
  std_msgs::msg::String msg;
  msg.data = value;
  return msg;
}
inline std_msgs::msg::String Js(const json &value) { return Str(value.dump()); }
inline json Parse(const std::string &s) {
  return json::parse(s, nullptr, false);
}
inline double Monotonic() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
inline int Run(const std::vector<std::string> &args) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    std::vector<char *> argv;
    for (auto &s : args)
      argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
inline std::string Trim(std::string value) {
  const std::vector<std::string> marks = {"，", "。", "！", "？"};
  auto ascii = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
           c == '.' || c == '!' || c == '?';
  };
  bool changed = true;
  while (changed && !value.empty()) {
    changed = false;
    if (ascii(static_cast<unsigned char>(value.front()))) {
      value.erase(0, 1);
      changed = true;
    } else if (ascii(static_cast<unsigned char>(value.back()))) {
      value.pop_back();
      changed = true;
    } else
      for (const auto &mark : marks) {
        if (value.rfind(mark, 0) == 0) {
          value.erase(0, mark.size());
          changed = true;
          break;
        }
        if (value.size() >= mark.size() &&
            value.compare(value.size() - mark.size(), mark.size(), mark) == 0) {
          value.erase(value.size() - mark.size());
          changed = true;
          break;
        }
      }
  }
  return value;
}
inline std::string Utf8Prefix(const std::string &value, size_t limit) {
  if (value.size() <= limit)
    return value;
  size_t end = limit;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
    --end;
  return value.substr(0, end);
}
} // namespace xiaocai_robot
