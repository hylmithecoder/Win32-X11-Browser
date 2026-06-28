#include "../include/Font.hpp"
#include "../include/Paint.hpp"
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

static int CountLit(const Paint::Canvas &c, Paint::Color bg) {
  int n = 0;
  for (int y = 0; y < c.height(); ++y) {
    for (int x = 0; x < c.width(); ++x) {
      if (c.at(x, y) != bg) {
        ++n;
      }
    }
  }
  return n;
}

// These tests hold for both the TrueType face and the bitmap fallback.
int main() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "Font rendering (" << (Font::usingTrueType() ? "TrueType" : "bitmap")
            << ")" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Paint::Color black{0, 0, 0, 255};
  Paint::Color white{255, 255, 255, 255};

  // Metrics scale with length and size.
  Check("wider text is wider",
        Font::textWidth("ABCDEF", 16) > Font::textWidth("AB", 16));
  Check("larger size is wider",
        Font::textWidth("A", 32) > Font::textWidth("A", 16));
  Check("larger size is taller", Font::lineHeight(32) > Font::lineHeight(16));
  Check("line height positive", Font::lineHeight(16) > 0);

  // Space draws nothing; a letter draws something.
  {
    Paint::Canvas c(40, 40);
    c.clear(white);
    Font::drawText(c, 2, 2, " ", black, 20);
    Check("space draws no pixels", CountLit(c, white) == 0);
  }
  {
    Paint::Canvas c(40, 40);
    c.clear(white);
    int adv = Font::drawText(c, 2, 2, "A", black, 20);
    Check("'A' draws pixels", CountLit(c, white) > 0);
    Check("'A' has a positive advance", adv > 0);
  }

  // Colour is honoured.
  {
    Paint::Canvas c(40, 40);
    c.clear(white);
    Paint::Color red{255, 0, 0, 255};
    Font::drawText(c, 2, 2, "R", red, 24);
    bool foundReddish = false;
    for (int y = 0; y < c.height() && !foundReddish; ++y) {
      for (int x = 0; x < c.width(); ++x) {
        Paint::Color p = c.at(x, y);
        if (p.r > p.g && p.r > p.b && p.r > 80) { // red-dominant (AA-aware)
          foundReddish = true;
          break;
        }
      }
    }
    Check("text drawn in requested colour", foundReddish);
  }

  // Two characters advance past one.
  {
    Paint::Canvas c(80, 40);
    c.clear(white);
    int one = Font::drawText(c, 2, 2, "M", black, 20);
    Paint::Canvas c2(80, 40);
    c2.clear(white);
    int two = Font::drawText(c2, 2, 2, "MM", black, 20);
    Check("two glyphs advance further than one", two > one);
  }

  // Larger size lights more pixels.
  {
    Paint::Canvas small(48, 48), big(48, 48);
    small.clear(white);
    big.clear(white);
    Font::drawText(small, 2, 2, "8", black, 12);
    Font::drawText(big, 2, 2, "8", black, 36);
    Check("bigger glyph lights more pixels",
          CountLit(big, white) > CountLit(small, white));
  }

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All font tests passed." << std::endl;
  } else {
    std::cout << g_failures << " font test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
