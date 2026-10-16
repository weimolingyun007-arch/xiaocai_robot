#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace xiaocai_robot {

struct WifiStatus {
  bool available{false};
  bool link_up{false};
  bool connected{false};
  bool signal_known{false};
  int strength{0};
  bool speed_known{false};
  double download_bytes_per_second{0.0};
  double upload_bytes_per_second{0.0};
};

class WifiStatusReader {
public:
  explicit WifiStatusReader(std::string interface_name);
  WifiStatus Read();

private:
  std::string interface_name_;
  bool previous_sample_valid_{false};
  bool previous_connected_{false};
  std::uint64_t previous_rx_bytes_{0};
  std::uint64_t previous_tx_bytes_{0};
  std::chrono::steady_clock::time_point previous_sample_time_{};
};

} // namespace xiaocai_robot
