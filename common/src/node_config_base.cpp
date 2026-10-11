#include "common/node_config_base.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace xiaocai_robot {

namespace {

std::filesystem::path ResolveConfigPath(const char *package_name) {
  if (package_name == nullptr || *package_name == '\0') {
    throw std::invalid_argument("config package name must not be empty");
  }
  if (const char *root = std::getenv("XIAOCAI_CONFIG_ROOT"); root && *root) {
    return std::filesystem::path(root) / package_name / "biz_config.toml";
  }
  return std::filesystem::path(
             ament_index_cpp::get_package_share_directory(package_name)) /
         "config" / "biz_config.toml";
}

} // namespace

NodeConfigBase::NodeConfigBase(const char *package_name)
    : NodeConfigBase(ResolveConfigPath(package_name)) {}

NodeConfigBase::NodeConfigBase(std::filesystem::path config_path)
    : config_path_(std::move(config_path)) {
  Load();
}

void NodeConfigBase::Load() {
  try {
    document_ = toml::parse_file(config_path_.string());
    const toml::table *logging = document_["logging"].as_table();
    if (!logging) {
      throw std::runtime_error("missing [logging] table");
    }

    if (auto value = (*logging)["directory"].value<std::string>()) {
      logging_.directory = *value;
    }
    if (auto value = (*logging)["level"].value<std::string>()) {
      logging_.level = *value;
    }
    if (auto value = (*logging)["pattern"].value<std::string>()) {
      logging_.pattern = *value;
    }
    if (auto value = (*logging)["console"].value<bool>()) {
      logging_.console = *value;
    }
    if (auto value = (*logging)["max_file_size_mb"].value<std::int64_t>()) {
      if (*value > 0 && *value <= 1024) {
        logging_.max_file_size_bytes =
            static_cast<std::size_t>(*value) * 1024U * 1024U;
      }
    }
    if (auto value = (*logging)["max_files_per_day"].value<std::int64_t>()) {
      if (*value > 0 && *value <= 100) {
        logging_.max_files_per_day = static_cast<std::size_t>(*value);
      }
    }
    if (auto value = (*logging)["retention_days"].value<std::int64_t>()) {
      if (*value > 0 && *value <= 3650) {
        logging_.retention_days = static_cast<int>(*value);
      }
    }
  } catch (const std::exception &error) {
    std::cerr << "[node_config] Unable to read " << config_path_ << ": "
              << error.what() << "; using built-in configuration defaults\n";
  }
}

} // namespace xiaocai_robot
