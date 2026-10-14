#pragma once

#include <string>

namespace xiaocai_robot {

struct WifiStatus {
  bool available{false};
  bool link_up{false};
  bool connected{false};
  bool signal_known{false};
  int strength{0};
};

class WifiStatusReader {
public:
  explicit WifiStatusReader(std::string interface_name);
  WifiStatus Read() const;

private:
  std::string interface_name_;
};

} // namespace xiaocai_robot
