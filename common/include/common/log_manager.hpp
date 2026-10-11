#pragma once

#include <spdlog/logger.h>

#include <filesystem>
#include <memory>
#include <string>
#include <cstddef>

namespace xc {

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
// write through logger().
class LogManager {
public:
  LogManager(std::string logger_name, LogManagerConfig config);

  std::shared_ptr<spdlog::logger> logger() const noexcept { return logger_; }

private:
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace xc
