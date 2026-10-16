#pragma once

#include "defination/music_state.hpp"

#include <optional>
#include <string>
#include <vector>

namespace xiaocai_robot {

struct DisplayMusicState {
  common::MusicState state{common::MusicState::Stopped};
  std::string song_id;
  std::string title;
  std::string artist;
  double position_seconds{0.0};
  double duration_seconds{0.0};
};

struct LyricsTimeline {
  std::string song_id;
  bool loading{false};
  std::vector<double> start_seconds;
  std::vector<std::string> lines;
};

struct MusicView {
  std::string title;
  std::string artist;
  std::string previous_line;
  std::string current_line;
  std::string next_line;
  double position_seconds{0.0};
  double duration_seconds{0.0};
  bool paused{false};
};

class LyricsPresenter {
public:
  LyricsPresenter();
  bool HandleMusicState(const DisplayMusicState &state, double monotonic);
  bool HandleLyrics(const LyricsTimeline &timeline);
  std::optional<MusicView> CurrentView(double monotonic) const;

private:
  DisplayMusicState music_state_;
  std::optional<MusicView> searching_fallback_;
  LyricsTimeline timeline_;
  double updated_at_;
};

} // namespace xiaocai_robot
