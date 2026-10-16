#include "display_node/display_window.hpp"
#include "defination/ble_status.hpp"
#include "qrcodegen-1.8.0/qrcodegen.hpp"

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace xiaocai_robot {
namespace {

constexpr int kMusicWidth = 736;
constexpr int kMusicCenter = 400;
constexpr int kButtonY = 410;
constexpr int kButtonWidth = 184;
constexpr int kButtonHeight = 54;
constexpr std::array<int, 3> kButtonX{80, 308, 536};
constexpr int kButtonIconOffset = 45;
constexpr int kButtonLabelOffset = 122;
constexpr int kVolumePanelLeft = 736;
constexpr int kVolumeX = 760;
constexpr int kVolumeTop = 194;
constexpr int kVolumeBottom = 344;
constexpr int kVolumeGestureTop = 120;
constexpr int kVolumeGestureBottom = 405;
constexpr double kVolumeVisibleSeconds = 1.4;

double SteadySeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string LocalClockText() {
  const std::time_t now = std::time(nullptr);
  std::tm local_time{};
  if (localtime_r(&now, &local_time) == nullptr)
    return "--:--";
  char text[32]{};
  if (std::strftime(text, sizeof(text), "%Y-%m-%d  %H:%M", &local_time) == 0)
    return "--:--";
  return text;
}

std::string FormatRate(double bytes_per_second) {
  char text[32]{};
  if (bytes_per_second < 1024.0) {
    std::snprintf(text, sizeof(text), "%.0fB/s", bytes_per_second);
  } else if (bytes_per_second < 1024.0 * 1024.0) {
    std::snprintf(text, sizeof(text), "%.1fK/s", bytes_per_second / 1024.0);
  } else {
    std::snprintf(text, sizeof(text), "%.1fM/s",
                  bytes_per_second / (1024.0 * 1024.0));
  }
  return text;
}

struct RgbImage {
  unsigned int width{};
  unsigned int height{};
  std::vector<std::uint8_t> pixels;
};

std::string ReadPpmToken(std::istream &input) {
  std::string token;
  while (input >> std::ws && input.peek() == '#')
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  if (!(input >> token))
    throw std::runtime_error("Unexpected end of PPM header");
  return token;
}

RgbImage LoadPpm(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot open image: " + path.string());

  if (ReadPpmToken(input) != "P6")
    throw std::runtime_error("Expected a binary PPM image: " + path.string());

  RgbImage image;
  image.width = static_cast<unsigned int>(std::stoul(ReadPpmToken(input)));
  image.height = static_cast<unsigned int>(std::stoul(ReadPpmToken(input)));
  if (ReadPpmToken(input) != "255" || image.width != 800 || image.height != 480)
    throw std::runtime_error("Unexpected PPM dimensions or color range: " +
                             path.string());

  char separator = 0;
  input.get(separator);
  if (separator == '\r' && input.peek() == '\n')
    input.get(separator);
  if (!std::isspace(static_cast<unsigned char>(separator)))
    throw std::runtime_error("Invalid PPM header separator: " + path.string());

  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 3);
  input.read(reinterpret_cast<char *>(image.pixels.data()),
             static_cast<std::streamsize>(image.pixels.size()));
  if (input.gcount() != static_cast<std::streamsize>(image.pixels.size()))
    throw std::runtime_error("Incomplete PPM pixel data: " + path.string());
  return image;
}

unsigned long ScaleChannel(std::uint8_t value, unsigned long mask) {
  if (mask == 0)
    return 0;
  const unsigned int shift = static_cast<unsigned int>(__builtin_ctzl(mask));
  const unsigned long maximum = mask >> shift;
  const unsigned long scaled =
      (static_cast<unsigned long>(value) * maximum + 127UL) / 255UL;
  return (scaled << shift) & mask;
}

XImage *CreateXImage(Display *display, Visual *visual, int depth,
                     const RgbImage &source) {
  XImage *image =
      XCreateImage(display, visual, static_cast<unsigned int>(depth), ZPixmap,
                   0, nullptr, source.width, source.height, 32, 0);
  if (!image)
    throw std::runtime_error("XCreateImage failed");

  image->red_mask = visual->red_mask;
  image->green_mask = visual->green_mask;
  image->blue_mask = visual->blue_mask;
  image->data = static_cast<char *>(std::calloc(
      static_cast<std::size_t>(image->bytes_per_line), source.height));
  if (!image->data) {
    XDestroyImage(image);
    throw std::bad_alloc();
  }

  for (unsigned int y = 0; y < source.height; ++y) {
    for (unsigned int x = 0; x < source.width; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * source.width + x) * 3;
      const unsigned long pixel =
          ScaleChannel(source.pixels[offset], image->red_mask) |
          ScaleChannel(source.pixels[offset + 1], image->green_mask) |
          ScaleChannel(source.pixels[offset + 2], image->blue_mask);
      XPutPixel(image, static_cast<int>(x), static_cast<int>(y), pixel);
    }
  }
  return image;
}

} // namespace

struct DisplayWindow::State {
  State()
      : display(nullptr), window(0), graphics_context(nullptr), buffer(0),
        text_draw(nullptr), fonts{}, colors{}, color_count(0),
        music_mode(false), login_required(false), login_loading(false),
        clock_text(LocalClockText()) {}
  ~State();

  int TextWidth(const std::string &text, std::size_t font) const;
  std::string FitText(std::string text, std::size_t font, int width) const;
  void DrawText(const std::string &text, std::size_t font, std::size_t color,
                int x, int baseline, bool centered = false);
  void DrawStatusBar();
  void DrawMusicControls();
  void DrawVolumeOverlay();
  int MusicButtonAt(int x, int y) const;
  void DragVolume(int y, bool final);
  void UpdateVolumeVisibility();
  bool UpdateClock();
  void DrawLogin();
  void Present();

  Display *display;
  Window window;
  GC graphics_context;
  Pixmap buffer;
  XftDraw *text_draw;
  std::array<XftFont *, 4> fonts;
  std::array<XftColor, 5> colors;
  std::size_t color_count;
  std::unordered_map<std::string, XImage *> images;
  std::string active_image;
  bool music_mode;
  std::string music_signature;
  xiaocai_msgs::msg::WifiStatus wifi_status;
  xiaocai_msgs::msg::BleStatus ble_status;
  bool login_required;
  bool login_loading;
  std::string login_url;
  std::string login_error;
  std::optional<qrcodegen::QrCode> login_qr;
  std::string clock_text;
  std::function<void(common::MusicAction)> music_control_callback;
  std::function<void(int)> volume_control_callback;
  int pressed_button{-1};
  bool music_paused{false};
  int volume_percent{80};
  int last_sent_volume{-1};
  bool volume_known{false};
  bool dragging_volume{false};
  bool volume_visible{false};
  double volume_visible_until{0.0};
  double last_button_at{-1.0};
  double last_volume_at{-1.0};
};

DisplayWindow::State::~State() {
  for (auto &[name, image] : images) {
    (void)name;
    if (image)
      XDestroyImage(image);
  }
  if (display) {
    if (text_draw)
      XftDrawDestroy(text_draw);
    for (auto *font : fonts)
      if (font)
        XftFontClose(display, font);
    const int screen = DefaultScreen(display);
    for (std::size_t index = 0; index < color_count; ++index)
      XftColorFree(display, DefaultVisual(display, screen),
                   DefaultColormap(display, screen), &colors[index]);
    if (buffer)
      XFreePixmap(display, buffer);
    if (graphics_context)
      XFreeGC(display, graphics_context);
    if (window)
      XDestroyWindow(display, window);
    XCloseDisplay(display);
  }
}

int DisplayWindow::State::TextWidth(const std::string &text,
                                    std::size_t font) const {
  XGlyphInfo extent{};
  XftTextExtentsUtf8(display, fonts[font],
                     reinterpret_cast<const FcChar8 *>(text.data()),
                     static_cast<int>(text.size()), &extent);
  return extent.xOff;
}

std::string DisplayWindow::State::FitText(std::string text, std::size_t font,
                                          int width) const {
  if (TextWidth(text, font) <= width)
    return text;
  while (!text.empty()) {
    auto end = text.size() - 1;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
      --end;
    text.resize(end);
    if (TextWidth(text + "…", font) <= width)
      break;
  }
  return text + "…";
}

void DisplayWindow::State::DrawText(const std::string &text, std::size_t font,
                                    std::size_t color, int x, int baseline,
                                    bool centered) {
  if (centered)
    x -= TextWidth(text, font) / 2;
  XftDrawStringUtf8(text_draw, &colors[color], fonts[font], x, baseline,
                    reinterpret_cast<const FcChar8 *>(text.data()),
                    static_cast<int>(text.size()));
}

void DisplayWindow::State::DrawStatusBar() {
  const int screen = DefaultScreen(display);
  XSetForeground(display, graphics_context, BlackPixel(display, screen));
  XFillRectangle(display, buffer, graphics_context, 0, 0, 800, 40);
  DrawText(clock_text, 0, 0, 20, 27);

  const std::string speed_text =
      wifi_status.speed_known
          ? "\xE2\x86\x93" + FormatRate(wifi_status.download_bytes_per_second) +
                " \xE2\x86\x91" +
                FormatRate(wifi_status.upload_bytes_per_second)
          : "\xE2\x86\x93-- \xE2\x86\x91--";
  const int speed_x = 712 - TextWidth(speed_text, 0);
  DrawText(speed_text, 0, 2, speed_x, 27);

  XSetLineAttributes(display, graphics_context, 2, LineSolid, CapRound,
                     JoinRound);
  for (int ring = 1; ring <= 3; ++ring) {
    const bool lit = wifi_status.connected && wifi_status.signal_known &&
                     wifi_status.strength > ring;
    XSetForeground(display, graphics_context, colors[lit ? 3 : 1].pixel);
    const int radius = 5 + ring * 5;
    XDrawArc(display, buffer, graphics_context, 744 - radius, 32 - radius,
             radius * 2, radius * 2, 45 * 64, 90 * 64);
  }
  XSetForeground(display, graphics_context,
                 colors[wifi_status.connected ? 3 : 1].pixel);
  XFillArc(display, buffer, graphics_context, 741, 29, 6, 6, 0, 360 * 64);
  if (!wifi_status.connected) {
    XSetForeground(display, graphics_context, colors[2].pixel);
    XDrawLine(display, buffer, graphics_context, 730, 12, 758, 37);
  }

  const bool bluetooth_connected =
      ble_status.state ==
      static_cast<std::uint8_t>(common::BleStatusState::Connected);
  const auto bluetooth_color = colors[bluetooth_connected ? 3 : 1].pixel;
  XSetForeground(display, graphics_context, bluetooth_color);
  XDrawLine(display, buffer, graphics_context, 780, 8, 780, 35);
  XDrawLine(display, buffer, graphics_context, 780, 8, 789, 15);
  XDrawLine(display, buffer, graphics_context, 789, 15, 780, 22);
  XDrawLine(display, buffer, graphics_context, 780, 22, 789, 29);
  XDrawLine(display, buffer, graphics_context, 789, 29, 780, 35);
  XDrawLine(display, buffer, graphics_context, 773, 14, 787, 28);
  XDrawLine(display, buffer, graphics_context, 773, 28, 787, 14);
  if (bluetooth_connected)
    XFillArc(display, buffer, graphics_context, 791, 32, 4, 4, 0, 360 * 64);
  XSetLineAttributes(display, graphics_context, 1, LineSolid, CapButt,
                     JoinMiter);
}

int DisplayWindow::State::MusicButtonAt(int x, int y) const {
  if (!music_mode || login_required || y < kButtonY ||
      y >= kButtonY + kButtonHeight)
    return -1;
  for (std::size_t i = 0; i < kButtonX.size(); ++i)
    if (x >= kButtonX[i] && x < kButtonX[i] + kButtonWidth)
      return static_cast<int>(i);
  return -1;
}

void DisplayWindow::State::DrawMusicControls() {
  for (std::size_t i = 0; i < kButtonX.size(); ++i) {
    const int x = kButtonX[i];
    const bool pressed = pressed_button == static_cast<int>(i);
    XSetForeground(display, graphics_context, colors[pressed ? 3 : 4].pixel);
    XFillRectangle(display, buffer, graphics_context, x, kButtonY, kButtonWidth,
                   kButtonHeight);
    XSetForeground(display, graphics_context, colors[3].pixel);
    XDrawRectangle(display, buffer, graphics_context, x, kButtonY,
                   kButtonWidth - 1, kButtonHeight - 1);
    const auto foreground = colors[pressed ? 4 : 0].pixel;
    XSetForeground(display, graphics_context, foreground);
    const int center_y = kButtonY + kButtonHeight / 2;
    const int center_x = x + kButtonIconOffset;
    if (i == 1 && !music_paused) {
      XFillRectangle(display, buffer, graphics_context, center_x - 8,
                     center_y - 10, 5, 20);
      XFillRectangle(display, buffer, graphics_context, center_x + 3,
                     center_y - 10, 5, 20);
    } else {
      const int direction = i == 0 ? -1 : 1;
      XPoint triangle[]{{static_cast<short>(center_x + direction * 8),
                         static_cast<short>(center_y)},
                        {static_cast<short>(center_x - direction * 7),
                         static_cast<short>(center_y - 10)},
                        {static_cast<short>(center_x - direction * 7),
                         static_cast<short>(center_y + 10)}};
      XFillPolygon(display, buffer, graphics_context, triangle, 3, Convex,
                   CoordModeOrigin);
      if (i != 1)
        XFillRectangle(display, buffer, graphics_context,
                       center_x + direction * 12 - 1, center_y - 10, 3, 20);
    }
    const auto *label = i == 0         ? "上一首"
                        : i == 2       ? "下一首"
                        : music_paused ? "播放"
                                       : "暂停";
    DrawText(label, 1, pressed ? 4 : 0, x + kButtonLabelOffset, kButtonY + 35,
             true);
  }
  if (volume_visible)
    DrawVolumeOverlay();
}

void DisplayWindow::State::DrawVolumeOverlay() {
  constexpr int panel_left = 724;
  constexpr int panel_top = 128;
  constexpr unsigned int panel_width = 72;
  constexpr unsigned int panel_height = 296;
  XSetForeground(display, graphics_context, colors[4].pixel);
  XFillRectangle(display, buffer, graphics_context, panel_left, panel_top,
                 panel_width, panel_height);
  XSetForeground(display, graphics_context, colors[2].pixel);
  XDrawRectangle(display, buffer, graphics_context, panel_left, panel_top,
                 panel_width - 1, panel_height - 1);
  DrawText("音量", 0, 0, kVolumeX, 154, true);
  DrawText("150", 0, 2, kVolumeX, 181, true);
  XSetForeground(display, graphics_context, colors[2].pixel);
  XFillRectangle(display, buffer, graphics_context, kVolumeX - 3, kVolumeTop, 6,
                 kVolumeBottom - kVolumeTop);
  XSetForeground(display, graphics_context, colors[3].pixel);
  const int thumb = kVolumeBottom -
                    static_cast<int>(std::lround((kVolumeBottom - kVolumeTop) *
                                                 volume_percent / 150.0));
  XFillRectangle(display, buffer, graphics_context, kVolumeX - 3, thumb, 6,
                 kVolumeBottom - thumb);
  XFillArc(display, buffer, graphics_context, kVolumeX - 9, thumb - 9, 18, 18,
           0, 360 * 64);
  DrawText("0", 0, 2, kVolumeX, 372, true);
  DrawText(volume_known ? std::to_string(volume_percent) + "%" : "--%", 0, 3,
           kVolumeX, 402, true);
}

void DisplayWindow::State::DragVolume(int y, bool final) {
  volume_visible = true;
  volume_visible_until = SteadySeconds() + kVolumeVisibleSeconds;
  volume_percent = static_cast<int>(
      std::lround((kVolumeBottom - std::clamp(y, kVolumeTop, kVolumeBottom)) *
                  150.0 / (kVolumeBottom - kVolumeTop)));
  volume_known = true;
  const auto now = SteadySeconds();
  if (volume_control_callback && volume_percent != last_sent_volume &&
      (final || now - last_volume_at >= 0.08)) {
    last_volume_at = now;
    last_sent_volume = volume_percent;
    volume_control_callback(volume_percent);
  }
  DrawMusicControls();
  Present();
}

void DisplayWindow::State::UpdateVolumeVisibility() {
  if (!volume_visible || dragging_volume ||
      SteadySeconds() < volume_visible_until)
    return;
  volume_visible = false;
  DrawMusicControls();
  Present();
}

bool DisplayWindow::State::UpdateClock() {
  const auto updated = LocalClockText();
  if (updated == clock_text)
    return false;
  clock_text = updated;
  return true;
}

void DisplayWindow::State::DrawLogin() {
  if (!login_required)
    return;
  const int screen = DefaultScreen(display);
  XSetForeground(display, graphics_context, BlackPixel(display, screen));
  XFillRectangle(display, buffer, graphics_context, 0, 0, 800, 480);
  DrawText("网易云音乐需要登录", 2, 3, 400, 67, true);
  if (login_qr) {
    constexpr int border = 4;
    const int scale = 288 / (login_qr->getSize() + border * 2);
    const int size = (login_qr->getSize() + border * 2) * scale;
    const int left = (800 - size) / 2;
    const int top = 89 + (288 - size) / 2;
    XSetForeground(display, graphics_context, WhitePixel(display, screen));
    XFillRectangle(display, buffer, graphics_context, left, top, size, size);
    XSetForeground(display, graphics_context, BlackPixel(display, screen));
    for (int y = 0; y < login_qr->getSize(); ++y)
      for (int x = 0; x < login_qr->getSize(); ++x)
        if (login_qr->getModule(x, y))
          XFillRectangle(display, buffer, graphics_context,
                         left + (x + border) * scale,
                         top + (y + border) * scale, scale, scale);
    DrawText("请使用网易云音乐 App 扫码登录", 1, 0, 400, 418, true);
    DrawText("在手机上确认授权后，此页面自动关闭", 0, 2, 400, 451, true);
  } else {
    DrawText(login_loading ? "正在获取登录二维码…" : "暂时无法获取二维码", 2, 0,
             400, 240, true);
    DrawText(login_loading ? "请稍候" : "请检查网络，程序会自动重试", 1, 2, 400,
             285, true);
  }
}

void DisplayWindow::State::Present() {
  DrawLogin();
  DrawStatusBar();
  XCopyArea(display, buffer, window, graphics_context, 0, 0, 800, 480, 0, 0);
  XFlush(display);
}

DisplayWindow::DisplayWindow(std::filesystem::path asset_directory,
                             const std::string &font_family)
    : state_(std::make_unique<State>()) {
  state_->display = XOpenDisplay(nullptr);
  if (!state_->display)
    throw std::runtime_error("Cannot connect to the X11 display");
  const int screen = DefaultScreen(state_->display);
  const int width = DisplayWidth(state_->display, screen);
  const int height = DisplayHeight(state_->display, screen);
  if (width != 800 || height != 480)
    throw std::runtime_error("Expected landscape X11 output 800x480, got " +
                             std::to_string(width) + "x" +
                             std::to_string(height));
  const auto visual = DefaultVisual(state_->display, screen);
  const auto colormap = DefaultColormap(state_->display, screen);
  const auto depth = DefaultDepth(state_->display, screen);
  state_->window = XCreateSimpleWindow(
      state_->display, RootWindow(state_->display, screen), 0, 0, width, height,
      0, BlackPixel(state_->display, screen),
      BlackPixel(state_->display, screen));
  XSetWindowAttributes attributes{};
  attributes.override_redirect = True;
  XChangeWindowAttributes(state_->display, state_->window, CWOverrideRedirect,
                          &attributes);
  XSelectInput(state_->display, state_->window,
               ExposureMask | StructureNotifyMask | ButtonPressMask |
                   ButtonReleaseMask | PointerMotionMask);
  XStoreName(state_->display, state_->window, "Xiaocai Display");
  state_->graphics_context =
      XCreateGC(state_->display, state_->window, 0, nullptr);
  state_->buffer =
      XCreatePixmap(state_->display, state_->window, width, height, depth);
  state_->text_draw =
      XftDrawCreate(state_->display, state_->buffer, visual, colormap);
  if (!state_->graphics_context || !state_->buffer || !state_->text_draw)
    throw std::runtime_error("Cannot create display drawing resources");
  const std::array<int, 4> sizes{14, 24, 26, 38};
  for (std::size_t index = 0; index < sizes.size(); ++index) {
    const std::string name =
        font_family + ":pixelsize=" + std::to_string(sizes[index]);
    state_->fonts[index] =
        XftFontOpenName(state_->display, screen, name.c_str());
    if (!state_->fonts[index])
      throw std::runtime_error("Cannot load display font: " + font_family);
  }
  const std::array<XRenderColor, 5> colors{{{0xe3e3, 0xebeb, 0xeeee, 0xffff},
                                            {0x7474, 0x8585, 0x8a8a, 0xffff},
                                            {0x9e9e, 0xacac, 0xb0b0, 0xffff},
                                            {0x2c2c, 0xe6e6, 0xeded, 0xffff},
                                            {0x1616, 0x2828, 0x2b2b, 0xffff}}};
  for (const auto &color : colors) {
    if (!XftColorAllocValue(state_->display, visual, colormap, &color,
                            &state_->colors[state_->color_count]))
      throw std::runtime_error("Cannot allocate display text colors");
    ++state_->color_count;
  }
  const std::array<std::string, 6> names{"idle",     "listening", "thinking",
                                         "speaking", "quiet",     "muted"};
  for (const auto &name : names) {
    const auto source = LoadPpm(asset_directory / (name + ".ppm"));
    state_->images.emplace(
        name, CreateXImage(state_->display, visual, depth, source));
  }
  XMapRaised(state_->display, state_->window);
  XFlush(state_->display);
}

DisplayWindow::~DisplayWindow() = default;

void DisplayWindow::ShowImage(const std::string &image_name) {
  if (!state_->music_mode && state_->active_image == image_name)
    return;
  const auto found = state_->images.find(image_name);
  if (found == state_->images.end())
    throw std::runtime_error("Unknown display expression: " + image_name);
  state_->active_image = image_name;
  if (state_->dragging_volume)
    XUngrabPointer(state_->display, CurrentTime);
  state_->dragging_volume = false;
  state_->volume_visible = false;
  state_->pressed_button = -1;
  state_->music_mode = false;
  state_->music_signature.clear();
  XPutImage(state_->display, state_->buffer, state_->graphics_context,
            found->second, 0, 0, 0, 0, 800, 480);
  state_->Present();
}

void DisplayWindow::ShowMusic(const MusicView &view) {
  const int progress =
      view.duration_seconds > 0
          ? static_cast<int>(kMusicWidth * std::clamp(view.position_seconds /
                                                          view.duration_seconds,
                                                      0.0, 1.0))
          : 0;
  auto format_time = [](double value) {
    const int seconds = static_cast<int>(std::clamp(value, 0.0, 359999.0));
    char text[32];
    std::snprintf(text, sizeof(text), "%02d:%02d", seconds / 60, seconds % 60);
    return std::string(text);
  };
  const auto elapsed = format_time(view.position_seconds);
  const auto duration = format_time(view.duration_seconds);
  const std::string signature =
      view.title + '\n' + view.artist + '\n' + view.previous_line + '\n' +
      view.current_line + '\n' + view.next_line + '\n' + elapsed + duration +
      std::to_string(progress) + (view.paused ? "paused" : "playing");
  if (state_->music_mode && state_->music_signature == signature)
    return;
  state_->music_mode = true;
  state_->music_paused = view.paused;
  state_->music_signature = signature;
  XSetForeground(state_->display, state_->graphics_context,
                 BlackPixel(state_->display, DefaultScreen(state_->display)));
  XFillRectangle(state_->display, state_->buffer, state_->graphics_context, 0,
                 0, 800, 480);
  state_->DrawText(state_->FitText(view.title, 2, kMusicWidth), 2, 0, 32, 72);
  state_->DrawText(state_->FitText(view.artist, 0, kMusicWidth), 0, 2, 32, 98);
  const bool wrap = state_->TextWidth(view.current_line, 3) > kMusicWidth;
  state_->DrawText(state_->FitText(view.previous_line, 1, kMusicWidth), 1, 1,
                   kMusicCenter, wrap ? 154 : 164, true);
  if (wrap) {
    std::size_t split = 0;
    while (split < view.current_line.size()) {
      auto next = split + 1;
      while (next < view.current_line.size() &&
             (static_cast<unsigned char>(view.current_line[next]) & 0xc0) ==
                 0x80)
        ++next;
      if (state_->TextWidth(view.current_line.substr(0, next), 3) > kMusicWidth)
        break;
      split = next;
    }
    state_->DrawText(view.current_line.substr(0, split), 3, 3, kMusicCenter,
                     225, true);
    state_->DrawText(
        state_->FitText(view.current_line.substr(split), 3, kMusicWidth), 3, 3,
        kMusicCenter, 270, true);
  } else {
    const bool placeholder =
        view.current_line == "暂无歌词" || view.current_line == "歌词加载中…";
    state_->DrawText(view.current_line, 3, placeholder ? 2 : 3, kMusicCenter,
                     240, true);
  }
  state_->DrawText(state_->FitText(view.next_line, 1, kMusicWidth), 1, 1,
                   kMusicCenter, wrap ? 318 : 312, true);
  XSetForeground(state_->display, state_->graphics_context,
                 state_->colors[4].pixel);
  XFillRectangle(state_->display, state_->buffer, state_->graphics_context, 32,
                 366, kMusicWidth, 2);
  XSetForeground(state_->display, state_->graphics_context,
                 state_->colors[3].pixel);
  XFillRectangle(state_->display, state_->buffer, state_->graphics_context, 32,
                 366, progress, 2);
  state_->DrawText(elapsed, 0, 2, 32, 392);
  state_->DrawText(view.paused ? "已暂停" : "播放中", 0, 3, kMusicCenter, 392,
                   true);
  state_->DrawText(duration, 0, 2, 768 - state_->TextWidth(duration, 0), 392);
  state_->DrawMusicControls();
  state_->Present();
}

bool DisplayWindow::IsVolumeOverlayActive() const {
  return state_->music_mode && state_->volume_visible;
}

void DisplayWindow::ShowWifiStatus(
    const xiaocai_msgs::msg::WifiStatus &status) {
  const auto &old = state_->wifi_status;
  if (old.available == status.available && old.link_up == status.link_up &&
      old.connected == status.connected &&
      old.ipv4_address == status.ipv4_address &&
      old.signal_known == status.signal_known &&
      old.strength == status.strength &&
      old.speed_known == status.speed_known &&
      old.download_bytes_per_second == status.download_bytes_per_second &&
      old.upload_bytes_per_second == status.upload_bytes_per_second &&
      old.auto_reconnect_enabled == status.auto_reconnect_enabled &&
      old.reconnecting == status.reconnecting &&
      old.reconnect_state == status.reconnect_state)
    return;
  state_->wifi_status = status;
  state_->Present();
}

void DisplayWindow::ShowBleStatus(const xiaocai_msgs::msg::BleStatus &status) {
  const auto &old = state_->ble_status;
  if (old.state == status.state &&
      old.connected_address == status.connected_address &&
      old.connected_name == status.connected_name)
    return;
  state_->ble_status = status;
  state_->Present();
}

void DisplayWindow::RefreshClock() {
  if (state_->UpdateClock())
    state_->Present();
}

void DisplayWindow::SetMusicControlCallback(
    std::function<void(common::MusicAction)> callback) {
  state_->music_control_callback = std::move(callback);
}

void DisplayWindow::SetVolumeControlCallback(
    std::function<void(int)> callback) {
  state_->volume_control_callback = std::move(callback);
}

void DisplayWindow::ShowVolume(int percent) {
  if (percent < 0 || percent > 150 || state_->dragging_volume ||
      (state_->volume_known && state_->volume_percent == percent))
    return;
  state_->volume_known = true;
  state_->volume_percent = percent;
  if (state_->music_mode && !state_->login_required) {
    state_->DrawMusicControls();
    state_->Present();
  }
}

void DisplayWindow::ShowLogin(bool required, bool loading,
                              const std::string &url,
                              const std::string &error) {
  if (state_->login_required == required && state_->login_loading == loading &&
      state_->login_url == url && state_->login_error == error)
    return;
  state_->login_required = required;
  if (required) {
    if (state_->dragging_volume)
      XUngrabPointer(state_->display, CurrentTime);
    state_->dragging_volume = false;
    state_->volume_visible = false;
    state_->pressed_button = -1;
  }
  state_->login_loading = loading;
  state_->login_error = error;
  if (state_->login_url != url) {
    state_->login_url = url;
    state_->login_qr.reset();
    if (url.rfind("https://163cn.tv/", 0) == 0 && url.size() <= 512)
      state_->login_qr = qrcodegen::QrCode::encodeText(
          url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
  }
  // The login page owns the whole backing buffer. Invalidate the normal page
  // cache so closing it restores the current face or lyrics on the next
  // refresh.
  state_->active_image.clear();
  state_->music_signature.clear();
  state_->Present();
}

void DisplayWindow::ProcessEvents() {
  XEvent event{};
  while (XPending(state_->display) > 0) {
    XNextEvent(state_->display, &event);
    if (event.type == Expose && event.xexpose.count == 0)
      state_->Present();
    if (!state_->music_mode || state_->login_required)
      continue;
    if (event.type == ButtonPress && event.xbutton.button == Button1) {
      const int x = event.xbutton.x;
      const int y = event.xbutton.y;
      if (x >= kVolumePanelLeft && y >= kVolumeGestureTop &&
          y <= kVolumeGestureBottom) {
        state_->dragging_volume = true;
        XGrabPointer(state_->display, state_->window, False,
                     PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                     GrabModeAsync, None, None, CurrentTime);
        state_->DragVolume(y, false);
      } else {
        state_->pressed_button = state_->MusicButtonAt(x, y);
        state_->DrawMusicControls();
        state_->Present();
      }
    } else if (event.type == MotionNotify && state_->dragging_volume) {
      state_->DragVolume(event.xmotion.y, false);
    } else if (event.type == ButtonRelease && event.xbutton.button == Button1) {
      if (state_->dragging_volume) {
        state_->DragVolume(event.xbutton.y, true);
        state_->dragging_volume = false;
        XUngrabPointer(state_->display, CurrentTime);
      } else {
        const auto button = state_->pressed_button;
        state_->pressed_button = -1;
        state_->DrawMusicControls();
        state_->Present();
        const auto now = SteadySeconds();
        if (button >= 0 &&
            button == state_->MusicButtonAt(event.xbutton.x, event.xbutton.y) &&
            now - state_->last_button_at >= 0.6 &&
            state_->music_control_callback) {
          state_->last_button_at = now;
          const auto action = button == 0   ? common::MusicAction::Previous
                              : button == 2 ? common::MusicAction::Next
                              : state_->music_paused
                                  ? common::MusicAction::Resume
                                  : common::MusicAction::Pause;
          state_->music_control_callback(action);
        }
      }
    }
  }
  state_->UpdateVolumeVisibility();
}

} // namespace xiaocai_robot
