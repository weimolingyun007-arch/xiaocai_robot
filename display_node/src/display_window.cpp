#include "display_node/display_window.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
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
  Display *display{};
  Window window{};
  GC graphics_context{};
  std::unordered_map<std::string, XImage *> images;
  std::string active_image;

  ~State() {
    for (auto &[name, image] : images) {
      (void)name;
      if (image)
        XDestroyImage(image);
    }
    if (display) {
      if (graphics_context)
        XFreeGC(display, graphics_context);
      if (window)
        XDestroyWindow(display, window);
      XCloseDisplay(display);
    }
  }
};

DisplayWindow::DisplayWindow(std::filesystem::path asset_directory)
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

  state_->window = XCreateSimpleWindow(
      state_->display, RootWindow(state_->display, screen), 0, 0,
      static_cast<unsigned int>(width), static_cast<unsigned int>(height), 0,
      BlackPixel(state_->display, screen), BlackPixel(state_->display, screen));
  XSetWindowAttributes attributes{};
  attributes.override_redirect = True;
  XChangeWindowAttributes(state_->display, state_->window, CWOverrideRedirect,
                          &attributes);
  XSelectInput(state_->display, state_->window,
               ExposureMask | StructureNotifyMask);
  XStoreName(state_->display, state_->window, "Xiaocai Image Display");
  state_->graphics_context =
      XCreateGC(state_->display, state_->window, 0, nullptr);
  if (!state_->graphics_context)
    throw std::runtime_error("XCreateGC failed");

  const std::array<std::string, 6> names = {"idle",     "listening", "thinking",
                                            "speaking", "quiet",     "muted"};
  for (const auto &name : names) {
    const auto source = LoadPpm(asset_directory / (name + ".ppm"));
    state_->images.emplace(
        name,
        CreateXImage(state_->display, DefaultVisual(state_->display, screen),
                     DefaultDepth(state_->display, screen), source));
  }
  XMapRaised(state_->display, state_->window);
  XFlush(state_->display);
}

DisplayWindow::~DisplayWindow() = default;

void DisplayWindow::ShowImage(const std::string &image_name) {
  const auto found = state_->images.find(image_name);
  if (found == state_->images.end())
    throw std::runtime_error("Unknown display expression: " + image_name);
  state_->active_image = image_name;
  XPutImage(state_->display, state_->window, state_->graphics_context,
            found->second, 0, 0, 0, 0, 800, 480);
  XFlush(state_->display);
}

void DisplayWindow::ProcessEvents() {
  XEvent event{};
  while (XPending(state_->display) > 0) {
    XNextEvent(state_->display, &event);
    if (event.type == Expose && event.xexpose.count == 0 &&
        !state_->active_image.empty())
      ShowImage(state_->active_image);
  }
}

} // namespace xiaocai_robot
