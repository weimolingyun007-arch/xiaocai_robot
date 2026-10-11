#include "dialogue_node/dialogue_config.hpp"
#include <filesystem>

namespace xiaocai_robot {

DialogueConfig::DialogueConfig() : NodeConfigBase("dialogue_node") {
  const auto *section = Document()["dialogue"].as_table();
  if (!section)
    return;
  if (auto value = (*section)["api_url"].value<std::string>())
    settings_.api_url = *value;
  if (auto value = (*section)["model"].value<std::string>())
    settings_.model = *value;
}

} // namespace xiaocai_robot
