#ifndef PAINT_HPP
#define PAINT_HPP

#include "Layout.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Paint {

// An 8-bit-per-channel RGBA colour. Alpha 255 is opaque, 0 fully transparent.
struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;

  bool operator==(const Color &o) const {
    return r == o.r && g == o.g && b == o.b && a == o.a;
  }
  bool operator!=(const Color &o) const { return !(*this == o); }
};

// Parse a CSS colour string into `out`. Supports named colours (a common
// subset), #rgb / #rrggbb / #rrggbbaa hex, and rgb()/rgba() functions. Returns
// false (leaving `out` untouched) for empty, "transparent", or unrecognised
// values, so callers can skip painting.
bool parseColor(const std::string &text, Color &out);

// A single paint operation. Kept as a tagged struct (rather than a variant) so
// new command kinds (text, images, ...) can be added without touching call
// sites that ignore them.
enum class CommandType { SolidRect };

struct DisplayCommand {
  CommandType type = CommandType::SolidRect;
  Layout::Rect rect;
  Color color;
};

using DisplayList = std::vector<DisplayCommand>;

// Walk a laid-out box tree and emit the back-to-front list of paint commands:
// each box's background fill, then its four border edges, then its children.
DisplayList buildDisplayList(const Layout::LayoutBox &root);

// A CPU-side RGBA pixel buffer. This is the software rendering backend used to
// verify painting headlessly; the GPU (Vulkan) and OpenCL compositor backends
// will consume the same DisplayList.
class Canvas {
public:
  Canvas(int width, int height);

  int width() const { return m_width; }
  int height() const { return m_height; }
  const std::vector<Color> &pixels() const { return m_pixels; }

  // Fill the whole canvas with an opaque colour.
  void clear(Color color);

  // Alpha-blend `color` over the (clipped) rectangle. Coordinates are rounded
  // to the nearest pixel; the rect is clipped to the canvas bounds.
  void fillRect(const Layout::Rect &rect, Color color);

  // Colour at (x, y); returns transparent black for out-of-bounds reads.
  Color at(int x, int y) const;

  // Execute every command in the list in order.
  void paint(const DisplayList &list);

  // Write the canvas to a binary PPM (P6) file, dropping alpha. Returns false
  // if the file cannot be opened.
  bool savePPM(const std::string &path) const;

private:
  int m_width;
  int m_height;
  std::vector<Color> m_pixels;
};

// Pack a canvas into 32-bit pixels valued 0x00RRGGBB (XRGB). In little-endian
// memory each word is laid out B, G, R, X, which is exactly what both an X11
// default TrueColor visual (LSBFirst) and a Win32 BI_RGB 32-bit top-down DIB
// expect, so the same buffer feeds both window backends. Alpha is dropped.
std::vector<std::uint32_t> toPackedPixels(const Canvas &canvas);

} // namespace Paint
} // namespace DesktopWebview

#endif // PAINT_HPP
