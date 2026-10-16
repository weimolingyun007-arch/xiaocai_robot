#pragma once

#include "defination/music_action.hpp"
#include "display_node/lyrics_presenter.hpp"
#include "display_node/wifi_status_reader.hpp"

#include <filesystem>
#include <functional>
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
  bool IsVolumeOverlayActive() const;
  void ShowWifiStatus(const WifiStatus &status);
  void RefreshClock();
  void
  SetMusicControlCallback(std::function<void(common::MusicAction)> callback);
  void SetVolumeControlCallback(std::function<void(int)> callback);
  void ShowVolume(int percent);
  void ShowLogin(bool required, bool loading, const std::string &url,
                 const std::string &error);
  void ProcessEvents();

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace xiaocai_robot
