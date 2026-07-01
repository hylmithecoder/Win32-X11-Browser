#ifndef IMAGE_HPP
#define IMAGE_HPP

#include "Paint.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Image {

// A decoded raster image: RGBA pixels in row-major order with the origin at the
// top-left. This is the common currency handed to the compositor and the video
// renderer.
struct Bitmap {
  int width = 0;
  int height = 0;
  std::vector<Paint::Color> pixels;

  bool valid() const {
    return width > 0 && height > 0 &&
           pixels.size() == static_cast<size_t>(width) * height;
  }

  Paint::Color at(int x, int y) const;
  void set(int x, int y, Paint::Color color);

  // Alpha-blend this image onto `dst` with its top-left at (dstX, dstY).
  void blitTo(Paint::Canvas &dst, int dstX, int dstY) const;
};

// Recognised container formats. Real raster codecs (PNG/JPEG/GIF) are a later
// stage; this MVP decodes the two dependency-free formats from scratch.
enum class Format { Unknown, Bmp, Ppm };

// Sniff the format from a leading byte signature.
Format detectFormat(const std::uint8_t *data, std::size_t size);

// Decode an image from memory into `out`. Supports uncompressed BMP (24/32-bit)
// and PPM (P6 binary / P3 ASCII). Returns false on unsupported or malformed
// input.
bool decode(const std::uint8_t *data, std::size_t size, Bitmap &out);
bool decode(const std::vector<std::uint8_t> &data, Bitmap &out);
bool decodeFile(const std::string &path, Bitmap &out);

} // namespace Image
} // namespace DesktopWebview

#endif // IMAGE_HPP
