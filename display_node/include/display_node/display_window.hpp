#pragma once

#include "display_node/lyrics_presenter.hpp"
#include "display_node/wifi_status_reader.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace xiaocai_robot {

class DisplayWindow {
public:
  DisplayWindow(std::filesystem::path asset_directory,
                const std::string &font_family);
  ~DisplayWindow();

  DisplayWindow(const DisplayWindow &) = delete;
  DisplayWindow &operator=(const DisplayWindow &) = delete;

  void ShowImage(const std::string &image_name);
  void ShowMusic(const MusicView &view);
  void ShowWifiStatus(const WifiStatus &status);
  void ShowLogin(bool required, bool loading, const std::string &url,
                 const std::string &error);
  void ProcessEvents();

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace xiaocai_robot
