#include "audio_io_node/audio_io_config.hpp"
#include <filesystem>

namespace xiaocai_robot {

AudioIoConfig::AudioIoConfig() : NodeConfigBase("audio_io_node") {
  const auto *section = Document()["audio_io"].as_table();
  if (!section)
    return;
  if (auto value = (*section)["capture_device"].value<std::string>())
    settings_.capture_device = *value;
  if (auto value = (*section)["playback_device"].value<std::string>())
    settings_.playback_device = *value;
  if (auto value = (*section)["wake_ack_path"].value<std::string>())
    settings_.wake_ack_path = *value;
  if (auto value = (*section)["volume_percent"].value<std::int64_t>())
    settings_.volume_percent = *value;
  if (auto value = (*section)["adc_pga_gain"].value<std::int64_t>())
    settings_.adc_pga_gain = *value;
  if (auto value = (*section)["mic_boost_gain"].value<std::int64_t>())
    settings_.mic_boost_gain = *value;
}

} // namespace xiaocai_robot
