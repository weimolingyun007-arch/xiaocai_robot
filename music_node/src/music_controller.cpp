#include "music_node/music_controller.hpp"
#include "common/mpv.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace xiaocai_robot {

namespace {
using State = common::MusicState;
// PCM frames are 20 ms each. Keep enough decoded audio to absorb network
// stalls without making a normal track wait for the full rebuffer threshold.
constexpr std::size_t kPcmQueueFrames = 500;    // 10 seconds
constexpr std::size_t kPcmStartFrames = 50;     // 1 second
constexpr std::size_t kPcmRebufferFrames = 250; // 5 seconds

MusicTrack TrackFromSong(const json &song) {
  MusicTrack track{
      song.value("id", std::string{}), song.value("name", std::string{}), {}};
  if (song.contains("artists") && song["artists"].is_array()) {
    for (const auto &artist : song["artists"]) {
      if (!artist.is_object() || !artist.contains("name") ||
          !artist["name"].is_string())
        continue;
      if (!track.artist.empty())
        track.artist += " / ";
      track.artist += artist["name"].get<std::string>();
    }
  }
  return track;
}

std::string SongTitleKey(std::string title) {
  // Versions of the same song must not occupy consecutive queue entries.
  for (const auto *mark : {"(", "（", "[", "【"}) {
    const auto position = title.find(mark);
    if (position != std::string::npos && position > 0)
      title.resize(position);
  }
  title.erase(std::remove_if(title.begin(), title.end(),
                             [](unsigned char c) { return std::isspace(c); }),
              title.end());
  std::transform(title.begin(), title.end(), title.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return title;
}

bool SameSongTitle(const std::string &left, const std::string &right) {
  const auto a = SongTitleKey(left);
  const auto b = SongTitleKey(right);
  return !a.empty() && !b.empty() && (a.rfind(b, 0) == 0 || b.rfind(a, 0) == 0);
}

bool IsGenericMusicQuery(const std::string &query) {
  if (query.empty())
    return true;
  for (const auto *phrase :
       {"任意一首音乐", "任意一首歌", "随便一首音乐", "随便一首歌",
        "随便来一首", "随便放首歌", "一首音乐", "一首歌", "音乐", "歌曲"}) {
    if (query == phrase)
      return true;
  }
  return false;
}

bool IsPlayableSong(const json &song) {
  return song.is_object() && song.value("playFlag", false) &&
         song.contains("originalId") && !song["originalId"].is_null() &&
         !song.value("id", std::string{}).empty() &&
         !SongTitleKey(song.value("name", std::string{})).empty();
}

std::string OriginalSongId(const json &song) {
  return song["originalId"].is_string() ? song["originalId"].get<std::string>()
                                        : song["originalId"].dump();
}
double SongDurationSeconds(const json &song) {
  if (!song.contains("duration") || !song["duration"].is_number())
    return 0;
  return std::max(0.0, song["duration"].get<double>() / 1000.0);
}
} // namespace

namespace {
bool SendLine(int fd, const std::string &line) {
  std::size_t sent = 0;
  while (sent < line.size()) {
    const auto count =
        ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReceiveLine(int fd, std::string &line, int timeout_ms) {
  line.clear();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline && line.size() < 8192) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1,
                             static_cast<int>(std::max<std::int64_t>(1, left)));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) ||
        ((descriptor.revents & POLLHUP) && !(descriptor.revents & POLLIN)))
      return false;
    char value = 0;
    const auto count = ::recv(fd, &value, 1, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    if (value == '\n')
      return true;
    line.push_back(value);
  }
  return false;
}

int ConnectUnix(const std::string &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
} // namespace

MusicController::MusicController(MusicSettings settings,
                                 LogManager &log_manager,
                                 StatusCallback status_callback,
                                 SpeechCallback speech_callback,
                                 LoginCallback login_callback,
                                 AudioOutputCallback audio_output_callback)
    : settings_(std::move(settings)), log_manager_(log_manager),
      status_callback_(std::move(status_callback)),
      speech_callback_(std::move(speech_callback)),
      login_callback_(std::move(login_callback)),
      audio_output_callback_(std::move(audio_output_callback)), stop_(false),
      silenced_(false), voice_playing_(false), busy_(false),
      last_voice_at_(0.0), last_conversation_at_(0.0), awake_(false),
      conversation_active_(false), resume_after_dialogue_(false),
      manually_paused_(true), revision_(0), backend_fd_(-1) {
  if (mkfifo(settings_.pcm_fifo.c_str(), 0600) != 0 && errno != EEXIST)
    throw std::runtime_error("Cannot create music PCM FIFO");
  struct stat fifo {};
  if (lstat(settings_.pcm_fifo.c_str(), &fifo) != 0 ||
      !S_ISFIFO(fifo.st_mode) || fifo.st_uid != geteuid())
    throw std::runtime_error("Invalid music PCM FIFO");
  pcm_stream_id_ = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  if (audio_output_callback_) {
    pcm_fd_ =
        open(settings_.pcm_fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (pcm_fd_ < 0)
      throw std::runtime_error("Cannot open decoder PCM FIFO");
    fcntl(pcm_fd_, F_SETPIPE_SZ, 4096);
    pcm_reader_ = std::thread([this] { PcmReaderLoop(); });
    pcm_publisher_ = std::thread([this] { PcmPublishLoop(); });
  }
  SetPlaybackStatus(State::Stopped);
  worker_ = std::thread([this] { WorkerLoop(); });
}

MusicController::~MusicController() {
  stop_ = true;
  ++revision_;
  ready_.notify_all();
  pcm_ready_.notify_all();
  if (worker_.joinable())
    worker_.join();
  StopPcmDecoder();
  CloseBackendSubscription();
  if (pcm_reader_.joinable())
    pcm_reader_.join();
  if (pcm_publisher_.joinable())
    pcm_publisher_.join();
  if (pcm_fd_ >= 0)
    close(pcm_fd_);
  unlink(settings_.pcm_fifo.c_str());
}

void MusicController::HandleMusicCommand(common::MusicAction action,
                                         const std::string &query,
                                         std::uint64_t request_id) {
  if (!common::IsMusicAction(action) || silenced_)
    return;
  resume_after_dialogue_ = false;
  manually_paused_ = action == common::MusicAction::Pause ||
                     action == common::MusicAction::Stop;
  const auto revision = ++revision_;
  if (action == common::MusicAction::Play ||
      action == common::MusicAction::Next ||
      action == common::MusicAction::Previous) {
    PausePcmStream(true);
    if (!audio_output_callback_)
      common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
    SetPlaybackStatus(State::Searching, {}, request_id);
  }
  if (action == common::MusicAction::Stop ||
      action == common::MusicAction::Pause) {
    PausePcmStream(true);
    if (!audio_output_callback_)
      common::MpvCommand(action == common::MusicAction::Stop
                             ? json{"stop"}
                             : json{"set_property", "pause", true},
                         settings_.mpv_socket);
  }
  {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.clear();
    jobs_.push_back({action, query, revision, request_id});
  }
  ready_.notify_one();
  LOG_INFO("MusicNode action: {}; query: {}", common::MusicActionName(action),
           query);
}

void MusicController::HandleAudioPlay(const std::string &path) {
  if (path.empty()) {
    silenced_ = true;
    resume_after_dialogue_ = false;
    manually_paused_ = true;
    ++revision_;
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.clear();
    }
    PausePcmStream(true);
    StopPcmDecoder();
    CloseBackendSubscription();
    SetPlaybackStatus(State::Stopped);
    LOG_INFO("Hush: music stopped and pending requests cancelled");
  } else {
    if (path == settings_.wake_ack_path) {
      silenced_ = false;
    }
    last_voice_at_ = Monotonic();
    PausePcmStream(true);
    if (!audio_output_callback_)
      common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
  }
}

void MusicController::HandleVoicePlaying(bool playing) {
  voice_playing_ = playing;
  last_voice_at_ = Monotonic();
}

void MusicController::HandleSpeechAwake(bool awake) {
  awake_ = awake;
  last_conversation_at_ = Monotonic();
  if (!awake)
    return;
  silenced_ = false;
  bool cancel_loading = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!manually_paused_ &&
        (status_.state == State::Playing || status_.state == State::Paused))
      resume_after_dialogue_ = true;
    cancel_loading = busy_ && status_.state == State::Searching;
  }
  if (cancel_loading) {
    ++revision_;
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.clear();
  }
  PausePcmStream(true);
  if (!audio_output_callback_)
    common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
}

void MusicController::HandleConversation(common::ConversationPhase phase) {
  conversation_active_ = phase != common::ConversationPhase::Idle;
  last_conversation_at_ = Monotonic();
}

void MusicController::HandleStateUpdate() {
  MaintainBackendSubscription();
  if (audio_output_callback_) {
    HandlePcmStateUpdate();
    return;
  }
  if (busy_) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_.state == State::Searching)
      return;
  }
  auto snapshot = common::MpvPlaybackSnapshot(settings_.mpv_socket,
                                              !audio_output_callback_);
  if (!snapshot.is_object() || snapshot.empty())
    return;
  if (silenced_ && !snapshot.value("idle", true)) {
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    SetPlaybackStatus(State::Stopped);
    return;
  }
  bool pcm_active = false;
  double pcm_position = 0;
  if (audio_output_callback_) {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    pcm_active = pcm_active_;
    pcm_position = pcm_position_;
  }
  bool advance = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    advance = snapshot.value("idle", true) && !pcm_active &&
              status_.state == State::Playing && !playlist_.empty() &&
              !manually_paused_ && !busy_;
  }
  if (advance) {
    HandleMusicCommand(common::MusicAction::Next, {}, 0);
    return;
  }
  if (resume_after_dialogue_ && !awake_ && !conversation_active_ &&
      !voice_playing_ && !silenced_ && Monotonic() - last_voice_at_ > 0.8 &&
      Monotonic() - last_conversation_at_ > 0.8) {
    if (snapshot.value("idle", true)) {
      resume_after_dialogue_ = false;
    } else {
      PausePcmStream(false);
      if (common::MpvOk(common::MpvCommand({"set_property", "pause", false},
                                           settings_.mpv_socket))) {
        resume_after_dialogue_ = false;
        LOG_INFO("Dialogue finished; music resumed at the saved position");
      }
    }
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (status_.state == State::Error)
    return;
  status_.state = snapshot.value("idle", true) && !pcm_active ? State::Stopped
                  : snapshot.value("paused", true)            ? State::Paused
                                                              : State::Playing;
  status_.position_seconds =
      audio_output_callback_ ? pcm_position : snapshot.value("position", 0.0);
  if (snapshot.value("duration", 0.0) > 0)
    status_.duration_seconds = snapshot.value("duration", 0.0);
  if (status_.state == State::Stopped) {
    status_.song_id.clear();
    status_.title.clear();
    status_.artist.clear();
  }
  status_callback_(status_);
}

// This FIFO is private to music_node and its decoder. ROS carries the bytes.
void MusicController::ResetPcmStream(bool active) {
  if (!audio_output_callback_)
    return;
  std::lock_guard<std::mutex> lock(pcm_mutex_);
  AudioPcmFrame cancel;
  cancel.action = common::AudioStreamAction::Cancel;
  cancel.stream_id = pcm_stream_id_;
  cancel.sequence = pcm_sequence_++;
  audio_output_callback_(cancel);
  ++pcm_stream_id_;
  pcm_sequence_ = 0;
  pcm_active_ = active;
  pcm_paused_ = false;
  pcm_eof_ = false;
  pcm_position_ = 0;
  pcm_queue_.clear();
  ++pcm_generation_;
  // MPV acknowledges stop before its AO thread necessarily closes the FIFO.
  // Drain through EAGAIN until the previous writer really closes, otherwise
  // its last samples/EOF could be mistaken for the next track's stream.
  std::uint8_t discarded[4096];
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool writer_closed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto count = read(pcm_fd_, discarded, sizeof(discarded));
    if (count == 0) {
      writer_closed = true;
      break;
    }
    if (count < 0 && errno != EINTR && errno != EAGAIN)
      break;
    if (count < 0 && errno == EAGAIN) {
      pollfd descriptor{pcm_fd_, POLLIN, 0};
      poll(&descriptor, 1, 10);
    }
  }
  pcm_ready_.notify_all();
  if (active && !writer_closed) {
    pcm_active_ = false;
    throw std::runtime_error("Previous decoder PCM writer did not close");
  }
}

void MusicController::HandleAudioOutputFailure(std::uint64_t stream_id) {
  {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    if (!pcm_active_ || stream_id != pcm_stream_id_)
      return;
  }
  ++revision_;
  manually_paused_ = true;
  resume_after_dialogue_ = false;
  StopPcmDecoder();
  SetPlaybackStatus(State::Error, "音频设备输出失败，请稍后再试。");
}

void MusicController::StopPcmDecoder(bool next_stream) {
  // Queue stop without waiting for the decoder's large PCM write. Reset drains
  // that write and waits for the writer to close before another track starts.
  common::MpvCommand({"stop"}, settings_.mpv_socket,
                     audio_output_callback_ ? 100 : 2000);
  ResetPcmStream(next_stream);
}

void MusicController::HandlePcmStateUpdate() {
  if (busy_) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_.state == State::Searching)
      return;
  }
  bool active, paused, eof;
  double position;
  {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    active = pcm_active_;
    paused = pcm_paused_;
    eof = pcm_eof_;
    position = pcm_position_;
  }
  if (resume_after_dialogue_ && !awake_ && !conversation_active_ &&
      !voice_playing_ && !silenced_ && Monotonic() - last_voice_at_ > 0.8 &&
      Monotonic() - last_conversation_at_ > 0.8) {
    resume_after_dialogue_ = false;
    if (active) {
      PausePcmStream(false);
      paused = false;
      LOG_INFO("Dialogue finished; music PCM resumed at the saved position");
    }
  }
  bool advance;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    advance = eof && !active && status_.state == State::Playing &&
              !playlist_.empty() && !manually_paused_ && !busy_;
  }
  if (advance) {
    HandleMusicCommand(common::MusicAction::Next, {}, 0);
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (status_.state == State::Error || status_.state == State::Searching)
    return;
  status_.state =
      active ? (paused ? State::Paused : State::Playing) : State::Stopped;
  status_.position_seconds = position;
  if (status_.state == State::Stopped) {
    status_.song_id.clear();
    status_.title.clear();
    status_.artist.clear();
  }
  status_callback_(status_);
}

void MusicController::PausePcmStream(bool paused) {
  if (!audio_output_callback_)
    return;
  std::lock_guard<std::mutex> lock(pcm_mutex_);
  if (paused && !pcm_paused_ && pcm_active_) {
    AudioPcmFrame cancel;
    cancel.action = common::AudioStreamAction::Cancel;
    cancel.stream_id = pcm_stream_id_++;
    cancel.sequence = pcm_sequence_;
    pcm_sequence_ = 0;
    audio_output_callback_(cancel);
  }
  pcm_paused_ = paused;
  pcm_ready_.notify_all();
}

void MusicController::PcmReaderLoop() {
  const std::size_t frame_bytes =
      (settings_.sample_rate / 50) * settings_.channels * 2;
  std::vector<std::uint8_t> pending;
  pending.reserve(frame_bytes);
  std::uint64_t generation = 0;
  bool had_data = false;
  while (!stop_) {
    std::unique_lock<std::mutex> lock(pcm_mutex_);
    pcm_ready_.wait(lock, [this] {
      // Bound decoder read-ahead; pausing the sender naturally backpressures
      // MPV without depending on its control IPC during a blocking write.
      return stop_ ||
             (pcm_active_ && !pcm_eof_ && pcm_queue_.size() < kPcmQueueFrames);
    });
    if (stop_)
      return;
    if (generation != pcm_generation_) {
      generation = pcm_generation_;
      pending.clear();
      had_data = false;
    }
    std::vector<std::uint8_t> bytes(frame_bytes - pending.size());
    const auto count = read(pcm_fd_, bytes.data(), bytes.size());
    if (count > 0) {
      pending.insert(pending.end(), bytes.begin(), bytes.begin() + count);
      had_data = true;
    }
    const bool ended = count == 0 && had_data;
    if (pending.size() == frame_bytes || (ended && !pending.empty())) {
      pending.resize(pending.size() / (settings_.channels * 2) *
                     (settings_.channels * 2));
      pcm_queue_.emplace_back();
      pcm_queue_.back().swap(pending);
      pcm_ready_.notify_all();
    } else if (!ended) {
      pcm_ready_.wait_for(lock, std::chrono::milliseconds(2),
                          [this, generation] {
                            return stop_ || pcm_generation_ != generation;
                          });
    }
    if (ended) {
      pcm_eof_ = true;
      pcm_ready_.notify_all();
    }
  }
}

void MusicController::PcmPublishLoop() {
  auto next_frame = std::chrono::steady_clock::now();
  std::uint64_t generation = 0;
  bool buffering = true;
  bool started = false;
  while (!stop_) {
    std::unique_lock<std::mutex> lock(pcm_mutex_);
    pcm_ready_.wait(lock,
                    [this] { return stop_ || (pcm_active_ && !pcm_paused_); });
    if (stop_)
      return;
    if (generation != pcm_generation_) {
      generation = pcm_generation_;
      next_frame = std::chrono::steady_clock::now();
      buffering = true;
      started = false;
    }
    if (buffering) {
      const auto threshold = started ? kPcmRebufferFrames : kPcmStartFrames;
      pcm_ready_.wait(lock, [this, generation, threshold] {
        return stop_ || !pcm_active_ || pcm_paused_ ||
               pcm_generation_ != generation || pcm_eof_ ||
               pcm_queue_.size() >= threshold;
      });
      if (stop_ || !pcm_active_ || pcm_paused_ || pcm_generation_ != generation)
        continue;
      buffering = false;
      next_frame = std::chrono::steady_clock::now();
      if (started)
        LOG_INFO("Music PCM buffer refilled; playback resumed");
    }
    pcm_ready_.wait_until(lock, next_frame, [this, generation] {
      return stop_ || !pcm_active_ || pcm_paused_ ||
             pcm_generation_ != generation;
    });
    if (stop_ || !pcm_active_ || pcm_paused_ || pcm_generation_ != generation)
      continue;
    if (pcm_queue_.empty() && !pcm_eof_) {
      // Stop the sink while refilling so it does not repeatedly underrun.
      AudioPcmFrame cancel;
      cancel.action = common::AudioStreamAction::Cancel;
      cancel.stream_id = pcm_stream_id_++;
      cancel.sequence = pcm_sequence_;
      pcm_sequence_ = 0;
      audio_output_callback_(cancel);
      buffering = true;
      LOG_WARN("Music PCM buffer empty; waiting for audio to refill");
      continue;
    }
    AudioPcmFrame frame;
    frame.stream_id = pcm_stream_id_;
    frame.sequence = pcm_sequence_++;
    frame.sample_rate = settings_.sample_rate;
    frame.channels = settings_.channels;
    if (pcm_queue_.empty()) {
      frame.action = common::AudioStreamAction::End;
      pcm_active_ = false;
    } else {
      started = true;
      frame.data = std::move(pcm_queue_.front());
      pcm_queue_.pop_front();
      pcm_position_ += static_cast<double>(frame.data.size()) /
                       (settings_.sample_rate * settings_.channels * 2);
    }
    audio_output_callback_(frame);
    next_frame = std::max(next_frame + std::chrono::milliseconds(20),
                          std::chrono::steady_clock::now());
    pcm_ready_.notify_all();
  }
}

void MusicController::WorkerLoop() {
  while (!stop_) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(jobs_mutex_);
      ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_)
        return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    if (!IsJobCurrent(job.revision))
      continue;
    busy_ = true;
    try {
      ExecuteJob(job);
    } catch (const std::exception &error) {
      LOG_ERROR("MusicNode command failed: {}", error.what());
      FailJob(job, "音乐服务暂时出了点问题，请再试一次。");
    }
    busy_ = false;
  }
}

void MusicController::ExecuteJob(const Job &job) {
  using Action = common::MusicAction;
  if (job.action == Action::Play) {
    SetPlaybackStatus(State::Searching, {}, job.request_id);
    PausePcmStream(true);
    if (!audio_output_callback_)
      common::MpvCommand({"set_property", "pause", true}, settings_.mpv_socket);
    const std::string keyword =
        IsGenericMusicQuery(job.query) ? settings_.default_keyword : job.query;
    auto search = RunCliCommand(
        {"search", "song", "--keyword", keyword, "--output", "json"},
        job.revision);
    if (!IsJobCurrent(job.revision))
      return;
    if (!search.ok || !search.data.is_object() ||
        search.data.value("code", 0) != 200 || !search.data.contains("data") ||
        !search.data["data"].is_object() ||
        !search.data["data"].contains("records") ||
        !search.data["data"]["records"].is_array()) {
      const int api_code = search.data.is_object() &&
                                   search.data.contains("code") &&
                                   search.data["code"].is_number_integer()
                               ? search.data["code"].get<int>()
                               : -1;
      LOG_WARN("Music search failed: exit_code={} timed_out={} api_code={} "
               "output_bytes={}",
               search.exit_code, search.timed_out, api_code,
               search.output.size());
      if (search.output.find("unknown command") != std::string::npos ||
          api_code == 401 || api_code == 403 || api_code == 301) {
        const auto login = RunCliCommand(
            {"login", "--check", "--output", "json"}, job.revision);
        if (!IsJobCurrent(job.revision))
          return;
        const auto login_message =
            login.data.is_object() && login.data.contains("message") &&
                    login.data["message"].is_string()
                ? login.data["message"].get<std::string>()
                : std::string{};
        const bool expired =
            login_message.find("未登录") != std::string::npos ||
            login_message.find("过期") != std::string::npos ||
            login_message.find("失效") != std::string::npos;
        if (expired && login.data.contains("success") &&
            login.data["success"].is_boolean() &&
            !login.data["success"].get<bool>()) {
          SetPlaybackStatus(State::Error,
                            "网易云登录已失效，请扫描屏幕二维码登录。",
                            job.request_id);
          if (login_callback_)
            login_callback_();
          else
            speech_callback_("网易云登录已失效，请扫描屏幕二维码登录。");
          return;
        }
        LOG_WARN("ncm-cli search command unavailable or authorization "
                 "rejected; login check exit_code={}",
                 login.exit_code);
      }
      FailJob(job, search.timed_out ? "网易云搜索超时，请稍后再试。"
                                    : "网易云搜索暂时没成功，请稍后再试。");
      return;
    }
    const auto &songs = search.data["data"]["records"];
    const json *selected = nullptr;
    for (const auto &song : songs) {
      if (!IsPlayableSong(song))
        continue;
      if (!selected)
        selected = &song;
      if (song.value("name", std::string{}) == keyword) {
        selected = &song;
        break;
      }
    }
    if (!selected) {
      FailJob(job, "没有找到当前账号可以播放的歌曲，请换一首。");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      playlist_ = {{TrackFromSong(*selected), OriginalSongId(*selected),
                    SongDurationSeconds(*selected)}};
      playlist_index_ = 0;
    }
    AppendPlaylist(songs);
    QueueTrack track;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      track = playlist_.front();
      LOG_INFO("Music queue selected: {} distinct songs", playlist_.size());
    }
    PlayTrack(job, track, 0);
  } else if (job.action == Action::Resume) {
    if (!WaitForSpeechToFinish(job.revision))
      return;
    if (audio_output_callback_) {
      bool active;
      {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        active = pcm_active_;
      }
      if (!active) {
        FailJob(job, "还没有可继续播放的音乐，请先点歌。");
        return;
      }
      PausePcmStream(false);
      SetPlaybackStatus(State::Playing, {}, job.request_id);
      return;
    }
    auto idle = common::MpvCommand({"get_property", "idle-active"},
                                   settings_.mpv_socket);
    if (!common::MpvOk(idle) || idle.value("data", true)) {
      FailJob(job, "还没有可继续播放的音乐，请先点歌。");
      return;
    }
    PausePcmStream(false);
    if (!common::MpvOk(common::MpvCommand({"set_property", "pause", false},
                                          settings_.mpv_socket)))
      FailJob(job, "音乐暂时无法继续播放。");
    else
      SetPlaybackStatus(State::Playing, {}, job.request_id);
  } else if (job.action == Action::Pause) {
    SetPlaybackStatus(State::Paused, {}, job.request_id);
  } else if (job.action == Action::Stop) {
    StopPcmDecoder();
    SetPlaybackStatus(State::Stopped, {}, job.request_id);
  } else if (job.action == Action::Next || job.action == Action::Previous) {
    if (!WaitForSpeechToFinish(job.revision))
      return;
    bool needs_alternatives = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      needs_alternatives = playlist_.size() == 1;
    }
    if (needs_alternatives) {
      // A specific-song search can contain only covers of that song. Broaden
      // the search instead of treating another version as the next song.
      auto search =
          RunCliCommand({"search", "song", "--keyword",
                         settings_.default_keyword, "--output", "json"},
                        job.revision);
      if (!IsJobCurrent(job.revision))
        return;
      if (search.ok && search.data.is_object() &&
          search.data.value("code", 0) == 200 && search.data.contains("data") &&
          search.data["data"].is_object() &&
          search.data["data"].contains("records") &&
          search.data["data"]["records"].is_array()) {
        AppendPlaylist(search.data["data"]["records"]);
      } else {
        LOG_WARN("Music alternatives search failed: exit_code={} timed_out={}",
                 search.exit_code, search.timed_out);
      }
    }
    QueueTrack track;
    std::size_t index = 0;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (playlist_.size() > 1) {
        index =
            job.action == Action::Next
                ? (playlist_index_ + 1) % playlist_.size()
                : (playlist_index_ + playlist_.size() - 1) % playlist_.size();
        track = playlist_[index];
        found = true;
      }
    }
    if (!found) {
      FailJob(job, "没有找到其他可播放的歌曲，请点一首新的歌。");
      return;
    }
    PlayTrack(job, track, index);
  }
}

void MusicController::AppendPlaylist(const json &songs) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  for (const auto &song : songs) {
    if (playlist_.size() >= 6)
      break;
    if (!IsPlayableSong(song))
      continue;
    auto track = TrackFromSong(song);
    const bool duplicate = std::any_of(
        playlist_.begin(), playlist_.end(), [&](const QueueTrack &queued) {
          return queued.track.song_id == track.song_id ||
                 SameSongTitle(queued.track.title, track.title);
        });
    if (!duplicate)
      playlist_.push_back(
          {std::move(track), OriginalSongId(song), SongDurationSeconds(song)});
  }
}

void MusicController::PlayTrack(const Job &job, const QueueTrack &track,
                                std::size_t index) {
  if (!WaitForSpeechToFinish(job.revision))
    return;
  const auto previous_path =
      audio_output_callback_
          ? json{}
          : common::MpvCommand({"get_property", "path"}, settings_.mpv_socket);
  const bool switching = job.action == common::MusicAction::Next ||
                         job.action == common::MusicAction::Previous;
  // Each CLI play owns one track. The robot owns song order and metadata.
  if (!audio_output_callback_) {
    common::MpvCommand({"set_property", "loop-file", "no"},
                       settings_.mpv_socket);
    common::MpvCommand({"set_property", "loop-playlist", "no"},
                       settings_.mpv_socket);
    common::MpvCommand({"stop"}, settings_.mpv_socket);
  } else {
    StopPcmDecoder(true);
  }
  const std::vector<std::string> play_args{
      "play",          "--song",          "--encrypted-id", track.track.song_id,
      "--original-id", track.original_id, "--output",       "json"};
  auto play = RunCliCommand(play_args, job.revision);
  if (!IsJobCurrent(job.revision)) {
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    return;
  }
  if (!EnsureBackendSubscription(job.revision)) {
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    FailJob(job, "网易云播放器后台没有保持连接，请稍后再试。");
    return;
  }

  auto wait_for_track = [&](double seconds) {
    const double until = Monotonic() + seconds;
    while (IsJobCurrent(job.revision) && Monotonic() < until) {
      if (audio_output_callback_) {
        {
          std::lock_guard<std::mutex> lock(pcm_mutex_);
          if (pcm_position_ > 0)
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      auto idle = common::MpvCommand({"get_property", "idle-active"},
                                     settings_.mpv_socket);
      if (common::MpvOk(idle) && !idle.value("data", true)) {
        const auto path =
            common::MpvCommand({"get_property", "path"}, settings_.mpv_socket);
        if (switching && common::MpvOk(previous_path) &&
            (!common::MpvOk(path) || path["data"] == previous_path["data"])) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        const auto audio = common::MpvCommand(
            {"get_property", "audio-out-params"}, settings_.mpv_socket);
        if (common::MpvOk(audio) && audio.contains("data") &&
            audio["data"].is_object())
          return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  };
  // The CLI starts a detached controller before song lookup and MPV startup
  // complete. Keep its daemon subscribed and allow the compatibility
  // adapter's 20-second request budget before considering a retry.
  // Audio output initialization, not the detached CLI's exit code, confirms
  // that the track loaded.
  bool loaded = wait_for_track(25.0);
  if (!loaded && IsJobCurrent(job.revision)) {
    LOG_WARN("ncm-cli did not load the track on first attempt; retrying once");
    LOG_WARN("First ncm-cli play: exit_code={} timed_out={}", play.exit_code,
             play.timed_out);
    StopPcmDecoder(true);
    play = RunCliCommand(play_args, job.revision);
    if (!IsJobCurrent(job.revision)) {
      common::MpvCommand({"stop"}, settings_.mpv_socket);
      return;
    }
    loaded = wait_for_track(25.0);
  }
  if (!loaded) {
    if (!play.ok ||
        (play.data.is_object() && !play.data.value("success", false)))
      LOG_WARN("ncm-cli play failed: exit_code={} timed_out={}", play.exit_code,
               play.timed_out);
    common::MpvCommand({"stop"}, settings_.mpv_socket);
    FailJob(job, "网易云后台两次都没有加载出歌曲，请稍后再试。");
    return;
  }
  if (!IsJobCurrent(job.revision))
    return;
  if (!audio_output_callback_ &&
      !common::MpvOk(common::MpvCommand({"set_property", "pause", false},
                                        settings_.mpv_socket))) {
    FailJob(job, "音乐已加载，但无法从扬声器开始播放。");
    return;
  }
  double duration_seconds = track.duration_seconds;
  if (duration_seconds <= 0) {
    const auto duration =
        common::MpvCommand({"get_property", "duration"}, settings_.mpv_socket);
    if (common::MpvOk(duration) && duration["data"].is_number())
      duration_seconds = duration["data"].get<double>();
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.duration_seconds = duration_seconds;
    playlist_index_ = index;
    status_.song_id = track.track.song_id;
    status_.title = track.track.title;
    status_.artist = track.track.artist;
    status_.position_seconds = 0;
  }
  SetPlaybackStatus(State::Playing, {}, job.request_id);
  LOG_INFO("MusicNode playback started: {} - {}; queue_index={}",
           track.track.title, track.track.artist, index);
}

bool MusicController::IsJobCurrent(std::uint64_t revision) const {
  return !stop_ && !silenced_ && revision_.load() == revision;
}

void MusicController::SetPlaybackStatus(State state, const std::string &error,
                                        std::uint64_t request_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  status_.state = state;
  status_.error = error;
  status_.request_id = request_id;
  if (state == State::Stopped || state == State::Searching ||
      state == State::Error) {
    status_.song_id.clear();
    status_.title.clear();
    status_.artist.clear();
    status_.position_seconds = 0;
    status_.duration_seconds = 0;
  }
  status_callback_(status_);
}

bool MusicController::EnsureBackendSubscription(std::uint64_t revision) {
  std::string directory = settings_.mpv_socket;
  const auto slash = directory.find_last_of('/');
  directory = slash == std::string::npos ? "." : directory.substr(0, slash);
  const std::string control_path = directory + "/player-daemon.sock";
  const double deadline = Monotonic() + 25.0;
  while (IsJobCurrent(revision) && Monotonic() < deadline) {
    {
      std::lock_guard<std::mutex> lock(backend_mutex_);
      if (backend_fd_ >= 0)
        return true;
    }
    int subscription = ConnectUnix(control_path);
    if (subscription >= 0) {
      const std::string request = json{{"command", "subscribe"},
                                       {"role", "observer"},
                                       {"controllerType", "cli"}}
                                      .dump() +
                                  "\n";
      std::string snapshot;
      if (SendLine(subscription, request) &&
          ReceiveLine(subscription, snapshot, 1200)) {
        const auto event = Parse(snapshot);
        if (event.is_object() &&
            event.value("event", std::string{}) == "stateSnapshot") {
          const int flags = ::fcntl(subscription, F_GETFL, 0);
          if (flags >= 0)
            ::fcntl(subscription, F_SETFL, flags | O_NONBLOCK);
          std::lock_guard<std::mutex> lock(backend_mutex_);
          if (!IsJobCurrent(revision)) {
            ::close(subscription);
            return false;
          }
          if (backend_fd_ >= 0) {
            ::close(subscription);
            return true;
          }
          backend_fd_ = subscription;
          LOG_INFO("Subscribed to ncm-cli player daemon state");
          return true;
        }
      }
      ::close(subscription);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void MusicController::MaintainBackendSubscription() {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ < 0)
    return;
  char buffer[4096];
  for (;;) {
    const auto count =
        ::recv(backend_fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0)
      continue;
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    ::close(backend_fd_);
    backend_fd_ = -1;
    LOG_WARN("ncm-cli player daemon subscription disconnected");
    return;
  }
}

void MusicController::CloseBackendSubscription() {
  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (backend_fd_ >= 0)
    ::close(backend_fd_);
  backend_fd_ = -1;
}

MusicController::CliResult
MusicController::RunCliCommand(const std::vector<std::string> &arguments,
                               std::uint64_t revision) {
  int output[2];
  if (pipe2(output, O_CLOEXEC) != 0)
    return {};
  std::vector<std::string> args{settings_.cli_path};
  args.insert(args.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  std::vector<std::string> environment;
  for (char **entry = environ; *entry; ++entry) {
    const std::string value(*entry);
    if (value.rfind("NODE_OPTIONS=", 0) != 0 &&
        value.rfind("XIAOCAI_MUSIC_PCM_", 0) != 0)
      environment.push_back(value);
  }
  const char *node_options = std::getenv("NODE_OPTIONS");
  if (!settings_.decoder_adapter_path.empty())
    environment.push_back(
        std::string("NODE_OPTIONS=") +
        (node_options ? std::string(node_options) + " " : "") +
        "--require=" + settings_.decoder_adapter_path);
  environment.push_back("XIAOCAI_MUSIC_PCM_FIFO=" + settings_.pcm_fifo);
  environment.push_back("XIAOCAI_MUSIC_PCM_RATE=" +
                        std::to_string(settings_.sample_rate));
  environment.push_back("XIAOCAI_MUSIC_PCM_CHANNELS=" +
                        std::to_string(settings_.channels));
  std::vector<char *> envp;
  for (auto &entry : environment)
    envp.push_back(entry.data());
  envp.push_back(nullptr);
  pid_t pid;
  const int error = posix_spawnp(&pid, argv[0], &actions, &attributes,
                                 argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(output[1]);
  if (error != 0) {
    close(output[0]);
    return {};
  }
  fcntl(output[0], F_SETFL, O_NONBLOCK);
  std::string text;
  const double until = Monotonic() + 30;
  int status = 0;
  bool exited = false, cancelled = false;
  while (!exited) {
    char buffer[4096];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
      if (text.size() < 2 * 1024 * 1024)
        text.append(buffer, count);
    exited = waitpid(pid, &status, WNOHANG) == pid;
    if (!exited && (!IsJobCurrent(revision) || Monotonic() > until)) {
      cancelled = true;
      kill(-pid, SIGTERM);
      const double grace = Monotonic() + 0.5;
      while (waitpid(pid, &status, WNOHANG) == 0 && Monotonic() < grace)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      kill(-pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      exited = true;
    }
    if (!exited)
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  char buffer[4096];
  ssize_t count;
  while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
    if (text.size() < 2 * 1024 * 1024)
      text.append(buffer, count);
  close(output[0]);
  json data = json::value_t::discarded;
  const auto begin = text.find('{');
  const auto end = text.rfind('}');
  if (begin != std::string::npos && end != std::string::npos && end >= begin)
    data = Parse(text.substr(begin, end - begin + 1));
  const int exit_code = WIFEXITED(status)     ? WEXITSTATUS(status)
                        : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                              : -1;
  return {!cancelled && exit_code == 0, data, text, exit_code,
          cancelled && IsJobCurrent(revision)};
}

bool MusicController::WaitForSpeechToFinish(std::uint64_t revision) {
  const double until = Monotonic() + 20;
  while (IsJobCurrent(revision) && Monotonic() < until) {
    if (!voice_playing_ && Monotonic() - last_voice_at_.load() > 0.5)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void MusicController::FailJob(const Job &job, const std::string &message) {
  if (!IsJobCurrent(job.revision))
    return;
  StopPcmDecoder();
  SetPlaybackStatus(State::Error, message, job.request_id);
  if (job.request_id == 0)
    speech_callback_(message);
  LOG_WARN("{}", message);
}

} // namespace xiaocai_robot
