#pragma once

#include <spdlog/logger.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace xiaocai_robot {

struct LogManagerConfig {
  std::filesystem::path directory;
  std::string level{"info"};
  std::string pattern{"%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%n] %v"};
  std::size_t max_file_size_bytes{20U * 1024U * 1024U};
  std::size_t max_files_per_day{3U};
  int retention_days{7};
  bool console{true};
};

// Creates a node-specific spdlog logger from already loaded configuration.
// The manager owns the logger; nodes should keep one manager as a member and
// write through Logger().
class LogManager {
public:
  LogManager(std::string logger_name, LogManagerConfig config);

  std::shared_ptr<spdlog::logger> Logger() const noexcept { return logger_; }

private:
  std::shared_ptr<spdlog::logger> logger_;
};

} // namespace xiaocai_robot

// Node methods use these macros to write through their `log_manager_` member.
#define LOG_INFO(...) log_manager_.Logger()->info(__VA_ARGS__)
#define LOG_WARN(...) log_manager_.Logger()->warn(__VA_ARGS__)
#define LOG_ERROR(...) log_manager_.Logger()->error(__VA_ARGS__)
