#include "music_node/music_config.hpp"
#include "common/mpv.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path DefaultConfigPath() {
  if (const char *root = std::getenv("XIAOCAI_CONFIG_ROOT"); root && *root) {
    return std::filesystem::path(root) / "music_node" / "biz_config.toml";
  }
  return std::filesystem::path(
             ament_index_cpp::get_package_share_directory("music_node")) /
         "config" / "biz_config.toml";
}

}  // namespace

MusicConfig::MusicConfig()
    : xc::NodeConfigBase(
          DefaultConfigPath()) {
  settings_.mpv_socket = xiaocai_robot::common::MusicSocket();
  const auto *section = Document()["music"].as_table();
  if (!section) return;
  if (auto value = (*section)["cli_path"].value<std::string>()) settings_.cli_path = *value;
  if (auto value = (*section)["mpv_socket"].value<std::string>(); value && !value->empty()) settings_.mpv_socket = *value;
  if (auto value = (*section)["default_keyword"].value<std::string>()) settings_.default_keyword = *value;
  if (auto value = (*section)["audio_device"].value<std::string>()) settings_.audio_device = *value;
  if (auto value = (*section)["wake_ack_path"].value<std::string>()) settings_.wake_ack_path = *value;
}
