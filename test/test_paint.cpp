#include "Css.hpp"
#include "Layout.hpp"
#include "Paint.hpp"
#include "Wrapper.hpp"
#include <cstdio>
#include <fstream>
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

static bool Eq(const Paint::Color &c, int r, int g, int b, int a = 255) {
  return c.r == r && c.g == g && c.b == b && c.a == a;
}

static void ColorTests() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "Colour parsing" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Paint::Color c;
  Check("named 'red'", Paint::parseColor("red", c) && Eq(c, 255, 0, 0));
  Check("named 'white' (case-insensitive)",
        Paint::parseColor("WHITE", c) && Eq(c, 255, 255, 255));
  Check("#fff short hex", Paint::parseColor("#fff", c) && Eq(c, 255, 255, 255));
  Check("#ff8800 hex", Paint::parseColor("#ff8800", c) && Eq(c, 255, 136, 0));
  Check("#0000ff80 hex w/ alpha",
        Paint::parseColor("#0000ff80", c) && Eq(c, 0, 0, 255, 128));
  Check("rgb(1, 2, 3)", Paint::parseColor("rgb(1, 2, 3)", c) && Eq(c, 1, 2, 3));
  Check("rgba(0,0,0,0.5) -> a~128",
        Paint::parseColor("rgba(0,0,0,0.5)", c) && c.a >= 126 && c.a <= 129);
  Check("rgba w/ space and slash",
        Paint::parseColor("rgba(255 0 0 / 50%)", c) && Eq(c, 255, 0, 0, 128));
  Check("hsl(120, 100%, 50%) -> lime",
        Paint::parseColor("hsl(120, 100%, 50%)", c) && Eq(c, 0, 255, 0, 255));
  Check("hsla(240, 100%, 50%, 0.8)",
        Paint::parseColor("hsla(240, 100%, 50%, 0.8)", c) &&
            Eq(c, 0, 0, 255, 204));
  Check("'transparent' rejected", !Paint::parseColor("transparent", c));
  Check("empty rejected", !Paint::parseColor("", c));
  Check("unknown keyword rejected", !Paint::parseColor("notacolor", c));
}

// Build a small page and lay it out at 200px wide.
static Layout::LayoutBox BuildScene(Wrapper::HtmlDocument &doc,
                                    Layout::StyledNode &styleOut) {
  const std::string html = "<html><body><div id=\"box\">x</div></body></html>";
  const std::string css =
      "body { background-color: white; }"
      "#box { width: 100px; height: 50px; background-color: red;"
      "       border: 10px solid blue; margin: 20px; }";

  doc.parse(html);
  Css::Stylesheet sheet = Css::parse(css);
  styleOut = Layout::styleTree(doc.root(), sheet);
  return Layout::layout(styleOut, 200.0f);
}

static void DisplayListTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Display list" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Wrapper::HtmlDocument doc;
  Layout::StyledNode style;
  Layout::LayoutBox root = BuildScene(doc, style);

  Paint::DisplayList list = Paint::buildDisplayList(root);

  // Expected commands: body background (1) + box background (1) + 4 borders
  // = 6.
  Check("6 paint commands emitted", list.size() == 6);

  // First command is the body's white background covering the viewport width.
  if (!list.empty()) {
    const Paint::DisplayCommand &bodyBg = list.front();
    Check("first command is body white background",
          Eq(bodyBg.color, 255, 255, 255) && bodyBg.rect.x == 0 &&
              bodyBg.rect.width == 200);
  }

  // Somewhere there must be a red background rect (the box).
  bool hasRed = false;
  bool hasBlue = false;
  for (const Paint::DisplayCommand &cmd : list) {
    if (Eq(cmd.color, 255, 0, 0)) {
      hasRed = true;
    }
    if (Eq(cmd.color, 0, 0, 255)) {
      hasBlue = true;
    }
  }
  Check("box red background present", hasRed);
  Check("blue border rects present", hasBlue);
}

static void RasterTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Software rasteriser" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Wrapper::HtmlDocument doc;
  Layout::StyledNode style;
  Layout::LayoutBox root = BuildScene(doc, style);
  Paint::DisplayList list = Paint::buildDisplayList(root);

  Paint::Canvas canvas(200, 120);
  canvas.paint(list);

  // Box geometry: content (30,30)-(130,80); border box (20,20)-(140,90).
  Check("box interior is red", Eq(canvas.at(60, 50), 255, 0, 0));
  Check("left border is blue", Eq(canvas.at(22, 50), 0, 0, 255));
  Check("top border is blue", Eq(canvas.at(60, 22), 0, 0, 255));
  Check("outside the box is body white", Eq(canvas.at(5, 5), 255, 255, 255));

  // Alpha compositing: 50% black over white -> mid grey.
  Paint::Canvas blend(1, 1);
  blend.clear(Paint::Color{255, 255, 255, 255});
  blend.fillRect(Layout::Rect{0, 0, 1, 1}, Paint::Color{0, 0, 0, 128});
  Paint::Color mid = blend.at(0, 0);
  Check("alpha blend 50% black/white -> ~grey",
        mid.r >= 120 && mid.r <= 135 && mid.r == mid.g && mid.g == mid.b);

  // PPM output round-trips a valid header.
  std::string path = "/tmp/dwv_paint_test.ppm";
  std::remove(path.c_str());
  Check("savePPM succeeds", canvas.savePPM(path));
  std::ifstream in(path, std::ios::binary);
  std::string magic;
  in >> magic;
  Check("PPM file starts with P6 magic", magic == "P6");
  std::remove(path.c_str());

  // Native pixel packing (the buffer fed to X11/Win32): 0x00RRGGBB.
  Paint::Canvas one(1, 1);
  one.clear(Paint::Color{255, 136, 0, 255}); // #ff8800
  std::vector<std::uint32_t> packed = Paint::toPackedPixels(one);
  Check("toPackedPixels packs as 0x00RRGGBB",
        packed.size() == 1 && packed[0] == 0x00ff8800u);
}

static void BorderRadiusTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "border-radius" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // A 100x100 red box with a 20px radius, no border, on a white background.
  const std::string html = "<html><body><div id=\"box\"></div></body></html>";
  const std::string css =
      "body { background-color: white; margin: 0; }"
      "#box { width: 100px; height: 100px; background-color: red; "
      "       border-radius: 20px; }";
  Wrapper::HtmlDocument doc;
  doc.parse(html);
  Css::Stylesheet sheet = Css::parse(css);
  Layout::StyledNode style = Layout::styleTree(doc.root(), sheet);
  Layout::LayoutBox root = Layout::layout(style, 100.0f);
  Paint::DisplayList list = Paint::buildDisplayList(root);

  Paint::Canvas canvas(100, 100);
  canvas.paint(list);

  Check("centre of a rounded box is filled", Eq(canvas.at(50, 50), 255, 0, 0));
  Check("edge midpoint (away from any corner) is filled",
        Eq(canvas.at(50, 2), 255, 0, 0));
  Check("extreme corner pixel is outside the radius arc (background shows)",
        Eq(canvas.at(0, 0), 255, 255, 255));
  Check("a point just inside the rounded corner's arc is filled",
        Eq(canvas.at(6, 6), 255, 0, 0));

  // radius <= 0 (unset) must fall back to an ordinary square fill -- the
  // extreme corner is no longer excluded.
  const std::string cssSquare =
      "body { background-color: white; margin: 0; }"
      "#box { width: 100px; height: 100px; background-color: red; }";
  Css::Stylesheet sheetSquare = Css::parse(cssSquare);
  Layout::StyledNode styleSquare = Layout::styleTree(doc.root(), sheetSquare);
  Layout::LayoutBox rootSquare = Layout::layout(styleSquare, 100.0f);
  Paint::Canvas canvasSquare(100, 100);
  canvasSquare.paint(Paint::buildDisplayList(rootSquare));
  Check("no border-radius: corner pixel IS filled (square box)",
        Eq(canvasSquare.at(0, 0), 255, 0, 0));
}

int main() {
  ColorTests();
  DisplayListTests();
  RasterTests();
  BorderRadiusTests();

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All paint tests passed." << std::endl;
  } else {
    std::cout << g_failures << " paint test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
