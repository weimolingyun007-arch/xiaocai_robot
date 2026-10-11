#include "tts_node/tts_config.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path default_config_path() {
  if (const char *root = std::getenv("XIAOCAI_CONFIG_ROOT"); root && *root) {
    return std::filesystem::path(root) / "tts_node" / "biz_config.toml";
  }
  return std::filesystem::path(
             ament_index_cpp::get_package_share_directory("tts_node")) /
         "config" / "biz_config.toml";
}

}  // namespace

TtsConfig::TtsConfig()
    : xc::NodeConfigBase(
          default_config_path()) {
  const auto *section = document()["tts"].as_table();
  if (!section) return;
  if (auto value = (*section)["model_dir"].value<std::string>()) settings_.model_dir = *value;
  if (auto value = (*section)["speaker_id"].value<std::int64_t>()) settings_.speaker_id = *value;
}
