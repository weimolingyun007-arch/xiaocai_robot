#include "display_node/lyrics_presenter.hpp"

#include <algorithm>
#include <cmath>

namespace xiaocai_robot {

LyricsPresenter::LyricsPresenter() : updated_at_(0.0) {}

bool LyricsPresenter::HandleMusicState(const DisplayMusicState &state,
                                       double monotonic) {
  if (!std::isfinite(state.position_seconds) ||
      !std::isfinite(state.duration_seconds) || state.position_seconds < 0 ||
      state.duration_seconds < 0 || state.song_id.size() > 256 ||
      state.title.size() > 4096 || state.artist.size() > 4096 ||
      static_cast<unsigned int>(state.state) >
          static_cast<unsigned int>(common::MusicState::Error))
    return false;
  if (state.state == common::MusicState::Searching) {
    if (music_state_.state == common::MusicState::Playing ||
        music_state_.state == common::MusicState::Paused) {
      searching_fallback_ = CurrentView(monotonic);
      if (searching_fallback_)
        searching_fallback_->paused = true;
    } else if (music_state_.state != common::MusicState::Searching) {
      searching_fallback_.reset();
    }
  } else if (state.state == common::MusicState::Playing ||
             state.state == common::MusicState::Paused ||
             state.state == common::MusicState::Stopped ||
             state.state == common::MusicState::Error) {
    searching_fallback_.reset();
  }
  music_state_ = state;
  updated_at_ = monotonic;
  return true;
}

bool LyricsPresenter::HandleLyrics(const LyricsTimeline &timeline) {
  if (timeline.song_id.size() > 256 ||
      timeline.start_seconds.size() != timeline.lines.size() ||
      timeline.lines.size() > 5000)
    return false;
  double last = -1.0;
  for (std::size_t index = 0; index < timeline.lines.size(); ++index) {
    const double start = timeline.start_seconds[index];
    if (!std::isfinite(start) || start < 0 || start < last ||
        timeline.lines[index].size() > 4096)
      return false;
    last = start;
  }
  timeline_ = timeline;
  return true;
}

std::optional<MusicView> LyricsPresenter::CurrentView(double monotonic) const {
  if (music_state_.state == common::MusicState::Searching)
    return searching_fallback_;
  if (music_state_.state != common::MusicState::Playing &&
      music_state_.state != common::MusicState::Paused)
    return std::nullopt;
  MusicView view;
  view.title = music_state_.title.empty() ? "音乐播放" : music_state_.title;
  view.artist = music_state_.artist;
  view.paused = music_state_.state == common::MusicState::Paused;
  view.position_seconds =
      music_state_.position_seconds +
      (view.paused ? 0.0 : std::clamp(monotonic - updated_at_, 0.0, 1.0));
  view.duration_seconds = music_state_.duration_seconds;
  if (view.duration_seconds > 0)
    view.position_seconds =
        std::min(view.position_seconds, view.duration_seconds);
  if (timeline_.song_id != music_state_.song_id || timeline_.loading) {
    view.current_line =
        music_state_.song_id.empty() ? "暂无歌词" : "歌词加载中…";
  } else if (timeline_.lines.empty()) {
    view.current_line = "暂无歌词";
  } else {
    const auto next =
        std::upper_bound(timeline_.start_seconds.begin(),
                         timeline_.start_seconds.end(), view.position_seconds);
    const auto index =
        static_cast<std::size_t>(next - timeline_.start_seconds.begin());
    if (index == 0) {
      view.current_line = "音乐前奏";
      view.next_line = timeline_.lines.front();
    } else {
      view.current_line = timeline_.lines[index - 1].empty()
                              ? "音乐间奏"
                              : timeline_.lines[index - 1];
      if (index > 1)
        view.previous_line = timeline_.lines[index - 2];
      if (index < timeline_.lines.size())
        view.next_line = timeline_.lines[index];
    }
  }
  return view;
}

} // namespace xiaocai_robot
