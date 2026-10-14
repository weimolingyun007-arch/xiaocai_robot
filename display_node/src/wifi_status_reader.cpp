#include "display_node/wifi_status_reader.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace xiaocai_robot {

WifiStatusReader::WifiStatusReader(std::string interface_name)
    : interface_name_(std::move(interface_name)) {
  if (interface_name_.empty() || interface_name_.size() >= IFNAMSIZ ||
      interface_name_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL"
                                        "MNOPQRSTUVWXYZ0123456789_.:-") !=
          std::string::npos)
    throw std::invalid_argument("Invalid Wi-Fi interface name");
}

WifiStatus WifiStatusReader::Read() const {
  WifiStatus status;
  status.available = if_nametoindex(interface_name_.c_str()) != 0;
  if (!status.available)
    return status;

  int carrier = 0;
  std::ifstream("/sys/class/net/" + interface_name_ + "/carrier") >> carrier;
  status.link_up = carrier == 1;
  ifaddrs *addresses = nullptr;
  if (getifaddrs(&addresses) == 0) {
    for (auto *entry = addresses; entry; entry = entry->ifa_next) {
      if (!entry->ifa_addr || interface_name_ != entry->ifa_name)
        continue;
      if (entry->ifa_addr->sa_family == AF_INET) {
        const auto address =
            ntohl(reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)
                      ->sin_addr.s_addr);
        status.connected = status.link_up && address != 0 &&
                           (address >> 24) != 127 && (address >> 16) != 0xa9fe;
      } else if (entry->ifa_addr->sa_family == AF_INET6) {
        const auto &address =
            reinterpret_cast<const sockaddr_in6 *>(entry->ifa_addr)->sin6_addr;
        if (!IN6_IS_ADDR_UNSPECIFIED(&address) &&
            !IN6_IS_ADDR_LOOPBACK(&address) && !IN6_IS_ADDR_LINKLOCAL(&address))
          status.connected = status.link_up;
      }
      if (status.connected)
        break;
    }
    freeifaddrs(addresses);
  }

  std::ifstream wireless("/proc/net/wireless");
  std::string line;
  while (std::getline(wireless, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::istringstream name(line.substr(0, colon));
    std::string interface;
    name >> interface;
    if (interface != interface_name_)
      continue;
    std::istringstream fields(line.substr(colon + 1));
    std::string flags;
    double quality = 0, signal = 0;
    if (fields >> flags >> quality >> signal && std::isfinite(signal)) {
      // Some wireless-extension drivers expose an unsigned 8-bit dBm value.
      if (signal > 0 && signal <= 255)
        signal -= 256;
      if (signal >= -120 && signal <= -1) {
        status.signal_known = true;
        status.strength = signal >= -55   ? 4
                          : signal >= -65 ? 3
                          : signal >= -75 ? 2
                                          : 1;
      }
    }
    break;
  }
  return status;
}

} // namespace xiaocai_robot
