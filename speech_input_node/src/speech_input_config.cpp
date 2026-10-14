#include "speech_input_node/speech_input_config.hpp"
#include <cmath>
#include <filesystem>

namespace xiaocai_robot {

SpeechInputConfig::SpeechInputConfig() : NodeConfigBase("speech_input_node") {
  const auto *section = Document()["speech_input"].as_table();
  if (!section)
    return;
  if (auto value = (*section)["wake_model_path"].value<std::string>())
    settings_.wake_model_path = *value;
  if (auto value = (*section)["sense_model_path"].value<std::string>())
    settings_.sense_model_path = *value;
  if (auto value = (*section)["sense_tokens_path"].value<std::string>())
    settings_.sense_tokens_path = *value;
  if (auto value = (*section)["vad_model_path"].value<std::string>())
    settings_.vad_model_path = *value;
  if (auto value = (*section)["wake_ack_path"].value<std::string>())
    settings_.wake_ack_path = *value;
  if (auto value = (*section)["input_gain"].value<double>())
    settings_.input_gain = *value;
  if (auto value = (*section)["inactivity_timeout_sec"].value<double>())
    settings_.inactivity_timeout_sec = *value;
  if (auto value = (*section)["followup_timeout_sec"].value<double>();
      value && std::isfinite(*value) && *value >= 1.0 && *value <= 120.0)
    settings_.followup_timeout_sec = *value;
}

} // namespace xiaocai_robot
