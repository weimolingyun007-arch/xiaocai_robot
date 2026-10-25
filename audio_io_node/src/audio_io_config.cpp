#include "audio_io_node/audio_io_config.hpp"
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace xiaocai_robot {

namespace {
AlsaPcmSettings ReadAlsaPcmSettings(const toml::table *table) {
  AlsaPcmSettings settings;
  if (!table)
    return settings;
  if (auto value = (*table)["pcm_name"].value<std::string>())
    settings.pcm_name = *value;
  if (auto value = (*table)["slave_pcm"].value<std::string>())
    settings.slave_pcm = *value;
  if (auto value = (*table)["softvol_control_name"].value<std::string>())
    settings.softvol_control_name = *value;
  if (auto value = (*table)["card"].value<std::int64_t>()) {
    if (*value < 0 || *value > std::numeric_limits<int>::max())
      throw std::runtime_error("ALSA card is outside the supported range");
    settings.card = static_cast<int>(*value);
  }
  if (auto value = (*table)["min_db"].value<double>())
    settings.min_db = *value;
  if (auto value = (*table)["max_db"].value<double>())
    settings.max_db = *value;
  if (auto value = (*table)["resolution"].value<std::int64_t>()) {
    if (*value < 1 || *value > std::numeric_limits<int>::max())
      throw std::runtime_error(
          "ALSA softvol resolution is outside the supported range");
    settings.resolution = static_cast<int>(*value);
  }
  return settings;
}

} // namespace

AudioIoConfig::AudioIoConfig() : NodeConfigBase("audio_io_node") {
  const auto *section = Document()["audio_io"].as_table();
  if (!section)
    return;
  settings_.alsa = ReadAlsaPcmSettings((*section)["alsa"].as_table());
  if (auto value = (*section)["capture_device"].value<std::string>())
    settings_.capture_device = *value;
  if (auto value = (*section)["playback_device"].value<std::string>())
    settings_.playback_device = *value;
  if (auto value = (*section)["wake_ack_path"].value<std::string>())
    settings_.wake_ack_path = *value;
  if (auto value = (*section)["startup_music_path"].value<std::string>())
    settings_.startup_music_path = *value;
  if (auto value = (*section)["volume_percent"].value<std::int64_t>())
    settings_.volume_percent = *value;
  if (auto value = (*section)["adc_pga_gain"].value<std::int64_t>())
    settings_.adc_pga_gain = *value;
  if (auto value = (*section)["mic_boost_gain"].value<std::int64_t>())
    settings_.mic_boost_gain = *value;
}

std::filesystem::path AudioIoConfig::StartupMusicPath() const {
  const std::filesystem::path configured_path(settings_.startup_music_path);
  if (configured_path.is_absolute())
    return configured_path;

  const char *resource_root = std::getenv("XIAOCAI_RESOURCE_ROOT");
  const auto root = resource_root
                        ? std::filesystem::path(resource_root)
                        : std::filesystem::current_path() / "resource";
  return root / configured_path;
}

} // namespace xiaocai_robot
