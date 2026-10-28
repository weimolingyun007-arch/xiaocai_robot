#include "dialogue_node/dialogue_config.hpp"
#include <filesystem>

namespace xiaocai_robot {

DialogueConfig::DialogueConfig() : NodeConfigBase("dialogue_node") {
  const auto *section = Document()["dialogue"].as_table();
  if (!section)
    return;
  if (auto value = (*section)["local_api_url"].value<std::string>())
    settings_.local_api_url = *value;
  if (auto value = (*section)["local_model"].value<std::string>())
    settings_.local_model = *value;
  if (auto value =
          (*section)["local_connect_timeout_seconds"].value<std::int64_t>();
      value && *value >= 1 && *value <= 30)
    settings_.local_connect_timeout_seconds = static_cast<std::int32_t>(*value);
  if (auto value = (*section)["local_timeout_seconds"].value<std::int64_t>();
      value && *value >= 1 && *value <= 120)
    settings_.local_timeout_seconds = static_cast<std::int32_t>(*value);
  if (auto value = (*section)["local_max_tokens"].value<std::int64_t>();
      value && *value >= 32 && *value <= 2048)
    settings_.local_max_tokens = static_cast<std::int32_t>(*value);
  if (auto value = (*section)["weather_default_city"].value<std::string>();
      value && value->size() <= 128)
    settings_.weather_default_city = *value;
  if (auto value = (*section)["weather_timeout_seconds"].value<std::int64_t>();
      value && *value >= 1 && *value <= 15)
    settings_.weather_timeout_seconds = static_cast<std::int32_t>(*value);
  if (auto value = (*section)["api_url"].value<std::string>())
    settings_.api_url = *value;
  if (auto value = (*section)["model"].value<std::string>())
    settings_.model = *value;
}

} // namespace xiaocai_robot
