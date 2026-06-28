#include "../include/Font.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <vector>

// Declarations only; implementation compiled in src/stb_impl.cpp.
#include "stb_truetype.h"

namespace DesktopWebview {
namespace Font {

namespace {

// Decode a UTF-8 string into Unicode code points (invalid bytes pass through as
// Latin-1).
std::vector<int> DecodeUtf8(const std::string &s) {
  std::vector<int> out;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    int cp = c;
    int extra = 0;
    if ((c & 0x80) == 0) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) {
      cp = (cp << 6) | (s[i] & 0x3F);
    }
    out.push_back(cp);
  }
  return out;
}

// ===========================================================================
// Bitmap fallback font (5x7 in an 8px cell)
// ===========================================================================
constexpr int kBmpGlyphW = 5;
constexpr int kBmpGlyphH = 7;
constexpr int kBmpCellW = 6;
constexpr int kBmpCellH = 8;

struct GlyphDef {
  char ch;
  std::array<const char *, 7> rows;
};

const GlyphDef kGlyphs[] = {
    {' ', {"     ", "     ", "     ", "     ", "     ", "     ", "     "}},
    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
    {'C', {" ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####"}},
    {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
    {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
    {'G', {" ####", "#    ", "#    ", "# ###", "#   #", "#   #", " ####"}},
    {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'I', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"}},
    {'J', {"  ###", "   # ", "   # ", "   # ", "#  # ", "#  # ", " ##  "}},
    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
    {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
    {'M', {"#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"}},
    {'N', {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}},
    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
    {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
    {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}},
    {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
    {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"}},
    {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
    {'3', {"#####", "    #", "   # ", "  ## ", "    #", "#   #", " ### "}},
    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
    {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
    {'6', {" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "}},
    {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
    {'9', {" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "}},
    {'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
    {',', {"     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   "}},
    {':', {"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "}},
    {';', {"     ", " ##  ", " ##  ", "     ", " ##  ", "  #  ", " #   "}},
    {'/', {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}},
    {'\\', {"#    ", "#    ", " #   ", "  #  ", "   # ", "    #", "    #"}},
    {'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
    {'_', {"     ", "     ", "     ", "     ", "     ", "     ", "#####"}},
    {'+', {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}},
    {'=', {"     ", "     ", "#####", "     ", "#####", "     ", "     "}},
    {'?', {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "}},
    {'!', {"  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "}},
    {'@', {" ### ", "#   #", "# ###", "# # #", "# ###", "#    ", " ### "}},
    {'#', {" # # ", " # # ", "#####", " # # ", "#####", " # # ", " # # "}},
    {'%', {"##  #", "##  #", "   # ", "  #  ", " #   ", "#  ##", "#  ##"}},
    {'&', {" ##  ", "#  # ", "#  # ", " ##  ", "# # #", "#  # ", " ## #"}},
    {'(', {"   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "}},
    {')', {" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "}},
    {'[', {" ### ", " #   ", " #   ", " #   ", " #   ", " #   ", " ### "}},
    {']', {" ### ", "   # ", "   # ", "   # ", "   # ", "   # ", " ### "}},
    {'~', {"     ", "     ", " #  #", "# ## ", "     ", "     ", "     "}},
    {'*', {"     ", "# #  ", " #   ", "###  ", " #   ", "# #  ", "     "}},
    {'<', {"   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "}},
    {'>', {" #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "}},
    {'"', {" # # ", " # # ", "     ", "     ", "     ", "     ", "     "}},
    {'\'', {"  #  ", "  #  ", "     ", "     ", "     ", "     ", "     "}},
    {'|', {"  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'^', {"  #  ", " # # ", "#   #", "     ", "     ", "     ", "     "}},
    {'$', {"  #  ", " ####", "# #  ", " ### ", "  # #", "#### ", "  #  "}},
    {'{', {"   ##", "  #  ", "  #  ", " #   ", "  #  ", "  #  ", "   ##"}},
    {'}', {"##   ", "  #  ", "  #  ", "   # ", "  #  ", "  #  ", "##   "}},
};

using BmpGlyph = std::array<std::uint8_t, kBmpGlyphH>;

const std::unordered_map<char, BmpGlyph> &BmpGlyphs() {
  static const std::unordered_map<char, BmpGlyph> table = [] {
    std::unordered_map<char, BmpGlyph> m;
    for (const GlyphDef &def : kGlyphs) {
      BmpGlyph g{};
      for (int r = 0; r < kBmpGlyphH; ++r) {
        std::uint8_t bits = 0;
        for (int c = 0; c < kBmpGlyphW; ++c) {
          if (def.rows[r][c] == '#') {
            bits |= static_cast<std::uint8_t>(1u << c);
          }
        }
        g[r] = bits;
      }
      m[def.ch] = g;
    }
    return m;
  }();
  return table;
}

const BmpGlyph &BmpGlyphFor(char ch) {
  static const BmpGlyph box = {0b11111, 0b10001, 0b10001, 0b10001,
                               0b10001, 0b10001, 0b11111};
  char up = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  const auto &table = BmpGlyphs();
  auto it = table.find(up);
  if (it != table.end()) {
    return it->second;
  }
  return box;
}

int BmpScale(int pixelHeight) {
  int s = static_cast<int>(std::lround(pixelHeight / double(kBmpCellH)));
  return std::max(1, s);
}

int BitmapDraw(Paint::Canvas &canvas, int x, int y, const std::string &text,
               Paint::Color color, int scale) {
  int penX = x;
  for (char ch : text) {
    const BmpGlyph &g = BmpGlyphFor(ch);
    for (int r = 0; r < kBmpGlyphH; ++r) {
      for (int c = 0; c < kBmpGlyphW; ++c) {
        if (g[r] & (1u << c)) {
          for (int sy = 0; sy < scale; ++sy) {
            for (int sx = 0; sx < scale; ++sx) {
              canvas.blendPixel(penX + c * scale + sx, y + r * scale + sy,
                                color);
            }
          }
        }
      }
    }
    penX += kBmpCellW * scale;
  }
  return penX - x;
}

// ===========================================================================
// TrueType state (stb_truetype)
// ===========================================================================
struct TtfState {
  bool attempted = false;
  bool loaded = false;
  std::vector<unsigned char> data;
  stbtt_fontinfo info{};
  int ascent = 0, descent = 0, lineGap = 0; // unscaled font units
};

TtfState &Ttf() {
  static TtfState s;
  return s;
}

bool TryLoad(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    return false;
  }
  TtfState &s = Ttf();
  s.data = std::move(bytes);
  int offset = stbtt_GetFontOffsetForIndex(s.data.data(), 0);
  if (offset < 0 ||
      !stbtt_InitFont(&s.info, s.data.data(), offset)) {
    s.data.clear();
    return false;
  }
  stbtt_GetFontVMetrics(&s.info, &s.ascent, &s.descent, &s.lineGap);
  s.loaded = true;
  return true;
}

// Pick a font file from a directory, preferring names containing "misans" then
// "regular".
std::string PickFontIn(const std::filesystem::path &dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return "";
  }
  std::string best;
  int bestScore = -1;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") {
      continue;
    }
    std::string name = entry.path().filename().string();
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    int score = 0;
    if (lower.find("misans") != std::string::npos) {
      score += 10;
    }
    if (lower.find("regular") != std::string::npos) {
      score += 2;
    }
    if (score > bestScore) {
      bestScore = score;
      best = entry.path().string();
    }
  }
  return best;
}

void EnsureInit() {
  TtfState &s = Ttf();
  if (s.attempted) {
    return;
  }
  s.attempted = true;

  if (const char *env = std::getenv("DWV_FONT")) {
    if (TryLoad(env)) {
      return;
    }
  }
  for (const char *dir : {"assets/fonts", "../assets/fonts",
                          "../../assets/fonts"}) {
    std::string path = PickFontIn(dir);
    if (!path.empty() && TryLoad(path)) {
      return;
    }
  }
}

} // namespace

bool loadFont(const std::string &path) {
  Ttf().attempted = true;
  Ttf().loaded = false;
  return TryLoad(path);
}

bool usingTrueType() {
  EnsureInit();
  return Ttf().loaded;
}

int drawText(Paint::Canvas &canvas, int x, int y, const std::string &text,
             Paint::Color color, int pixelHeight) {
  if (pixelHeight < 1) {
    pixelHeight = 1;
  }
  EnsureInit();
  TtfState &s = Ttf();
  if (!s.loaded) {
    return BitmapDraw(canvas, x, y, text, color, BmpScale(pixelHeight));
  }

  float scale = stbtt_ScaleForPixelHeight(&s.info, static_cast<float>(pixelHeight));
  int baseline = y + static_cast<int>(std::lround(s.ascent * scale));
  float penX = static_cast<float>(x);

  std::vector<int> cps = DecodeUtf8(text);
  int prev = 0;
  for (int cp : cps) {
    if (prev) {
      penX += stbtt_GetCodepointKernAdvance(&s.info, prev, cp) * scale;
    }
    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&s.info, scale, scale, cp, &gw,
                                                  &gh, &xoff, &yoff);
    if (bmp) {
      int gx = static_cast<int>(std::lround(penX)) + xoff;
      int gy = baseline + yoff;
      for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
          unsigned char cov = bmp[j * gw + i];
          if (cov) {
            Paint::Color c = color;
            c.a = static_cast<std::uint8_t>(color.a * cov / 255);
            canvas.blendPixel(gx + i, gy + j, c);
          }
        }
      }
      stbtt_FreeBitmap(bmp, nullptr);
    }
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&s.info, cp, &adv, &lsb);
    penX += adv * scale;
    prev = cp;
  }
  return static_cast<int>(std::lround(penX)) - x;
}

int textWidth(const std::string &text, int pixelHeight) {
  if (pixelHeight < 1) {
    pixelHeight = 1;
  }
  EnsureInit();
  TtfState &s = Ttf();
  if (!s.loaded) {
    return static_cast<int>(text.size()) * kBmpCellW * BmpScale(pixelHeight);
  }
  float scale = stbtt_ScaleForPixelHeight(&s.info, static_cast<float>(pixelHeight));
  float w = 0;
  int prev = 0;
  for (int cp : DecodeUtf8(text)) {
    if (prev) {
      w += stbtt_GetCodepointKernAdvance(&s.info, prev, cp) * scale;
    }
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&s.info, cp, &adv, &lsb);
    w += adv * scale;
    prev = cp;
  }
  return static_cast<int>(std::lround(w));
}

int lineHeight(int pixelHeight) {
  if (pixelHeight < 1) {
    pixelHeight = 1;
  }
  EnsureInit();
  TtfState &s = Ttf();
  if (!s.loaded) {
    return kBmpCellH * BmpScale(pixelHeight);
  }
  float scale = stbtt_ScaleForPixelHeight(&s.info, static_cast<float>(pixelHeight));
  return static_cast<int>(
      std::lround((s.ascent - s.descent + s.lineGap) * scale));
}

} // namespace Font
} // namespace DesktopWebview
