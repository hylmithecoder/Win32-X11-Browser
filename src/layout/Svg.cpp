#include "Svg.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace DesktopWebview {
namespace Svg {

namespace {

struct PointF {
  float x;
  float y;
};

// ---- raster primitives -----------------------------------------------------

// Even-odd scanline fill of a single (implicitly closed) polygon.
void FillPolygon(Paint::Canvas &canvas, const std::vector<PointF> &pts,
                 Paint::Color color) {
  if (pts.size() < 3) {
    return;
  }
  float minY = pts[0].y, maxY = pts[0].y;
  for (const PointF &p : pts) {
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  }
  int y0 = std::max(0, static_cast<int>(std::floor(minY)));
  int y1 = std::min(canvas.height() - 1, static_cast<int>(std::ceil(maxY)));

  std::vector<float> xs;
  for (int y = y0; y <= y1; ++y) {
    float sy = y + 0.5f;
    xs.clear();
    for (size_t i = 0; i < pts.size(); ++i) {
      const PointF &a = pts[i];
      const PointF &b = pts[(i + 1) % pts.size()];
      // Does edge a-b straddle scanline sy?
      bool aAbove = a.y <= sy;
      bool bAbove = b.y <= sy;
      if (aAbove != bAbove) {
        float t = (sy - a.y) / (b.y - a.y);
        xs.push_back(a.x + t * (b.x - a.x));
      }
    }
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2) {
      int xStart = static_cast<int>(std::ceil(xs[i] - 0.5f));
      int xEnd = static_cast<int>(std::floor(xs[i + 1] - 0.5f));
      for (int x = xStart; x <= xEnd; ++x) {
        canvas.blendPixel(x, y, color);
      }
    }
  }
}

// Plot a filled square brush of side ~`width` centred at (cx, cy).
void Brush(Paint::Canvas &canvas, float cx, float cy, float width,
           Paint::Color color) {
  int r = std::max(0, static_cast<int>(std::round(width / 2.0f - 0.5f)));
  int ix = static_cast<int>(std::round(cx));
  int iy = static_cast<int>(std::round(cy));
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      canvas.blendPixel(ix + dx, iy + dy, color);
    }
  }
}

// Stroke a polyline (optionally closed) with the given width via a brush walk.
void StrokePolyline(Paint::Canvas &canvas, const std::vector<PointF> &pts,
                    Paint::Color color, float width, bool closed) {
  if (pts.size() < 2) {
    return;
  }
  size_t segs = closed ? pts.size() : pts.size() - 1;
  for (size_t i = 0; i < segs; ++i) {
    const PointF &a = pts[i];
    const PointF &b = pts[(i + 1) % pts.size()];
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    int steps = std::max(1, static_cast<int>(std::ceil(len)));
    for (int s = 0; s <= steps; ++s) {
      float t = static_cast<float>(s) / steps;
      Brush(canvas, a.x + dx * t, a.y + dy * t, width, color);
    }
  }
}

// ---- attribute helpers -----------------------------------------------------

std::string GetAttr(xmlNode *node, const char *name) {
  xmlChar *v = xmlGetProp(node, reinterpret_cast<const xmlChar *>(name));
  if (!v) {
    return "";
  }
  std::string s(reinterpret_cast<const char *>(v));
  xmlFree(v);
  return s;
}

float ParseLength(const std::string &s, float fallback = 0.0f) {
  if (s.empty()) {
    return fallback;
  }
  const char *begin = s.c_str();
  char *end = nullptr;
  double v = std::strtod(begin, &end);
  if (end == begin) {
    return fallback;
  }
  return static_cast<float>(v); // ignore unit suffix (px/pt/...)
}

// Parse a flat list of numbers (comma/space/whitespace separated).
std::vector<float> ParseNumberList(const std::string &s) {
  std::vector<float> out;
  const char *p = s.c_str();
  while (*p) {
    while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) {
      ++p;
    }
    if (!*p) {
      break;
    }
    char *end = nullptr;
    double v = std::strtod(p, &end);
    if (end == p) {
      ++p; // skip an unparseable char
      continue;
    }
    out.push_back(static_cast<float>(v));
    p = end;
  }
  return out;
}

struct Style {
  Paint::Color fill{0, 0, 0, 255};
  bool hasFill = true;
  Paint::Color stroke{0, 0, 0, 255};
  bool hasStroke = false;
  float strokeWidth = 1.0f;
};

Style ResolveStyle(xmlNode *node, const Style &inherited) {
  Style st = inherited;
  std::string fill = GetAttr(node, "fill");
  if (!fill.empty()) {
    if (fill == "none") {
      st.hasFill = false;
    } else {
      Paint::Color c;
      if (Paint::parseColor(fill, c)) {
        st.fill = c;
        st.hasFill = true;
      }
    }
  }
  std::string stroke = GetAttr(node, "stroke");
  if (!stroke.empty()) {
    if (stroke == "none") {
      st.hasStroke = false;
    } else {
      Paint::Color c;
      if (Paint::parseColor(stroke, c)) {
        st.stroke = c;
        st.hasStroke = true;
      }
    }
  }
  std::string sw = GetAttr(node, "stroke-width");
  if (!sw.empty()) {
    st.strokeWidth = ParseLength(sw, st.strokeWidth);
  }
  return st;
}

void FillAndStroke(Paint::Canvas &canvas, const std::vector<PointF> &pts,
                   const Style &st, bool closed) {
  if (st.hasFill && pts.size() >= 3) {
    FillPolygon(canvas, pts, st.fill);
  }
  if (st.hasStroke && st.strokeWidth > 0) {
    StrokePolyline(canvas, pts, st.stroke, st.strokeWidth, closed);
  }
}

// Approximate an ellipse as a polygon.
std::vector<PointF> EllipsePolygon(float cx, float cy, float rx, float ry) {
  std::vector<PointF> pts;
  const int segments = 64;
  for (int i = 0; i < segments; ++i) {
    float a = (2.0f * 3.14159265358979f * i) / segments;
    pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
  }
  return pts;
}

// ---- path data (M/L/H/V/Z) -------------------------------------------------

// A path is parsed into one or more subpaths; `closed` tracks Z per subpath.
struct SubPath {
  std::vector<PointF> points;
  bool closed = false;
};

std::vector<SubPath> ParsePath(const std::string &d) {
  std::vector<SubPath> subs;
  SubPath cur;
  float cx = 0, cy = 0;
  float startX = 0, startY = 0;

  size_t i = 0;
  char cmd = 0;
  auto flush = [&]() {
    if (!cur.points.empty()) {
      subs.push_back(cur);
      cur = SubPath{};
    }
  };

  while (i < d.size()) {
    char ch = d[i];
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') {
      ++i;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(ch))) {
      cmd = ch;
      ++i;
      if (cmd == 'Z' || cmd == 'z') {
        cur.closed = true;
        cur.points.push_back({startX, startY});
        flush();
        cx = startX;
        cy = startY;
      }
      continue;
    }

    // Otherwise expect numbers for the current command.
    char *end = nullptr;
    double n = std::strtod(d.c_str() + i, &end);
    if (end == d.c_str() + i) {
      ++i;
      continue;
    }
    float v1 = static_cast<float>(n);
    i = static_cast<size_t>(end - d.c_str());

    auto readNum = [&](float &out) -> bool {
      while (i < d.size() &&
             (std::isspace(static_cast<unsigned char>(d[i])) || d[i] == ',')) {
        ++i;
      }
      char *e = nullptr;
      double m = std::strtod(d.c_str() + i, &e);
      if (e == d.c_str() + i) {
        return false;
      }
      out = static_cast<float>(m);
      i = static_cast<size_t>(e - d.c_str());
      return true;
    };

    switch (cmd) {
    case 'M':
    case 'm': {
      float y;
      if (!readNum(y)) {
        break;
      }
      flush();
      if (cmd == 'm') {
        cx += v1;
        cy += y;
      } else {
        cx = v1;
        cy = y;
      }
      startX = cx;
      startY = cy;
      cur.points.push_back({cx, cy});
      cmd = (cmd == 'm') ? 'l' : 'L'; // subsequent pairs are lineto
      break;
    }
    case 'L':
    case 'l': {
      float y;
      if (!readNum(y)) {
        break;
      }
      if (cmd == 'l') {
        cx += v1;
        cy += y;
      } else {
        cx = v1;
        cy = y;
      }
      cur.points.push_back({cx, cy});
      break;
    }
    case 'H':
    case 'h':
      cx = (cmd == 'h') ? cx + v1 : v1;
      cur.points.push_back({cx, cy});
      break;
    case 'V':
    case 'v':
      cy = (cmd == 'v') ? cy + v1 : v1;
      cur.points.push_back({cx, cy});
      break;
    default:
      // Unsupported command (curves/arcs): stop to avoid misparsing.
      flush();
      return subs;
    }
  }
  flush();
  return subs;
}

// ---- element dispatch ------------------------------------------------------

void RenderNode(Paint::Canvas &canvas, xmlNode *node, const Style &inherited) {
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) {
      continue;
    }
    std::string tag(reinterpret_cast<const char *>(n->name));
    Style st = ResolveStyle(n, inherited);

    if (tag == "g") {
      RenderNode(canvas, n->children, st);
    } else if (tag == "rect") {
      float x = ParseLength(GetAttr(n, "x"));
      float y = ParseLength(GetAttr(n, "y"));
      float w = ParseLength(GetAttr(n, "width"));
      float h = ParseLength(GetAttr(n, "height"));
      if (w > 0 && h > 0) {
        std::vector<PointF> pts = {
            {x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
        FillAndStroke(canvas, pts, st, true);
      }
    } else if (tag == "circle") {
      float cx = ParseLength(GetAttr(n, "cx"));
      float cy = ParseLength(GetAttr(n, "cy"));
      float r = ParseLength(GetAttr(n, "r"));
      if (r > 0) {
        FillAndStroke(canvas, EllipsePolygon(cx, cy, r, r), st, true);
      }
    } else if (tag == "ellipse") {
      float cx = ParseLength(GetAttr(n, "cx"));
      float cy = ParseLength(GetAttr(n, "cy"));
      float rx = ParseLength(GetAttr(n, "rx"));
      float ry = ParseLength(GetAttr(n, "ry"));
      if (rx > 0 && ry > 0) {
        FillAndStroke(canvas, EllipsePolygon(cx, cy, rx, ry), st, true);
      }
    } else if (tag == "line") {
      std::vector<PointF> pts = {
          {ParseLength(GetAttr(n, "x1")), ParseLength(GetAttr(n, "y1"))},
          {ParseLength(GetAttr(n, "x2")), ParseLength(GetAttr(n, "y2"))}};
      if (st.hasStroke) {
        StrokePolyline(canvas, pts, st.stroke, st.strokeWidth, false);
      }
    } else if (tag == "polygon" || tag == "polyline") {
      std::vector<float> nums = ParseNumberList(GetAttr(n, "points"));
      std::vector<PointF> pts;
      for (size_t k = 0; k + 1 < nums.size(); k += 2) {
        pts.push_back({nums[k], nums[k + 1]});
      }
      bool closed = tag == "polygon";
      FillAndStroke(canvas, pts, st, closed);
    } else if (tag == "path") {
      std::vector<SubPath> subs = ParsePath(GetAttr(n, "d"));
      for (const SubPath &sp : subs) {
        FillAndStroke(canvas, sp.points, st, sp.closed);
      }
    }

    // Recurse into children of container-like elements already handled above;
    // <svg> root children are entered by the top-level caller.
  }
}

} // namespace

bool render(const std::string &svgText, Image::Bitmap &out,
            const RenderOptions &opts) {
  xmlDocPtr doc = xmlReadMemory(
      svgText.c_str(), static_cast<int>(svgText.size()), "svg.xml", nullptr,
      XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET);
  if (!doc) {
    return false;
  }
  xmlNode *root = xmlDocGetRootElement(doc);
  if (!root) {
    xmlFreeDoc(doc);
    return false;
  }

  // Determine output size.
  int width = opts.width;
  int height = opts.height;
  if (width <= 0) {
    width = static_cast<int>(ParseLength(GetAttr(root, "width"), 0));
  }
  if (height <= 0) {
    height = static_cast<int>(ParseLength(GetAttr(root, "height"), 0));
  }
  if (width <= 0 || height <= 0) {
    std::vector<float> vb = ParseNumberList(GetAttr(root, "viewBox"));
    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) {
      if (width <= 0) {
        width = static_cast<int>(vb[2]);
      }
      if (height <= 0) {
        height = static_cast<int>(vb[3]);
      }
    }
  }
  if (width <= 0) {
    width = 300;
  }
  if (height <= 0) {
    height = 150;
  }

  Paint::Canvas canvas(width, height);
  canvas.clear(opts.background);

  Style base; // SVG default: fill black, no stroke
  RenderNode(canvas, root->children, base);

  out.width = width;
  out.height = height;
  out.pixels = canvas.pixels();

  xmlFreeDoc(doc);
  return out.valid();
}

} // namespace Svg
} // namespace DesktopWebview
