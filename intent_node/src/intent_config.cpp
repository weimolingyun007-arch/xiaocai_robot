#include "intent_node/intent_config.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>

#include <cstdlib>
#include <utility>

namespace {

std::filesystem::path DefaultConfigPath() {
  if (const char *root = std::getenv("XIAOCAI_CONFIG_ROOT"); root && *root) {
    return std::filesystem::path(root) / "intent_node" / "biz_config.toml";
  }
  return std::filesystem::path(
             ament_index_cpp::get_package_share_directory("intent_node")) /
         "config" / "biz_config.toml";
}

}  // namespace

IntentConfig::IntentConfig() : IntentConfig(DefaultConfigPath()) {}

IntentConfig::IntentConfig(std::filesystem::path config_path)
    : xc::NodeConfigBase(std::move(config_path)) {}
