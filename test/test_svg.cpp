#include "../include/Image.hpp"
#include "../include/Svg.hpp"
#include <iostream>
#include <string>

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

static bool Eq(const Paint::Color &c, int r, int g, int b) {
  return c.r == r && c.g == g && c.b == b;
}

static void RectTests() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "SVG <rect> + size resolution" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  std::string svg =
      "<svg width='100' height='80'>"
      "<rect x='10' y='10' width='40' height='30' fill='red'/>"
      "<rect x='60' y='40' width='30' height='30' fill='#00ff00'/>"
      "</svg>";

  Image::Bitmap img;
  Check("render succeeds", Svg::render(svg, img));
  Check("size from width/height attrs", img.width == 100 && img.height == 80);
  Check("inside red rect is red", Eq(img.at(20, 20), 255, 0, 0));
  Check("inside green rect is green", Eq(img.at(70, 50), 0, 255, 0));
  Check("gap between rects untouched (transparent)", img.at(55, 20).a == 0);
}

static void ShapeTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "SVG circle / polygon / path / background" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  std::string svg =
      "<svg width='120' height='120'>"
      "<circle cx='30' cy='30' r='20' fill='blue'/>"
      "<polygon points='70,10 110,10 90,50' fill='#ffff00'/>"
      "<path d='M 10 80 L 60 80 L 60 110 L 10 110 Z' fill='magenta'/>"
      "</svg>";

  Svg::RenderOptions opts;
  opts.background = Paint::Color{255, 255, 255, 255}; // opaque white bg
  Image::Bitmap img;
  Check("render succeeds", Svg::render(svg, img, opts));
  Check("background applied (corner white)", Eq(img.at(118, 2), 255, 255, 255));
  Check("circle centre is blue", Eq(img.at(30, 30), 0, 0, 255));
  Check("circle outside stays background", Eq(img.at(2, 2), 255, 255, 255));
  Check("polygon interior is yellow", Eq(img.at(90, 20), 255, 255, 0));
  Check("path-filled rect is magenta", Eq(img.at(30, 95), 255, 0, 255));
}

static void OverrideSizeTest() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "SVG explicit output size + viewBox fallback" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // No width/height attrs; size comes from viewBox.
  std::string svg = "<svg viewBox='0 0 50 40'><rect width='50' height='40' "
                    "fill='black'/></svg>";
  Image::Bitmap img;
  Check("render succeeds", Svg::render(svg, img));
  Check("size taken from viewBox", img.width == 50 && img.height == 40);
  Check("filled black", Eq(img.at(25, 20), 0, 0, 0));

  // Explicit override wins.
  Svg::RenderOptions opts;
  opts.width = 10;
  opts.height = 10;
  Image::Bitmap img2;
  Check("render with override", Svg::render(svg, img2, opts));
  Check("size overridden to 10x10", img2.width == 10 && img2.height == 10);
}

int main() {
  RectTests();
  ShapeTests();
  OverrideSizeTest();

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All SVG tests passed." << std::endl;
  } else {
    std::cout << g_failures << " SVG test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
