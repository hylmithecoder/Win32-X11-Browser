#ifndef FONT_HPP
#define FONT_HPP

#include "Paint.hpp"

#include <string>
#include <vector>

namespace DesktopWebview {
namespace Font {

// Text rendering at a given pixel height. The default TrueType face is loaded
// from $DWV_FONT or assets/fonts/*.ttf (e.g. MiSans); otherwise a built-in 5x7
// bitmap fallback is scaled to approximate the requested height. Additional
// faces can be registered under a family name (e.g. from @font-face) and
// selected per call via `fontFamily`, a CSS font-family list ("Roboto",
// sans-serif): the first registered family wins, unknown/generic names fall
// back to the default face.

// Draw `text` with the top of its line box at (x, y), at `pixelHeight` pixels.
// Returns the advance width in pixels.
int drawText(Paint::Canvas &canvas, int x, int y, const std::string &text,
             Paint::Color color, int pixelHeight = 16,
             const std::string &fontFamily = "");

// Advance width / line height for `text` at `pixelHeight`.
int textWidth(const std::string &text, int pixelHeight = 16,
              const std::string &fontFamily = "");
int lineHeight(int pixelHeight = 16, const std::string &fontFamily = "");

// True when a TrueType face is loaded (vs the bitmap fallback).
bool usingTrueType();

// Explicitly load the default font from a file; returns true on success.
bool loadFont(const std::string &path);

// Register font file bytes (ttf/otf/ttc) under a family name so font-family
// can resolve to it. Returns false when the bytes are not a parseable font
// (e.g. woff2), leaving any previous registration for that family intact.
bool registerFont(const std::string &family, std::vector<unsigned char> data);

} // namespace Font
} // namespace DesktopWebview

#endif // FONT_HPP
