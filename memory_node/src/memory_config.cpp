#include "memory_node/memory_config.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path default_config_path() {
  if (const char *root = std::getenv("XIAOCAI_CONFIG_ROOT"); root && *root) {
    return std::filesystem::path(root) / "memory_node" / "biz_config.toml";
  }
  return std::filesystem::path(
             ament_index_cpp::get_package_share_directory("memory_node")) /
         "config" / "biz_config.toml";
}

}  // namespace

MemoryConfig::MemoryConfig()
    : xc::NodeConfigBase(
          default_config_path()) {
  const auto *section = document()["memory"].as_table();
  if (!section) return;
  if (auto value = (*section)["db_path"].value<std::string>()) settings_.db_path = *value;
}
