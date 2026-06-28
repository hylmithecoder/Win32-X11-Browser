#ifndef SVG_HPP
#define SVG_HPP

#include "Image.hpp"
#include "Paint.hpp"

#include <string>

namespace DesktopWebview {
namespace Svg {

// Options controlling how an SVG document is rasterised.
struct RenderOptions {
  // Output size in pixels. When 0, the SVG's own width/height (or viewBox) is
  // used, falling back to 300x150 (the SVG default).
  int width = 0;
  int height = 0;
  // Colour the canvas is cleared to before drawing (default transparent).
  Paint::Color background{0, 0, 0, 0};
};

// Rasterise an SVG document string into `out`. Supported elements: <g>, <rect>,
// <circle>, <ellipse>, <line>, <polyline>, <polygon>, and <path> (M/L/H/V/Z,
// absolute and relative). Supported presentation attributes: fill, stroke,
// stroke-width (named colours and #hex via Paint::parseColor). Curve/arc path
// commands, transforms, and viewBox scaling are out of scope for this stage.
//
// Returns false if the document cannot be parsed.
bool render(const std::string &svgText, Image::Bitmap &out,
            const RenderOptions &opts = RenderOptions{});

} // namespace Svg
} // namespace DesktopWebview

#endif // SVG_HPP
