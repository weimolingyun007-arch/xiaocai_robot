#include "tts_node/tts_config.hpp"
#include <filesystem>

namespace xiaocai_robot {

TtsConfig::TtsConfig() : NodeConfigBase("tts_node") {
  const auto *section = Document()["tts"].as_table();
  if (!section)
    return;
  if (auto value = (*section)["model_dir"].value<std::string>())
    settings_.model_dir = *value;
  if (auto value = (*section)["speaker_id"].value<std::int64_t>())
    settings_.speaker_id = *value;
}

} // namespace xiaocai_robot
