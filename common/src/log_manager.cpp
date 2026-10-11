#include "common/log_manager.hpp"

#include <spdlog/details/file_helper.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace xc {
namespace {

std::string LocalDateKey(std::time_t timestamp) {
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &timestamp);
#else
  localtime_r(&timestamp, &local);
#endif
  std::ostringstream stream;
  stream << std::put_time(&local, "%Y%m%d");
  return stream.str();
}

std::string LocalFileStamp(std::time_t timestamp) {
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &timestamp);
#else
  localtime_r(&timestamp, &local);
#endif
  std::ostringstream stream;
  stream << std::put_time(&local, "%Y%m%d_%H%M%S");
  return stream.str();
}

std::filesystem::path ExpandDirectory(std::filesystem::path path) {
  const std::string value = path.string();
  if (!value.empty() && value.front() == '~') {
    if (const char *home = std::getenv("HOME")) {
      path = std::filesystem::path(home) / value.substr(value.size() > 1 ? 2 : 1);
    }
  } else if (path.is_relative()) {
    const char *home = std::getenv("HOME");
    path = (home ? std::filesystem::path(home) : std::filesystem::current_path()) / path;
  }
  return path.lexically_normal();
}

bool IsDigitString(const std::string &value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isdigit(character);
  });
}

bool ParseLogFilename(const std::string &name, const std::string &prefix,
                        std::string &stamp, std::size_t &rotation_index) {
  constexpr std::size_t kStampSize = 15U;
  if (name.compare(0, prefix.size(), prefix) != 0 ||
      name.size() < prefix.size() + kStampSize + 4U) {
    return false;
  }
  stamp = name.substr(prefix.size(), kStampSize);
  if (stamp[8] != '_' || !IsDigitString(stamp.substr(0, 8)) ||
      !IsDigitString(stamp.substr(9, 6))) {
    return false;
  }

  const std::string suffix = name.substr(prefix.size() + kStampSize);
  if (suffix == ".log") {
    rotation_index = 0U;
    return true;
  }
  if (suffix.size() < 6U || suffix.front() != '.' ||
      suffix.compare(suffix.size() - 4U, 4U, ".log") != 0) {
    return false;
  }

  const std::string index_text = suffix.substr(1U, suffix.size() - 5U);
  if (!IsDigitString(index_text)) {
    return false;
  }
  rotation_index = 0U;
  for (const char digit : index_text) {
    const auto value = static_cast<std::size_t>(digit - '0');
    if (rotation_index > (std::numeric_limits<std::size_t>::max() - value) / 10U) {
      return false;
    }
    rotation_index = rotation_index * 10U + value;
  }
  return rotation_index > 0U;
}


class DailySizeRotatingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
  DailySizeRotatingSink(std::string logger_name, LogManagerConfig config)
      : directory_(std::move(config.directory)),
        filename_prefix_(std::move(logger_name) + "_"),
        max_file_size_bytes_(config.max_file_size_bytes),
        max_files_per_day_(config.max_files_per_day), retention_days_(config.retention_days) {
    std::filesystem::create_directories(directory_);
    BeginDay(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  }

protected:
  void sink_it_(const spdlog::details::log_msg &message) override {
    const auto timestamp = std::chrono::system_clock::to_time_t(message.time);
    const std::string message_day = LocalDateKey(timestamp);
    if (message_day != active_day_) {
      BeginDay(timestamp);
    }

    spdlog::memory_buf_t formatted;
    formatter_->format(message, formatted);
    if (formatted.size() > max_file_size_bytes_) {
      formatted.resize(max_file_size_bytes_);
      if (formatted.size() > 0U) {
        formatted.data()[formatted.size() - 1U] = '\n';
      }
    }

    if (current_size_ + formatted.size() > max_file_size_bytes_) {
      RotateCurrentDay();
    }
    file_.write(formatted);
    current_size_ = file_.size();
  }

  void flush_() override { file_.flush(); }

private:
  std::filesystem::path Filename(std::size_t index) const {
    std::string name = filename_prefix_ + active_stamp_;
    if (index > 0U) {
      name += "." + std::to_string(index);
    }
    return directory_ / (name + ".log");
  }

  void CleanupExpired(const std::string &today) {
    const auto now = std::chrono::system_clock::now();
    const auto cutoff_time = now - std::chrono::hours(24 * (retention_days_ - 1));
    const auto cutoff = LocalDateKey(std::chrono::system_clock::to_time_t(cutoff_time));
    for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      std::string stamp;
      std::size_t rotation_index = 0U;
      if (!ParseLogFilename(name, filename_prefix_, stamp, rotation_index)) {
        continue;
      }
      const std::string date = stamp.substr(0, 8);
      if (date >= today || date >= cutoff) {
        continue;
      }
      std::error_code error;
      std::filesystem::remove(entry.path(), error);
      if (error) {
        std::cerr << "[log_manager] Unable to remove expired log " << entry.path()
                  << ": " << error.message() << '\n';
      }
    }

    // Clean up daily folders created by the previous layout, if any.
    for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
      if (!entry.is_directory()) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (name.size() != 10 || (name[4] != '-' && name[4] != '_') ||
          name[7] != name[4]) {
        continue;
      }
      std::string date;
      date.reserve(8U);
      for (std::size_t index = 0; index < name.size(); ++index) {
        if (index == 4U || index == 7U) {
          continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(name[index]))) {
          date.clear();
          break;
        }
        date.push_back(name[index]);
      }
      if (date.empty() || date >= today || date >= cutoff) {
        continue;
      }
      std::error_code error;
      std::filesystem::remove_all(entry.path(), error);
      if (error) {
        std::cerr << "[log_manager] Unable to remove expired log directory " << entry.path()
                  << ": " << error.message() << '\n';
      }
    }
  }

  std::string LatestStampForDay(const std::string &day) const {
    std::string latest_stamp;
    for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      std::string stamp;
      std::size_t rotation_index = 0U;
      if (!ParseLogFilename(entry.path().filename().string(), filename_prefix_, stamp,
                              rotation_index) ||
          stamp.substr(0, 8) != day) {
        continue;
      }
      if (stamp > latest_stamp) {
        latest_stamp = std::move(stamp);
      }
    }
    return latest_stamp;
  }

  void BeginDay(std::time_t timestamp) {
    file_.close();
    active_day_ = LocalDateKey(timestamp);
    CleanupExpired(active_day_);
    active_stamp_ = LatestStampForDay(active_day_);
    if (active_stamp_.empty()) {
      active_stamp_ = LocalFileStamp(timestamp);
    }

    const auto active_file = Filename(0U);
    std::error_code error;
    current_size_ = std::filesystem::exists(active_file, error) && !error
                        ? std::filesystem::file_size(active_file, error)
                        : 0U;
    if (error) {
      current_size_ = 0U;
    }

    if (current_size_ >= max_file_size_bytes_) {
      RotateCurrentDay();
    } else {
      file_.open(active_file.string(), false);
    }
  }

  void RotateCurrentDay() {
    file_.close();
    for (std::size_t index = max_files_per_day_ - 1U; index > 0U; --index) {
      const auto source = Filename(index - 1U);
      const auto target = Filename(index);
      std::error_code error;
      std::filesystem::remove(target, error);
      error.clear();
      if (std::filesystem::exists(source, error) && !error) {
        std::filesystem::rename(source, target);
      }
    }
    std::error_code error;
    std::filesystem::remove(Filename(0U), error);
    file_.open(Filename(0U).string(), true);
    current_size_ = 0U;
  }

  std::filesystem::path directory_;
  std::string filename_prefix_;
  std::size_t max_file_size_bytes_;
  std::size_t max_files_per_day_;
  int retention_days_;
  std::string active_day_;
  std::string active_stamp_;
  std::size_t current_size_{0U};
  spdlog::details::file_helper file_;
};

std::string SafeFilename(std::string name) {
  std::replace_if(name.begin(), name.end(), [](unsigned char character) {
    return !(std::isalnum(character) || character == '_' || character == '-' || character == '.');
  }, '_');
  return name.empty() ? "node" : name;
}

}  // namespace

LogManager::LogManager(std::string logger_name, LogManagerConfig config) {
  const std::string file_prefix = SafeFilename(logger_name);
  if (config.directory.empty()) {
    config.directory = std::filesystem::path("/userdata/log") / file_prefix;
  } else {
    config.directory = ExpandDirectory(std::move(config.directory));
  }
  std::vector<spdlog::sink_ptr> sinks;
  try {
    sinks.push_back(std::make_shared<DailySizeRotatingSink>(file_prefix, config));
  } catch (const std::exception &error) {
    std::cerr << "[log_manager] Unable to initialize file logging: " << error.what()
              << "; continuing with console logging\n";
  }
  if (config.console || sinks.empty()) {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }

  logger_ = std::make_shared<spdlog::logger>(logger_name, sinks.begin(), sinks.end());
  logger_->set_level(spdlog::level::from_str(config.level));
  logger_->set_pattern(config.pattern);
  logger_->flush_on(spdlog::level::warn);
}

}  // namespace xc
