#include "dialogue_node/dialogue_config.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path default_config_path() {
  if (const char *root = std::getenv("XIAOCAI_CONFIG_ROOT"); root && *root) {
    return std::filesystem::path(root) / "dialogue_node" / "biz_config.toml";
  }
  return std::filesystem::path(
             ament_index_cpp::get_package_share_directory("dialogue_node")) /
         "config" / "biz_config.toml";
}

}  // namespace

DialogueConfig::DialogueConfig()
    : xc::NodeConfigBase(
          default_config_path()) {
  const auto *section = document()["dialogue"].as_table();
  if (!section) return;
  if (auto value = (*section)["api_url"].value<std::string>()) settings_.api_url = *value;
  if (auto value = (*section)["model"].value<std::string>()) settings_.model = *value;
}
