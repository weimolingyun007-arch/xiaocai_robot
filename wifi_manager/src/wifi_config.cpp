#include "wifi_manager/wifi_config.hpp"

#include <net/if.h>
#include <stdexcept>

namespace xiaocai_robot {

WifiConfig::WifiConfig() : NodeConfigBase("wifi_manager") {
  const auto *section = Document()["wifi"].as_table();
  if (!section)
    return;

  if (auto value = (*section)["interface"].value<std::string>();
      value && !value->empty()) {
    if (value->size() >= IFNAMSIZ ||
        value->find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL"
                                 "MNOPQRSTUVWXYZ0123456789_.:-") !=
            std::string::npos)
      throw std::invalid_argument("Invalid Wi-Fi interface name in config");
    settings_.interface = *value;
  }
  if (auto value = (*section)["auto_reconnect"].value<bool>())
    settings_.auto_reconnect = *value;
  if (auto value = (*section)["reconnect_interval_sec"].value<std::int64_t>();
      value && *value >= 1 && *value <= 300)
    settings_.reconnect_interval_sec = static_cast<int>(*value);
}

} // namespace xiaocai_robot
