#pragma once

#include "common/node_config_base.hpp"

#include <filesystem>

namespace xiaocai_robot {

struct DisplaySettings {
  std::filesystem::path asset_directory{"display"};
};

class DisplayConfig : public NodeConfigBase {
public:
  DisplayConfig();

  const DisplaySettings &Settings() const noexcept { return settings_; }
  std::filesystem::path AssetDirectory() const;

private:
  DisplaySettings settings_;
};

} // namespace xiaocai_robot
