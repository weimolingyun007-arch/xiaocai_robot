#pragma once

#include "common/node_config_base.hpp"

#include <string>

namespace xiaocai_robot {

struct WifiSettings {
  std::string interface { "wlan0" };
  bool auto_reconnect{true};
  int reconnect_interval_sec{10};
};

class WifiConfig : public NodeConfigBase {
public:
  WifiConfig();
  const WifiSettings &Settings() const noexcept { return settings_; }

private:
  WifiSettings settings_;
};

} // namespace xiaocai_robot
