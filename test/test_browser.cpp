#include "Browser.hpp"
#include "Paint.hpp"
#include <cstdio>
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

// Search a rectangular region of the canvas for a pixel matching (r,g,b).
static bool FindColor(const Paint::Canvas &c, int x0, int y0, int x1, int y1,
                      int r, int g, int b) {
  x1 = std::min(x1, c.width());
  y1 = std::min(y1, c.height());
  for (int y = std::max(0, y0); y < y1; ++y) {
    for (int x = std::max(0, x0); x < x1; ++x) {
      Paint::Color p = c.at(x, y);
      if (p.r == r && p.g == g && p.b == b) {
        return true;
      }
    }
  }
  return false;
}

// Search for a "dark" pixel (antialiased text core), not pure black.
static bool FindDark(const Paint::Canvas &c, int x0, int y0, int x1, int y1) {
  x1 = std::min(x1, c.width());
  y1 = std::min(y1, c.height());
  for (int y = std::max(0, y0); y < y1; ++y) {
    for (int x = std::max(0, x0); x < x1; ++x) {
      Paint::Color p = c.at(x, y);
      if (p.r < 90 && p.g < 90 && p.b < 90) {
        return true;
      }
    }
  }
  return false;
}

static bool AnyNonBackground(const Paint::Canvas &c, int x0, int y0, int x1,
                             int y1, Paint::Color bg) {
  x1 = std::min(x1, c.width());
  y1 = std::min(y1, c.height());
  for (int y = std::max(0, y0); y < y1; ++y) {
    for (int x = std::max(0, x0); x < x1; ++x) {
      if (c.at(x, y) != bg) {
        return true;
      }
    }
  }
  return false;
}

int main() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "Browser orchestration" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  const int W = 400, H = 300;
  const int browser = Browser::Browser::kBrowserHeight;

  // --- empty state ---------------------------------------------------------
  {
    Browser::Browser b;
    Paint::Canvas c = b.render(W, H);
    Check("empty: browser bar drawn at top",
          FindColor(c, 0, 0, W, 4, 0xdd, 0xdd, 0xdd));
    Check("empty: status text shown below browser",
          AnyNonBackground(c, 0, browser, W, H,
                           Paint::Color{255, 255, 255, 255}));
  }

  // --- a real page with image + text + boxes -------------------------------
  // Write a 40x30 solid-red image the page can reference.
  const std::string logoPath = "/tmp/dwv_browser_logo.ppm";
  {
    Paint::Canvas logo(40, 30);
    logo.clear(Paint::Color{255, 0, 0, 255});
    logo.savePPM(logoPath);
  }

  const std::string html =
      "<html><head><style>"
      "body { background-color: #ffeedd; }"
      "#box { height: 30px; background-color: #00aa00; }"
      "</style></head><body>"
      "<h1 id='t'>Hello</h1>"
      "<div id='box'></div>"
      "<img src='dwv_browser_logo.ppm' width='40' height='30'>"
      "</body></html>";

  Browser::Browser b;
  Check("loadHtml succeeds", b.loadHtml(html, "/tmp/"));

  b.setUrlText("http://localhost/");
  Paint::Canvas c = b.render(W, H);

  // Browser + address bar text.
  Check("browser bar present", FindColor(c, 0, 0, W, 4, 0xdd, 0xdd, 0xdd));
  Check("address-bar URL text rendered",
        AnyNonBackground(c, 12, 6, W - 8, browser - 6,
                         Paint::Color{255, 255, 255, 255}));

  // Page content (below browser).
  Check("body background colour painted",
        FindColor(c, 0, browser, W, H, 0xff, 0xee, 0xdd));
  Check("heading text rendered (dark pixels under browser)",
        FindDark(c, 8, browser, 300, browser + 120));
  Check("green #box rendered",
        FindColor(c, 0, browser, W, H, 0x00, 0xaa, 0x00));
  Check("image (red) composited", FindColor(c, 0, browser, W, H, 255, 0, 0));

  std::remove(logoPath.c_str());

  // --- address-bar editing -------------------------------------------------
  {
    Browser::Browser e;
    e.setUrlText("");
    Browser::KeyInput k;
    k.kind = Browser::KeyInput::Char;
    for (char ch : std::string("abc")) {
      k.ch = ch;
      e.handleKey(k);
    }
    Check("typing appends to URL text", e.urlText() == "abc");

    // Left arrow moves cursor left
    Browser::KeyInput left;
    left.kind = Browser::KeyInput::Left;
    e.handleKey(left); // cursor is now after 'b'

    // Typing inserts character at cursor
    Browser::KeyInput ins;
    ins.kind = Browser::KeyInput::Char;
    ins.ch = 'x';
    e.handleKey(ins);
    Check("insert at cursor index works", e.urlText() == "abxc");

    // Backspace removes character before cursor
    Browser::KeyInput bs;
    bs.kind = Browser::KeyInput::Backspace;
    e.handleKey(bs); // deletes 'x'
    Check("backspace at cursor index works", e.urlText() == "abc");

    // Move to start and check bounds
    e.handleKey(left); // after 'b'
    e.handleKey(left); // after 'a'
    e.handleKey(left); // before 'a'
    e.handleKey(left); // no-op bounds check

    // Try backspacing at start
    Check("backspacing at start is no-op", !e.handleKey(bs));
    Check("url unchanged after invalid backspace", e.urlText() == "abc");

    // Right arrow moves cursor right
    Browser::KeyInput right;
    right.kind = Browser::KeyInput::Right;
    e.handleKey(right); // after 'a'
    e.handleKey(right); // after 'b'
    e.handleKey(right); // after 'c'

    // Backspace at end
    e.handleKey(bs);
    Check("backspace at end removes char", e.urlText() == "ab");
  }

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All browser tests passed." << std::endl;
  } else {
    std::cout << g_failures << " browser test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
