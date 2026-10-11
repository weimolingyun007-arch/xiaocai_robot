#include "speech_input_node/speech_input_config.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path default_config_path() {
  if (const char *root = std::getenv("XIAOCAI_CONFIG_ROOT"); root && *root) {
    return std::filesystem::path(root) / "speech_input_node" / "biz_config.toml";
  }
  return std::filesystem::path(
             ament_index_cpp::get_package_share_directory("speech_input_node")) /
         "config" / "biz_config.toml";
}

}  // namespace

SpeechInputConfig::SpeechInputConfig()
    : xc::NodeConfigBase(
          default_config_path()) {
  const auto *section = document()["speech_input"].as_table();
  if (!section) return;
  if (auto value = (*section)["wake_model_path"].value<std::string>()) settings_.wake_model_path = *value;
  if (auto value = (*section)["sense_model_path"].value<std::string>()) settings_.sense_model_path = *value;
  if (auto value = (*section)["sense_tokens_path"].value<std::string>()) settings_.sense_tokens_path = *value;
  if (auto value = (*section)["vad_model_path"].value<std::string>()) settings_.vad_model_path = *value;
  if (auto value = (*section)["wake_ack_path"].value<std::string>()) settings_.wake_ack_path = *value;
  if (auto value = (*section)["wake_ack_ding_path"].value<std::string>()) settings_.wake_ack_ding_path = *value;
  if (auto value = (*section)["input_gain"].value<double>()) settings_.input_gain = *value;
  if (auto value = (*section)["inactivity_timeout_sec"].value<double>()) settings_.inactivity_timeout_sec = *value;
}
