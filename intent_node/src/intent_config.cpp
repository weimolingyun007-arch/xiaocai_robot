#include "intent_node/intent_config.hpp"

#include <utility>

namespace xiaocai_robot {

IntentConfig::IntentConfig() : NodeConfigBase("intent_node") { LoadSettings(); }

IntentConfig::IntentConfig(std::filesystem::path config_path)
    : NodeConfigBase(std::move(config_path)) {
  LoadSettings();
}

void IntentConfig::LoadSettings() {
  const auto *classifier = Document()["classifier"].as_table();
  if (classifier) {
    if (auto value = (*classifier)["model_path"].value<std::string>();
        value && !value->empty())
      settings_.model_path = *value;
    if (auto value = (*classifier)["confidence_threshold"].value<double>();
        value && *value >= 0.0 && *value <= 1.0)
      settings_.confidence_threshold = *value;
    if (auto value = (*classifier)["correction_threshold"].value<double>();
        value && *value >= 0.0 && *value <= 1.0)
      settings_.correction_threshold = *value;
  }

  const auto *section = Document()["intent_feedback"].as_table();
  if (!section)
    return;

  if (auto value = (*section)["enabled"].value<bool>())
    settings_.feedback_collection_enabled = *value;
  if (auto value = (*section)["database_path"].value<std::string>();
      value && !value->empty())
    settings_.feedback_database_path = *value;
}

} // namespace xiaocai_robot
