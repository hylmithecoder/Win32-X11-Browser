#ifndef FONT_HPP
#define FONT_HPP

#include "Paint.hpp"

#include <string>

namespace DesktopWebview {
namespace Font {

// Text rendering at a given pixel height. A TrueType face is used when one can
// be loaded from $DWV_FONT or assets/fonts/*.ttf (e.g. MiSans); otherwise a
// built-in 5x7 bitmap fallback is scaled to approximate the requested height.

// Draw `text` with the top of its line box at (x, y), at `pixelHeight` pixels.
// Returns the advance width in pixels.
int drawText(Paint::Canvas &canvas, int x, int y, const std::string &text,
             Paint::Color color, int pixelHeight = 16);

// Advance width / line height for `text` at `pixelHeight`.
int textWidth(const std::string &text, int pixelHeight = 16);
int lineHeight(int pixelHeight = 16);

// True when a TrueType face is loaded (vs the bitmap fallback).
bool usingTrueType();

// Explicitly load a font file; returns true on success.
bool loadFont(const std::string &path);

} // namespace Font
} // namespace DesktopWebview

#endif // FONT_HPP
