#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace xiaocai_robot {

class DisplayWindow {
public:
  explicit DisplayWindow(std::filesystem::path asset_directory);
  ~DisplayWindow();

  DisplayWindow(const DisplayWindow &) = delete;
  DisplayWindow &operator=(const DisplayWindow &) = delete;

  void ShowImage(const std::string &image_name);
  void ProcessEvents();

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace xiaocai_robot
