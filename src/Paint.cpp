#include "../include/Paint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace DesktopWebview {
namespace Paint {

namespace {

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string Trim(const std::string &s) {
  const std::string ws = " \t\r\n\f";
  size_t begin = s.find_first_not_of(ws);
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

int HexPair(char hi, char lo) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') {
      return 10 + (c - 'a');
    }
    return -1;
  };
  int h = nibble(hi);
  int l = nibble(lo);
  if (h < 0 || l < 0) {
    return -1;
  }
  return h * 16 + l;
}

bool ParseHex(const std::string &s, Color &out) {
  // s begins with '#'.
  std::string hex = s.substr(1);
  Color c;
  if (hex.size() == 3) {
    // #rgb -> #rrggbb
    int r = HexPair(hex[0], hex[0]);
    int g = HexPair(hex[1], hex[1]);
    int b = HexPair(hex[2], hex[2]);
    if (r < 0 || g < 0 || b < 0) {
      return false;
    }
    c.r = static_cast<std::uint8_t>(r);
    c.g = static_cast<std::uint8_t>(g);
    c.b = static_cast<std::uint8_t>(b);
  } else if (hex.size() == 6 || hex.size() == 8) {
    int r = HexPair(hex[0], hex[1]);
    int g = HexPair(hex[2], hex[3]);
    int b = HexPair(hex[4], hex[5]);
    if (r < 0 || g < 0 || b < 0) {
      return false;
    }
    c.r = static_cast<std::uint8_t>(r);
    c.g = static_cast<std::uint8_t>(g);
    c.b = static_cast<std::uint8_t>(b);
    if (hex.size() == 8) {
      int a = HexPair(hex[6], hex[7]);
      if (a < 0) {
        return false;
      }
      c.a = static_cast<std::uint8_t>(a);
    }
  } else {
    return false;
  }
  out = c;
  return true;
}

std::vector<std::string> ParseColorArgs(const std::string &inner) {
  std::vector<std::string> args;
  std::string cur;
  for (char c : inner) {
    if (c == ',' || c == '/' || std::isspace(static_cast<unsigned char>(c))) {
      std::string trimmed = Trim(cur);
      if (!trimmed.empty()) {
        args.push_back(trimmed);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  std::string trimmed = Trim(cur);
  if (!trimmed.empty()) {
    args.push_back(trimmed);
  }
  return args;
}

Color HslToRgb(float h, float s, float l, std::uint8_t alpha = 255) {
  h = std::fmod(h, 360.0f);
  if (h < 0)
    h += 360.0f;

  float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
  float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
  float m = l - c / 2.0f;

  float r = 0, g = 0, b = 0;
  if (h >= 0 && h < 60) {
    r = c;
    g = x;
    b = 0;
  } else if (h >= 60 && h < 120) {
    r = x;
    g = c;
    b = 0;
  } else if (h >= 120 && h < 180) {
    r = 0;
    g = c;
    b = x;
  } else if (h >= 180 && h < 240) {
    r = 0;
    g = x;
    b = c;
  } else if (h >= 240 && h < 300) {
    r = x;
    g = 0;
    b = c;
  } else {
    r = c;
    g = 0;
    b = x;
  }

  Color rgb;
  rgb.r = static_cast<std::uint8_t>(std::clamp((r + m) * 255.0f, 0.0f, 255.0f));
  rgb.g = static_cast<std::uint8_t>(std::clamp((g + m) * 255.0f, 0.0f, 255.0f));
  rgb.b = static_cast<std::uint8_t>(std::clamp((b + m) * 255.0f, 0.0f, 255.0f));
  rgb.a = alpha;
  return rgb;
}

bool ParseRgbFunction(const std::string &s, Color &out) {
  size_t open = s.find('(');
  size_t close = s.find(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return false;
  }
  std::string inner = s.substr(open + 1, close - open - 1);
  std::vector<std::string> parts = ParseColorArgs(inner);
  if (parts.size() != 3 && parts.size() != 4) {
    return false;
  }

  auto parseChannel = [](const std::string &arg, bool &ok) -> int {
    ok = !arg.empty();
    if (!ok)
      return 0;
    float val = std::atof(arg.c_str());
    if (arg.find('%') != std::string::npos) {
      val = val / 100.0f * 255.0f;
    }
    return std::clamp(static_cast<int>(val), 0, 255);
  };

  bool ok = true;
  bool partOk = false;
  Color c;
  c.r = static_cast<std::uint8_t>(parseChannel(parts[0], partOk));
  ok = ok && partOk;
  c.g = static_cast<std::uint8_t>(parseChannel(parts[1], partOk));
  ok = ok && partOk;
  c.b = static_cast<std::uint8_t>(parseChannel(parts[2], partOk));
  ok = ok && partOk;

  if (parts.size() == 4) {
    float alphaVal = std::atof(parts[3].c_str());
    if (parts[3].find('%') != std::string::npos) {
      alphaVal = alphaVal / 100.0f;
    }
    c.a = static_cast<std::uint8_t>(std::clamp(alphaVal, 0.0f, 1.0f) * 255.0f +
                                    0.5f);
  } else {
    c.a = 255;
  }
  if (!ok) {
    return false;
  }
  out = c;
  return true;
}

bool ParseHslFunction(const std::string &s, Color &out) {
  size_t open = s.find('(');
  size_t close = s.find(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return false;
  }
  std::string inner = s.substr(open + 1, close - open - 1);
  std::vector<std::string> parts = ParseColorArgs(inner);
  if (parts.size() != 3 && parts.size() != 4) {
    return false;
  }

  float h = std::atof(parts[0].c_str());
  float s_val = std::atof(parts[1].c_str());
  if (parts[1].find('%') != std::string::npos) {
    s_val /= 100.0f;
  }
  float l_val = std::atof(parts[2].c_str());
  if (parts[2].find('%') != std::string::npos) {
    l_val /= 100.0f;
  }

  std::uint8_t alpha = 255;
  if (parts.size() == 4) {
    float alphaVal = std::atof(parts[3].c_str());
    if (parts[3].find('%') != std::string::npos) {
      alphaVal = alphaVal / 100.0f;
    }
    alpha = static_cast<std::uint8_t>(
        std::clamp(alphaVal, 0.0f, 1.0f) * 255.0f + 0.5f);
  }

  out = HslToRgb(h, s_val, l_val, alpha);
  return true;
}

const std::unordered_map<std::string, Color> &NamedColors() {
  static const std::unordered_map<std::string, Color> table = {
      {"black", {0, 0, 0, 255}},       {"white", {255, 255, 255, 255}},
      {"red", {255, 0, 0, 255}},       {"green", {0, 128, 0, 255}},
      {"lime", {0, 255, 0, 255}},      {"blue", {0, 0, 255, 255}},
      {"yellow", {255, 255, 0, 255}},  {"cyan", {0, 255, 255, 255}},
      {"aqua", {0, 255, 255, 255}},    {"magenta", {255, 0, 255, 255}},
      {"fuchsia", {255, 0, 255, 255}}, {"gray", {128, 128, 128, 255}},
      {"grey", {128, 128, 128, 255}},  {"silver", {192, 192, 192, 255}},
      {"maroon", {128, 0, 0, 255}},    {"olive", {128, 128, 0, 255}},
      {"navy", {0, 0, 128, 255}},      {"teal", {0, 128, 128, 255}},
      {"purple", {128, 0, 128, 255}},  {"orange", {255, 165, 0, 255}},
      {"pink", {255, 192, 203, 255}},  {"brown", {165, 42, 42, 255}},
  };
  return table;
}

// Blend src over dst using src's alpha (straight-alpha "over" operator).
Color BlendOver(Color dst, Color src) {
  if (src.a == 255) {
    return src;
  }
  if (src.a == 0) {
    return dst;
  }
  float sa = src.a / 255.0f;
  float da = 1.0f - sa;
  auto mix = [&](std::uint8_t s, std::uint8_t d) {
    return static_cast<std::uint8_t>(s * sa + d * da + 0.5f);
  };
  Color out;
  out.r = mix(src.r, dst.r);
  out.g = mix(src.g, dst.g);
  out.b = mix(src.b, dst.b);
  // Resulting coverage: src over dst.
  out.a = static_cast<std::uint8_t>(src.a + dst.a * da + 0.5f);
  return out;
}

// First whitespace/space-separated colour token that parses (used to pull a
// colour out of shorthands like "1px solid red").
bool FirstColorToken(const std::string &value, Color &out) {
  std::stringstream ss(value);
  std::string tok;
  while (ss >> tok) {
    if (parseColor(tok, out)) {
      return true;
    }
  }
  return false;
}

// Resolve a box's background colour, if any (background-color, then the
// background shorthand).
bool BackgroundColor(const Layout::LayoutBox &box, Color &out) {
  std::string direct = box.value("background-color");
  if (!direct.empty() && parseColor(direct, out)) {
    return true;
  }
  std::string shorthand = box.value("background");
  if (!shorthand.empty() && FirstColorToken(shorthand, out)) {
    return true;
  }
  return false;
}

// Resolve a box's border colour, if any (border-color, then a colour token in
// the border shorthand).
bool BorderColor(const Layout::LayoutBox &box, Color &out) {
  std::string direct = box.value("border-color");
  if (!direct.empty() && parseColor(direct, out)) {
    return true;
  }
  std::string shorthand = box.value("border");
  if (!shorthand.empty() && FirstColorToken(shorthand, out)) {
    return true;
  }
  return false;
}

void PaintBox(const Layout::LayoutBox &box, DisplayList &out) {
  const Layout::Dimensions &d = box.dimensions;

  // Background fills the border box.
  Color bg;
  if (BackgroundColor(box, bg)) {
    out.push_back({CommandType::SolidRect, d.borderBox(), bg});
  }

  // Borders: four solid rects around the padding box, drawn in the border
  // colour. Only emitted when a colour resolves and the edge has width.
  Color bc;
  if (BorderColor(box, bc)) {
    Layout::Rect border = d.borderBox();
    if (d.border.left > 0) {
      out.push_back({CommandType::SolidRect,
                     {border.x, border.y, d.border.left, border.height},
                     bc});
    }
    if (d.border.right > 0) {
      out.push_back({CommandType::SolidRect,
                     {border.x + border.width - d.border.right, border.y,
                      d.border.right, border.height},
                     bc});
    }
    if (d.border.top > 0) {
      out.push_back({CommandType::SolidRect,
                     {border.x, border.y, border.width, d.border.top},
                     bc});
    }
    if (d.border.bottom > 0) {
      out.push_back({CommandType::SolidRect,
                     {border.x, border.y + border.height - d.border.bottom,
                      border.width, d.border.bottom},
                     bc});
    }
  }

  for (const Layout::LayoutBox &child : box.children) {
    PaintBox(child, out);
  }
}

} // namespace

bool parseColor(const std::string &text, Color &out) {
  std::string s = ToLower(Trim(text));
  if (s.empty() || s == "transparent" || s == "none") {
    return false;
  }
  if (s[0] == '#') {
    return ParseHex(s, out);
  }
  if (s.rfind("rgb", 0) == 0) {
    return ParseRgbFunction(s, out);
  }
  if (s.rfind("hsl", 0) == 0) {
    return ParseHslFunction(s, out);
  }
  auto it = NamedColors().find(s);
  if (it != NamedColors().end()) {
    out = it->second;
    return true;
  }
  return false;
}

DisplayList buildDisplayList(const Layout::LayoutBox &root) {
  DisplayList list;
  PaintBox(root, list);
  return list;
}

Canvas::Canvas(int width, int height)
    : m_width(std::max(0, width)), m_height(std::max(0, height)),
      m_pixels(static_cast<size_t>(std::max(0, width)) *
                   static_cast<size_t>(std::max(0, height)),
               Color{0, 0, 0, 0}) {}

void Canvas::clear(Color color) {
  std::fill(m_pixels.begin(), m_pixels.end(), color);
}

Color Canvas::at(int x, int y) const {
  if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
    return Color{0, 0, 0, 0};
  }
  return m_pixels[static_cast<size_t>(y) * m_width + x];
}

void Canvas::fillRect(const Layout::Rect &rect, Color color) {
  // Round to pixel coordinates and clip to the canvas.
  int x0 = static_cast<int>(std::lround(rect.x));
  int y0 = static_cast<int>(std::lround(rect.y));
  int x1 = static_cast<int>(std::lround(rect.x + rect.width));
  int y1 = static_cast<int>(std::lround(rect.y + rect.height));
  x0 = std::clamp(x0, 0, m_width);
  y0 = std::clamp(y0, 0, m_height);
  x1 = std::clamp(x1, 0, m_width);
  y1 = std::clamp(y1, 0, m_height);

  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      Color &dst = m_pixels[static_cast<size_t>(y) * m_width + x];
      dst = BlendOver(dst, color);
    }
  }
}

void Canvas::blendPixel(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
    return;
  }
  Color &dst = m_pixels[static_cast<size_t>(y) * m_width + x];
  dst = BlendOver(dst, color);
}

void Canvas::paint(const DisplayList &list) {
  for (const DisplayCommand &cmd : list) {
    switch (cmd.type) {
    case CommandType::SolidRect:
      fillRect(cmd.rect, cmd.color);
      break;
    }
  }
}

std::vector<std::uint32_t> toPackedPixels(const Canvas &canvas) {
  const std::vector<Color> &px = canvas.pixels();
  std::vector<std::uint32_t> out(px.size());
  for (size_t i = 0; i < px.size(); ++i) {
    const Color &c = px[i];
    out[i] = (static_cast<std::uint32_t>(c.r) << 16) |
             (static_cast<std::uint32_t>(c.g) << 8) |
             static_cast<std::uint32_t>(c.b);
  }
  return out;
}

bool Canvas::savePPM(const std::string &path) const {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file << "P6\n" << m_width << " " << m_height << "\n255\n";
  for (const Color &c : m_pixels) {
    char rgb[3] = {static_cast<char>(c.r), static_cast<char>(c.g),
                   static_cast<char>(c.b)};
    file.write(rgb, 3);
  }
  return static_cast<bool>(file);
}

} // namespace Paint
} // namespace DesktopWebview
