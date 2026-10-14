#include "music_node/music_login_manager.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace xiaocai_robot {
namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

Json ParseResponse(const std::string &output) {
  const auto begin = output.find('{');
  const auto end = output.rfind('}');
  if (begin == std::string::npos || end == std::string::npos || end <= begin)
    return Json::value_t::discarded;
  return Json::parse(output.substr(begin, end - begin + 1), nullptr, false);
}
} // namespace

MusicLoginManager::MusicLoginManager(std::string cli_path,
                                     LogManager &log_manager,
                                     StatusCallback status_callback,
                                     SpeechCallback speech_callback)
    : cli_path_(std::move(cli_path)), log_manager_(log_manager),
      status_callback_(std::move(status_callback)),
      speech_callback_(std::move(speech_callback)), stop_(false),
      requested_(false) {
  worker_ = std::thread([this] { WorkerLoop(); });
}

MusicLoginManager::~MusicLoginManager() {
  stop_ = true;
  ready_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void MusicLoginManager::RequestLogin() {
  std::lock_guard<std::mutex> lock(mutex_);
  requested_ = true;
  ready_.notify_one();
}

void MusicLoginManager::WorkerLoop() {
  auto next_check = Clock::now();
  auto renew_at = Clock::time_point::min();
  while (!stop_) {
    bool requested = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait_until(lock, next_check,
                        [this] { return stop_ || requested_; });
      if (stop_)
        return;
      requested = requested_;
      requested_ = false;
    }
    if (requested && !status_.required)
      PublishStatus({true, true, {}, {}});
    try {
      const auto response =
          ParseResponse(RunCommand({"login", "--check", "--output", "json"}));
      if (!response.is_object() || !response.contains("success") ||
          !response["success"].is_boolean())
        throw std::runtime_error("login status response is unavailable");
      if (response["success"].get<bool>()) {
        if (status_.required)
          LOG_INFO("NetEase login restored; closing the QR page");
        PublishStatus({});
        next_check = Clock::now() + std::chrono::seconds(60);
        continue;
      }
      const auto message = response.value("message", std::string{});
      if (message.find("未登录") == std::string::npos &&
          message.find("过期") == std::string::npos &&
          message.find("失效") == std::string::npos)
        throw std::runtime_error(
            "login check failed without an expired-session response");

      const bool first = !status_.required;
      if (first || requested)
        speech_callback_("网易云登录已失效，请扫描屏幕二维码登录。");
      if (first || status_.url.empty() || Clock::now() >= renew_at) {
        PublishStatus({true, true, {}, {}});
        const auto output =
            RunCommand({"login", "--background", "--output", "json"});
        static const std::regex kLoginUrl(R"(https://163cn\.tv/[A-Za-z0-9]+)");
        std::smatch match;
        if (!std::regex_search(output, match, kLoginUrl))
          throw std::runtime_error("official login URL is unavailable");
        PublishStatus({true, false, match.str(), {}});
        renew_at = Clock::now() + std::chrono::seconds(120);
        LOG_INFO("NetEase login QR ready; waiting for phone authorization");
      }
      next_check = Clock::now() + std::chrono::seconds(3);
    } catch (const std::exception &error) {
      if (stop_)
        return;
      LOG_WARN("NetEase login status unavailable: {}", error.what());
      if (status_.required) {
        PublishStatus({true, false, {}, "暂时无法获取二维码，正在重试"});
        if (requested)
          speech_callback_("网易云暂时无法连接，请查看屏幕提示。");
      }
      next_check = Clock::now() + std::chrono::seconds(15);
    }
  }
}

void MusicLoginManager::PublishStatus(const MusicLoginStatus &status) {
  if (status_.required == status.required &&
      status_.loading == status.loading && status_.url == status.url &&
      status_.error == status.error)
    return;
  status_ = status;
  status_callback_(status_);
}

std::string
MusicLoginManager::RunCommand(const std::vector<std::string> &arguments) {
  int output[2];
  if (pipe2(output, O_CLOEXEC) != 0)
    throw std::runtime_error("cannot create login command pipe");
  std::vector<std::string> args{cli_path_};
  args.insert(args.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, output[0]);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  pid_t pid = -1;
  const int error =
      posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(output[1]);
  if (error != 0) {
    close(output[0]);
    throw std::runtime_error("cannot start login command");
  }
  fcntl(output[0], F_SETFL, O_NONBLOCK);
  std::string text;
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  int status = 0;
  bool exited = false;
  bool cancelled = false;
  auto drain = [&] {
    char buffer[4096];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) {
      if (text.size() + static_cast<std::size_t>(count) > 65536) {
        cancelled = true;
        break;
      }
      text.append(buffer, count);
    }
  };
  while (!exited) {
    drain();
    const auto waited = waitpid(pid, &status, WNOHANG);
    exited = waited == pid || (waited < 0 && errno != EINTR);
    if (!exited && (cancelled || stop_ || Clock::now() >= deadline)) {
      cancelled = true;
      kill(-pid, SIGTERM);
      const auto grace = Clock::now() + std::chrono::milliseconds(300);
      while (waitpid(pid, &status, WNOHANG) == 0 && Clock::now() < grace)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      kill(-pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      exited = true;
    }
    if (!exited)
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  drain();
  close(output[0]);
  if (cancelled)
    throw std::runtime_error("login command timed out or was cancelled");
  return text;
}

} // namespace xiaocai_robot
