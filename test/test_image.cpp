#include "../include/Image.hpp"
#include "../include/Paint.hpp"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace DesktopWebview;

static int g_failures = 0;

static void Check(const std::string &label, bool condition) {
  if (condition) {
    std::cout << "  [PASS] " << label << std::endl;
  } else {
    std::cout << "  [FAIL] " << label << std::endl;
    ++g_failures;
  }
}

static bool Eq(const Paint::Color &c, int r, int g, int b, int a = 255) {
  return c.r == r && c.g == g && c.b == b && c.a == a;
}

static void putLE32(std::vector<std::uint8_t> &v, std::uint32_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
  v.push_back((x >> 16) & 0xff);
  v.push_back((x >> 24) & 0xff);
}
static void putLE16(std::vector<std::uint8_t> &v, std::uint16_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
}

// Build a 2x2 24-bit BMP: TL red, TR green, BL blue, BR white.
static std::vector<std::uint8_t> MakeBmp() {
  std::vector<std::uint8_t> px; // bottom-up rows, BGR, padded to 4 bytes
  // Bottom row (y=1 in image): blue, white
  px.insert(px.end(), {255, 0, 0, 255, 255, 255}); // B(255,0,0) W(255,255,255)
  px.push_back(0);
  px.push_back(0); // pad to 8 bytes (6 -> 8)
  // Top row (y=0 in image): red, green
  px.insert(px.end(), {0, 0, 255, 0, 255, 0}); // R(0,0,255) G(0,255,0) in BGR
  px.push_back(0);
  px.push_back(0);

  std::vector<std::uint8_t> bmp;
  bmp.push_back('B');
  bmp.push_back('M');
  std::uint32_t pixelOffset = 14 + 40;
  std::uint32_t fileSize = pixelOffset + static_cast<std::uint32_t>(px.size());
  putLE32(bmp, fileSize);
  putLE16(bmp, 0);
  putLE16(bmp, 0);
  putLE32(bmp, pixelOffset);
  // BITMAPINFOHEADER
  putLE32(bmp, 40);
  putLE32(bmp, 2);  // width
  putLE32(bmp, 2);  // height (positive => bottom-up)
  putLE16(bmp, 1);  // planes
  putLE16(bmp, 24); // bpp
  putLE32(bmp, 0);  // BI_RGB
  putLE32(bmp, static_cast<std::uint32_t>(px.size()));
  putLE32(bmp, 2835);
  putLE32(bmp, 2835);
  putLE32(bmp, 0);
  putLE32(bmp, 0);
  bmp.insert(bmp.end(), px.begin(), px.end());
  return bmp;
}

static void BmpTests() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "BMP decoding" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  std::vector<std::uint8_t> bmp = MakeBmp();
  Check("format sniffed as BMP",
        Image::detectFormat(bmp.data(), bmp.size()) == Image::Format::Bmp);

  Image::Bitmap img;
  Check("decode succeeds", Image::decode(bmp, img));
  Check("dimensions 2x2", img.width == 2 && img.height == 2);
  // Origin top-left after bottom-up flip.
  Check("top-left red", Eq(img.at(0, 0), 255, 0, 0));
  Check("top-right green", Eq(img.at(1, 0), 0, 255, 0));
  Check("bottom-left blue", Eq(img.at(0, 1), 0, 0, 255));
  Check("bottom-right white", Eq(img.at(1, 1), 255, 255, 255));
}

static void PpmTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "PPM decoding + Canvas round-trip" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // ASCII P3 with a comment line.
  std::string p3 = "P3\n# a comment\n2 1\n255\n255 0 0  0 128 255\n";
  std::vector<std::uint8_t> data(p3.begin(), p3.end());
  Image::Bitmap img;
  Check("P3 decode succeeds", Image::decode(data, img));
  Check("P3 dims 2x1", img.width == 2 && img.height == 1);
  Check("P3 pixel 0 red", Eq(img.at(0, 0), 255, 0, 0));
  Check("P3 pixel 1 (0,128,255)", Eq(img.at(1, 0), 0, 128, 255));

  // Round-trip: render a canvas, save P6, decode it back.
  Paint::Canvas canvas(4, 3);
  canvas.clear(Paint::Color{10, 20, 30, 255});
  canvas.fillRect(Layout::Rect{1, 1, 2, 1}, Paint::Color{200, 100, 50, 255});
  std::string path = "/tmp/dwv_image_test.ppm";
  Check("canvas savePPM", canvas.savePPM(path));
  Image::Bitmap back;
  Check("decode written P6 file", Image::decodeFile(path, back));
  Check("round-trip dims 4x3", back.width == 4 && back.height == 3);
  Check("round-trip background pixel", Eq(back.at(0, 0), 10, 20, 30));
  Check("round-trip filled pixel", Eq(back.at(1, 1), 200, 100, 50));
  std::remove(path.c_str());
}

// A 4x4 solid orange (255,128,0) PNG, to exercise the stb_image path.
static const unsigned char kPng[] = {
    137, 80,  78,  71,  13, 10,  26, 10,  0,   0,   0,   13,  73,  72, 68,
    82,  0,   0,   0,   4,  0,   0,  0,   4,   8,   6,   0,   0,   0,  169,
    241, 158, 126, 0,   0,  0,   18, 73,  68,  65,  84,  120, 156, 99, 248,
    223, 192, 240, 31,  25, 51,  144, 46, 0,   0,   80,  79,  39,  225, 235,
    0,   248, 165, 0,   0,  0,   0,  73,  69,  78,  68,  174, 66,  96, 130};

static void PngTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "PNG decoding (stb_image)" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Check("format not BMP/PPM",
        Image::detectFormat(kPng, sizeof(kPng)) == Image::Format::Unknown);
  Image::Bitmap img;
  Check("PNG decode succeeds", Image::decode(kPng, sizeof(kPng), img));
  Check("PNG dims 4x4", img.width == 4 && img.height == 4);
  Check("PNG pixel is orange (255,128,0)", Eq(img.at(2, 2), 255, 128, 0));
}

static void BlitTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Image blit onto Canvas" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Image::Bitmap img;
  img.width = 2;
  img.height = 2;
  img.pixels.assign(4, Paint::Color{255, 0, 0, 255}); // opaque red
  // Make one pixel half-transparent.
  img.set(0, 0, Paint::Color{0, 0, 0, 128});

  Paint::Canvas canvas(4, 4);
  canvas.clear(Paint::Color{255, 255, 255, 255});
  img.blitTo(canvas, 1, 1);

  Check("opaque pixel copied", Eq(canvas.at(2, 1), 255, 0, 0));
  Check("pixel outside blit untouched", Eq(canvas.at(0, 0), 255, 255, 255));
  Paint::Color blended = canvas.at(1, 1); // 50% black over white
  Check("semi-transparent pixel blended",
        blended.r >= 120 && blended.r <= 135 && blended.r == blended.g);
}

int main() {
  BmpTests();
  PpmTests();
  PngTests();
  BlitTests();

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All image tests passed." << std::endl;
  } else {
    std::cout << g_failures << " image test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
