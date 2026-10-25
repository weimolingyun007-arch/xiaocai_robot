#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace xiaocai_robot {

struct AlsaPcmSettings {
  std::string pcm_name{"xiaocai_speaker"};
  std::string slave_pcm{"plug:dmix"};
  std::string softvol_control_name{"Xiaocai Output Gain"};
  int card{0};
  double min_db{-60.0};
  double max_db{6.0};
  int resolution{256};
};

inline std::filesystem::path ConfigureAlsa(const std::string &node_name,
                                           const AlsaPcmSettings &settings) {
  static const std::regex safe_name("^[A-Za-z0-9_]+$");
  if (!std::regex_match(node_name, safe_name))
    throw std::runtime_error("Invalid ALSA config owner name: " + node_name);
  if (!std::regex_match(settings.pcm_name, safe_name))
    throw std::runtime_error("Invalid ALSA PCM name: " + settings.pcm_name);
  if (settings.slave_pcm.empty() ||
      settings.slave_pcm.find_first_of("\"\\\r\n") != std::string::npos ||
      settings.softvol_control_name.empty() ||
      settings.softvol_control_name.find_first_of("\"\\\r\n") !=
          std::string::npos)
    throw std::runtime_error("Invalid ALSA PCM configuration");
  if (settings.card < 0 || settings.resolution <= 0 ||
      !std::isfinite(settings.min_db) || !std::isfinite(settings.max_db) ||
      settings.min_db >= settings.max_db)
    throw std::runtime_error("Invalid ALSA softvol parameters");

  char directory_template[] = "/tmp/xiaocai_alsa.XXXXXX";
  char *directory = ::mkdtemp(directory_template);
  if (!directory)
    throw std::runtime_error("Cannot create temporary ALSA config directory");

  const auto config_path = std::filesystem::path(directory) / "asound.conf";
  std::ofstream output(config_path);
  if (!output)
    throw std::runtime_error("Cannot write ALSA config: " +
                             config_path.string());

  output << "</usr/share/alsa/alsa.conf>\n"
         << "# Generated for " << node_name << " from biz_config.toml.\n\n"
         << "pcm." << settings.pcm_name << " {\n"
         << "  type softvol\n"
         << "  slave.pcm \"" << settings.slave_pcm << "\"\n"
         << "  control { name \"" << settings.softvol_control_name
         << "\"; card " << settings.card << " }\n"
         << "  min_dB " << std::fixed << std::setprecision(1) << settings.min_db
         << "\n"
         << "  max_dB " << settings.max_db << "\n"
         << "  resolution " << settings.resolution << "\n"
         << "}\n\n"
         << "ctl." << settings.pcm_name << " { type hw; card " << settings.card
         << " }\n";
  output.close();
  if (!output)
    throw std::runtime_error("Cannot finish ALSA config: " +
                             config_path.string());

  if (::setenv("ALSA_CONFIG_PATH", config_path.c_str(), 1) != 0)
    throw std::runtime_error("Cannot set ALSA_CONFIG_PATH");
  return config_path;
}

} // namespace xiaocai_robot
