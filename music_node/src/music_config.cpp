#include "music_node/music_config.hpp"
#include "common/mpv.hpp"
#include <filesystem>
#include <stdexcept>

namespace xiaocai_robot {

MusicConfig::MusicConfig() : NodeConfigBase("music_node") {
  settings_.mpv_socket = common::MusicSocket();
  settings_.decoder_adapter_path =
      (ConfigPath().parent_path() / "ncm_player_compat.cjs").string();
  const auto *section = Document()["music"].as_table();
  if (!section)
    return;
  if (auto value = (*section)["cli_path"].value<std::string>())
    settings_.cli_path = *value;
  if (auto value = (*section)["mpv_socket"].value<std::string>();
      value && !value->empty())
    settings_.mpv_socket = *value;
  if (auto value = (*section)["default_keyword"].value<std::string>())
    settings_.default_keyword = *value;
  if (auto value = (*section)["pcm_fifo"].value<std::string>())
    settings_.pcm_fifo = *value;
  if (auto value = (*section)["decoder_adapter_path"].value<std::string>())
    settings_.decoder_adapter_path = *value;
  if (auto value = (*section)["sample_rate"].value<std::int64_t>()) {
    if (*value < 8000 || *value > 192000)
      throw std::runtime_error("Unsupported music PCM sample rate");
    settings_.sample_rate = static_cast<std::uint32_t>(*value);
  }
  if (auto value = (*section)["channels"].value<std::int64_t>()) {
    if (*value < 1 || *value > 2)
      throw std::runtime_error("Unsupported music PCM channel count");
    settings_.channels = static_cast<std::uint8_t>(*value);
  }
  if (auto value = (*section)["wake_ack_path"].value<std::string>())
    settings_.wake_ack_path = *value;
}

} // namespace xiaocai_robot
