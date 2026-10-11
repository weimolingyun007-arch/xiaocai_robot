#include "music_node/music_config.hpp"
#include "common/mpv.hpp"
#include <filesystem>

namespace xiaocai_robot {

MusicConfig::MusicConfig() : NodeConfigBase("music_node") {
  settings_.mpv_socket = common::MusicSocket();
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
  if (auto value = (*section)["audio_device"].value<std::string>())
    settings_.audio_device = *value;
  if (auto value = (*section)["wake_ack_path"].value<std::string>())
    settings_.wake_ack_path = *value;
}

} // namespace xiaocai_robot
