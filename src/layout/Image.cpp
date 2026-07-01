#include "Image.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

// Declarations only; the implementation is compiled in src/stb_impl.cpp.
#include "stb_image.h"

namespace DesktopWebview {
namespace Image {

namespace {

// Little-endian readers over a byte span (BMP is little-endian).
std::uint16_t ReadU16LE(const std::uint8_t *p) {
  return static_cast<std::uint16_t>(p[0]) |
         (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t ReadU32LE(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t ReadS32LE(const std::uint8_t *p) {
  return static_cast<std::int32_t>(ReadU32LE(p));
}

// ---- BMP -------------------------------------------------------------------

bool DecodeBmp(const std::uint8_t *data, std::size_t size, Bitmap &out) {
  // BITMAPFILEHEADER (14) + BITMAPINFOHEADER (>=40).
  if (size < 54 || data[0] != 'B' || data[1] != 'M') {
    return false;
  }
  std::uint32_t pixelOffset = ReadU32LE(data + 10);
  std::uint32_t dibSize = ReadU32LE(data + 14);
  if (dibSize < 40) {
    return false; // only BITMAPINFOHEADER-style headers
  }
  std::int32_t width = ReadS32LE(data + 18);
  std::int32_t height = ReadS32LE(data + 22);
  std::uint16_t bpp = ReadU16LE(data + 28);
  std::uint32_t compression = ReadU32LE(data + 30);

  if (width <= 0 || height == 0 || compression != 0 /* BI_RGB */) {
    return false;
  }
  if (bpp != 24 && bpp != 32) {
    return false;
  }

  bool topDown = height < 0;
  std::int32_t absHeight = topDown ? -height : height;

  int bytesPerPixel = bpp / 8;
  // Rows are padded to a 4-byte boundary.
  std::size_t rowStride =
      (static_cast<std::size_t>(width) * bytesPerPixel + 3) &
      ~static_cast<std::size_t>(3);
  std::size_t needed =
      static_cast<std::size_t>(pixelOffset) + rowStride * absHeight;
  if (needed > size) {
    return false;
  }

  out.width = width;
  out.height = absHeight;
  out.pixels.assign(static_cast<size_t>(width) * absHeight,
                    Paint::Color{0, 0, 0, 255});

  for (std::int32_t row = 0; row < absHeight; ++row) {
    // BMP is bottom-up unless height is negative.
    std::int32_t srcRow = topDown ? row : (absHeight - 1 - row);
    const std::uint8_t *line = data + pixelOffset + rowStride * srcRow;
    for (std::int32_t x = 0; x < width; ++x) {
      const std::uint8_t *px =
          line + static_cast<std::size_t>(x) * bytesPerPixel;
      Paint::Color c;
      c.b = px[0];
      c.g = px[1];
      c.r = px[2];
      c.a = (bpp == 32) ? px[3] : 255;
      out.set(x, row, c);
    }
  }
  return true;
}

// ---- PPM (P6 / P3) ---------------------------------------------------------

// Read the next whitespace-delimited token, skipping '#' comment lines.
bool NextToken(const std::uint8_t *data, std::size_t size, std::size_t &pos,
               std::string &token) {
  token.clear();
  while (pos < size) {
    unsigned char c = data[pos];
    if (c == '#') {
      while (pos < size && data[pos] != '\n') {
        ++pos;
      }
      continue;
    }
    if (std::isspace(c)) {
      ++pos;
      continue;
    }
    break;
  }
  while (pos < size && !std::isspace(static_cast<unsigned char>(data[pos])) &&
         data[pos] != '#') {
    token.push_back(static_cast<char>(data[pos]));
    ++pos;
  }
  return !token.empty();
}

bool DecodePpm(const std::uint8_t *data, std::size_t size, Bitmap &out) {
  if (size < 2 || data[0] != 'P' || (data[1] != '6' && data[1] != '3')) {
    return false;
  }
  bool binary = data[1] == '6';
  std::size_t pos = 2;

  std::string wTok, hTok, maxTok;
  if (!NextToken(data, size, pos, wTok) || !NextToken(data, size, pos, hTok) ||
      !NextToken(data, size, pos, maxTok)) {
    return false;
  }
  int width = std::atoi(wTok.c_str());
  int height = std::atoi(hTok.c_str());
  int maxVal = std::atoi(maxTok.c_str());
  if (width <= 0 || height <= 0 || maxVal <= 0 || maxVal > 255) {
    return false;
  }

  out.width = width;
  out.height = height;
  out.pixels.assign(static_cast<size_t>(width) * height,
                    Paint::Color{0, 0, 0, 255});

  if (binary) {
    // Exactly one whitespace byte separates the header from binary data.
    ++pos;
    std::size_t needed = static_cast<std::size_t>(width) * height * 3;
    if (pos + needed > size) {
      return false;
    }
    for (int i = 0; i < width * height; ++i) {
      Paint::Color c;
      c.r = data[pos + i * 3 + 0];
      c.g = data[pos + i * 3 + 1];
      c.b = data[pos + i * 3 + 2];
      c.a = 255;
      out.pixels[i] = c;
    }
  } else {
    for (int i = 0; i < width * height; ++i) {
      std::string r, g, b;
      if (!NextToken(data, size, pos, r) || !NextToken(data, size, pos, g) ||
          !NextToken(data, size, pos, b)) {
        return false;
      }
      out.pixels[i] =
          Paint::Color{static_cast<std::uint8_t>(std::atoi(r.c_str())),
                       static_cast<std::uint8_t>(std::atoi(g.c_str())),
                       static_cast<std::uint8_t>(std::atoi(b.c_str())), 255};
    }
  }
  return true;
}

} // namespace

Paint::Color Bitmap::at(int x, int y) const {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return Paint::Color{0, 0, 0, 0};
  }
  return pixels[static_cast<size_t>(y) * width + x];
}

void Bitmap::set(int x, int y, Paint::Color color) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return;
  }
  pixels[static_cast<size_t>(y) * width + x] = color;
}

void Bitmap::blitTo(Paint::Canvas &dst, int dstX, int dstY) const {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      dst.blendPixel(dstX + x, dstY + y, at(x, y));
    }
  }
}

Format detectFormat(const std::uint8_t *data, std::size_t size) {
  if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
    return Format::Bmp;
  }
  if (size >= 2 && data[0] == 'P' && (data[1] == '6' || data[1] == '3')) {
    return Format::Ppm;
  }
  return Format::Unknown;
}

namespace {

// Decode PNG/JPEG/GIF/etc. via stb_image (forced to RGBA).
bool DecodeStb(const std::uint8_t *data, std::size_t size, Bitmap &out) {
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels =
      stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
  if (!pixels) {
    return false;
  }
  out.width = w;
  out.height = h;
  out.pixels.resize(static_cast<size_t>(w) * h);
  for (int i = 0; i < w * h; ++i) {
    out.pixels[i] = Paint::Color{pixels[i * 4 + 0], pixels[i * 4 + 1],
                                 pixels[i * 4 + 2], pixels[i * 4 + 3]};
  }
  stbi_image_free(pixels);
  return out.valid();
}

} // namespace

bool decode(const std::uint8_t *data, std::size_t size, Bitmap &out) {
  switch (detectFormat(data, size)) {
  case Format::Bmp:
    return DecodeBmp(data, size, out);
  case Format::Ppm:
    return DecodePpm(data, size, out);
  default:
    // PNG / JPEG / GIF / etc.
    return DecodeStb(data, size, out);
  }
}

bool decode(const std::vector<std::uint8_t> &data, Bitmap &out) {
  return decode(data.data(), data.size(), out);
}

bool decodeFile(const std::string &path, Bitmap &out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
  return decode(data, out);
}

} // namespace Image
} // namespace DesktopWebview
