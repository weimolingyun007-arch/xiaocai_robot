#include "display_node/display_window.hpp"
#include "qrcodegen-1.8.0/qrcodegen.hpp"

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        music_mode(false), login_required(false), login_loading(false) {}
  ~State();

  int TextWidth(const std::string &text, std::size_t font) const;
  std::string FitText(std::string text, std::size_t font, int width) const;
  void DrawText(const std::string &text, std::size_t font, std::size_t color,
                int x, int baseline, bool centered = false);
  void DrawWifiStatus();
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
  WifiStatus wifi_status;
  bool login_required;
  bool login_loading;
  std::string login_url;
  std::string login_error;
  std::optional<qrcodegen::QrCode> login_qr;
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

void DisplayWindow::State::DrawWifiStatus() {
  const int screen = DefaultScreen(display);
  XSetForeground(display, graphics_context, BlackPixel(display, screen));
  XFillRectangle(display, buffer, graphics_context, 734, 7, 44, 33);
  XSetLineAttributes(display, graphics_context, 2, LineSolid, CapRound,
                     JoinRound);
  for (int ring = 1; ring <= 3; ++ring) {
    const bool lit = wifi_status.connected && wifi_status.signal_known &&
                     wifi_status.strength > ring;
    XSetForeground(display, graphics_context, colors[lit ? 3 : 1].pixel);
    const int radius = 5 + ring * 5;
    XDrawArc(display, buffer, graphics_context, 756 - radius, 32 - radius,
             radius * 2, radius * 2, 45 * 64, 90 * 64);
  }
  XSetForeground(display, graphics_context,
                 colors[wifi_status.connected ? 3 : 1].pixel);
  XFillArc(display, buffer, graphics_context, 753, 29, 6, 6, 0, 360 * 64);
  if (!wifi_status.connected) {
    XSetForeground(display, graphics_context, colors[2].pixel);
    XDrawLine(display, buffer, graphics_context, 742, 12, 770, 37);
  }
  XSetLineAttributes(display, graphics_context, 1, LineSolid, CapButt,
                     JoinMiter);
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
  DrawWifiStatus();
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
               ExposureMask | StructureNotifyMask);
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
  state_->music_mode = false;
  state_->music_signature.clear();
  XPutImage(state_->display, state_->buffer, state_->graphics_context,
            found->second, 0, 0, 0, 0, 800, 480);
  state_->Present();
}

void DisplayWindow::ShowMusic(const MusicView &view) {
  const int progress =
      view.duration_seconds > 0
          ? static_cast<int>(
                736 * std::clamp(view.position_seconds / view.duration_seconds,
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
  state_->music_signature = signature;
  XSetForeground(state_->display, state_->graphics_context,
                 BlackPixel(state_->display, DefaultScreen(state_->display)));
  XFillRectangle(state_->display, state_->buffer, state_->graphics_context, 0,
                 0, 800, 480);
  state_->DrawText(state_->FitText(view.title, 2, 736), 2, 0, 32, 58);
  state_->DrawText(state_->FitText(view.artist, 0, 736), 0, 2, 32, 84);
  const bool wrap = state_->TextWidth(view.current_line, 3) > 736;
  state_->DrawText(state_->FitText(view.previous_line, 1, 736), 1, 1, 400,
                   wrap ? 166 : 190, true);
  if (wrap) {
    std::size_t split = 0;
    while (split < view.current_line.size()) {
      auto next = split + 1;
      while (next < view.current_line.size() &&
             (static_cast<unsigned char>(view.current_line[next]) & 0xc0) ==
                 0x80)
        ++next;
      if (state_->TextWidth(view.current_line.substr(0, next), 3) > 736)
        break;
      split = next;
    }
    state_->DrawText(view.current_line.substr(0, split), 3, 3, 400, 245, true);
    state_->DrawText(state_->FitText(view.current_line.substr(split), 3, 736),
                     3, 3, 400, 290, true);
  } else {
    const bool placeholder =
        view.current_line == "暂无歌词" || view.current_line == "歌词加载中…";
    state_->DrawText(view.current_line, 3, placeholder ? 2 : 3, 400, 263, true);
  }
  state_->DrawText(state_->FitText(view.next_line, 1, 736), 1, 1, 400,
                   wrap ? 346 : 327, true);
  XSetForeground(state_->display, state_->graphics_context,
                 state_->colors[4].pixel);
  XFillRectangle(state_->display, state_->buffer, state_->graphics_context, 32,
                 418, 736, 2);
  XSetForeground(state_->display, state_->graphics_context,
                 state_->colors[3].pixel);
  XFillRectangle(state_->display, state_->buffer, state_->graphics_context, 32,
                 418, progress, 2);
  state_->DrawText(elapsed, 0, 2, 32, 446);
  state_->DrawText(view.paused ? "已暂停" : "播放中", 0, 3, 400, 446, true);
  state_->DrawText(duration, 0, 2, 768 - state_->TextWidth(duration, 0), 446);
  state_->Present();
}

void DisplayWindow::ShowWifiStatus(const WifiStatus &status) {
  const auto &old = state_->wifi_status;
  if (old.available == status.available && old.link_up == status.link_up &&
      old.connected == status.connected &&
      old.signal_known == status.signal_known &&
      old.strength == status.strength)
    return;
  state_->wifi_status = status;
  state_->Present();
}

void DisplayWindow::ShowLogin(bool required, bool loading,
                              const std::string &url,
                              const std::string &error) {
  if (state_->login_required == required && state_->login_loading == loading &&
      state_->login_url == url && state_->login_error == error)
    return;
  state_->login_required = required;
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
  }
}

} // namespace xiaocai_robot
