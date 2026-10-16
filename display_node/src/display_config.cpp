#include "display_node/display_config.hpp"

#include <cstdlib>
#include <string>

namespace xiaocai_robot {

DisplayConfig::DisplayConfig() : NodeConfigBase("display_node") {
  const auto *section = Document()["display"].as_table();
  if (!section)
    return;

  if (auto value = (*section)["asset_directory"].value<std::string>())
    settings_.asset_directory = *value;
  if (auto value = (*section)["font_family"].value<std::string>();
      value && !value->empty())
    settings_.font_family = *value;
}

std::filesystem::path DisplayConfig::AssetDirectory() const {
  if (settings_.asset_directory.is_absolute())
    return settings_.asset_directory;

  const char *resource_root = std::getenv("XIAOCAI_RESOURCE_ROOT");
  const auto root = resource_root
                        ? std::filesystem::path(resource_root)
                        : std::filesystem::current_path() / "resource";
  return root / settings_.asset_directory;
}

} // namespace xiaocai_robot
