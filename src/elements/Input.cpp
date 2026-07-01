#include "Input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace DesktopWebview {
namespace Elements {

namespace {

// --- Palette --------------------------------------------------------------
const Paint::Color kWhite{0xff, 0xff, 0xff, 0xff};
const Paint::Color kBlack{0x00, 0x00, 0x00, 0xff};
const Paint::Color kBorder{0x76, 0x76, 0x76, 0xff};
const Paint::Color kButtonBg{0xef, 0xef, 0xef, 0xff};
const Paint::Color kAccent{0x33, 0x66, 0xcc, 0xff};
const Paint::Color kFocus{0x4d, 0x90, 0xfe, 0xff};
const Paint::Color kPlaceholder{0x75, 0x75, 0x75, 0xff};
const Paint::Color kDisabledBg{0xea, 0xea, 0xea, 0xff};
const Paint::Color kDisabledText{0x8a, 0x8a, 0x8a, 0xff};

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Collapse runs of ASCII whitespace to single spaces and trim the ends.
std::string CollapseWhitespace(const std::string &s) {
  std::string out;
  bool inSpace = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      inSpace = true;
    } else {
      if (inSpace && !out.empty()) {
        out.push_back(' ');
      }
      inSpace = false;
      out.push_back(c);
    }
  }
  return out;
}

// --- Primitive drawing helpers -------------------------------------------
void FillRect(Paint::Canvas &c, int x, int y, int w, int h, Paint::Color col) {
  if (w <= 0 || h <= 0) {
    return;
  }
  c.fillRect(Layout::Rect{static_cast<float>(x), static_cast<float>(y),
                          static_cast<float>(w), static_cast<float>(h)},
             col);
}

// A 1px-wide rectangle outline.
void StrokeRect(Paint::Canvas &c, int x, int y, int w, int h, Paint::Color col) {
  if (w <= 0 || h <= 0) {
    return;
  }
  FillRect(c, x, y, w, 1, col);
  FillRect(c, x, y + h - 1, w, 1, col);
  FillRect(c, x, y, 1, h, col);
  FillRect(c, x + w - 1, y, 1, h, col);
}

// A filled square of side `t` centred on (px, py) -- the "pen" for thick lines.
void PenDot(Paint::Canvas &c, int px, int py, int t, Paint::Color col) {
  int half = t / 2;
  for (int dy = 0; dy < t; ++dy) {
    for (int dx = 0; dx < t; ++dx) {
      c.blendPixel(px - half + dx, py - half + dy, col);
    }
  }
}

// Bresenham line of thickness `t`.
void ThickLine(Paint::Canvas &c, int x0, int y0, int x1, int y1, int t,
               Paint::Color col) {
  int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    PenDot(c, x0, y0, t, col);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void FillCircle(Paint::Canvas &c, float cx, float cy, float r, Paint::Color col) {
  int x0 = static_cast<int>(std::floor(cx - r));
  int x1 = static_cast<int>(std::ceil(cx + r));
  int y0 = static_cast<int>(std::floor(cy - r));
  int y1 = static_cast<int>(std::ceil(cy + r));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      float ddx = x + 0.5f - cx, ddy = y + 0.5f - cy;
      if (ddx * ddx + ddy * ddy <= r * r) {
        c.blendPixel(x, y, col);
      }
    }
  }
}

void StrokeCircle(Paint::Canvas &c, float cx, float cy, float r,
                  Paint::Color col) {
  int x0 = static_cast<int>(std::floor(cx - r - 1));
  int x1 = static_cast<int>(std::ceil(cx + r + 1));
  int y0 = static_cast<int>(std::floor(cy - r - 1));
  int y1 = static_cast<int>(std::ceil(cy + r + 1));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      float ddx = x + 0.5f - cx, ddy = y + 0.5f - cy;
      float d = std::sqrt(ddx * ddx + ddy * ddy);
      if (d >= r - 1.0f && d <= r) {
        c.blendPixel(x, y, col);
      }
    }
  }
}

// A downward-pointing triangle (the <select> dropdown arrow) inside the box
// [x..x+w] x [y..y+h].
void DownTriangle(Paint::Canvas &c, int x, int y, int w, int h,
                  Paint::Color col) {
  for (int row = 0; row < h; ++row) {
    int inset = row * w / (2 * h);
    FillRect(c, x + inset, y + row, w - 2 * inset, 1, col);
  }
}

// A checkmark tick drawn inside the box [x..x+w] x [y..y+h].
void Checkmark(Paint::Canvas &c, int x, int y, int w, int h, Paint::Color col) {
  int t = std::max(1, w / 7);
  int x1 = x + static_cast<int>(w * 0.22f), y1 = y + static_cast<int>(h * 0.52f);
  int x2 = x + static_cast<int>(w * 0.42f), y2 = y + static_cast<int>(h * 0.72f);
  int x3 = x + static_cast<int>(w * 0.78f), y3 = y + static_cast<int>(h * 0.28f);
  ThickLine(c, x1, y1, x2, y2, t, col);
  ThickLine(c, x2, y2, x3, y3, t, col);
}

} // namespace

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

ControlKind classify(const Wrapper::Node &node) {
  if (!node.isElement()) {
    return ControlKind::NoControl;
  }
  std::string name = ToLower(node.name());
  if (name == "input") {
    std::string type = ToLower(node.attribute("type"));
    if (type == "hidden") {
      return ControlKind::Hidden;
    }
    if (type == "checkbox") {
      return ControlKind::Checkbox;
    }
    if (type == "radio") {
      return ControlKind::Radio;
    }
    if (type == "submit" || type == "button" || type == "reset") {
      return ControlKind::Button;
    }
    if (type == "password") {
      return ControlKind::Password;
    }
    return ControlKind::Text; // text/search/email/url/number/... and the default
  }
  if (name == "textarea") {
    return ControlKind::Textarea;
  }
  if (name == "select") {
    return ControlKind::Select;
  }
  if (name == "button") {
    return ControlKind::Button;
  }
  return ControlKind::NoControl;
}

bool isFormControl(const Wrapper::Node &node) {
  return classify(node) != ControlKind::NoControl;
}

bool isTextEntry(const Wrapper::Node &node) {
  ControlKind k = classify(node);
  return k == ControlKind::Text || k == ControlKind::Password ||
         k == ControlKind::Textarea;
}

// ---------------------------------------------------------------------------
// Display text
// ---------------------------------------------------------------------------

DisplayText displayText(const Wrapper::Node &node) {
  DisplayText out;
  switch (classify(node)) {
  case ControlKind::Button: {
    std::string t;
    if (ToLower(node.name()) == "button") {
      t = CollapseWhitespace(node.text());
    }
    if (t.empty()) {
      t = node.attribute("value");
    }
    if (t.empty()) {
      t = (ToLower(node.attribute("type")) == "reset") ? "Reset" : "Submit";
    }
    out.text = t;
    break;
  }
  case ControlKind::Select: {
    std::string first, selected;
    bool haveFirst = false, haveSelected = false;
    for (const Wrapper::Node &opt : node.getElementsByTagName("option")) {
      std::string ot = CollapseWhitespace(opt.text());
      if (!haveFirst) {
        first = ot;
        haveFirst = true;
      }
      if (opt.hasAttribute("selected")) {
        selected = ot;
        haveSelected = true;
        break;
      }
    }
    out.text = haveSelected ? selected : first;
    break;
  }
  case ControlKind::Text:
  case ControlKind::Password:
  case ControlKind::Textarea: {
    ControlKind k = classify(node);
    std::string val = (k == ControlKind::Textarea)
                          ? CollapseWhitespace(node.text())
                          : node.attribute("value");
    if (!val.empty()) {
      out.text =
          (k == ControlKind::Password) ? std::string(val.size(), '*') : val;
    } else {
      out.text = node.attribute("placeholder");
      out.placeholder = true;
    }
    break;
  }
  default:
    break; // checkbox / radio / hidden / none: no text
  }
  return out;
}

// ---------------------------------------------------------------------------
// Intrinsic sizing
// ---------------------------------------------------------------------------

Size intrinsicSize(const Wrapper::Node &node, int fontSize) {
  Size sz;
  ControlKind k = classify(node);
  if (k == ControlKind::NoControl || k == ControlKind::Hidden) {
    return sz;
  }
  if (fontSize <= 0) {
    fontSize = 16;
  }
  int lh = Font::lineHeight(fontSize);
  switch (k) {
  case ControlKind::Checkbox:
  case ControlKind::Radio:
    sz.width = sz.height = std::max(13, fontSize - 3);
    break;
  case ControlKind::Button: {
    sz.width = Font::textWidth(displayText(node).text, fontSize) + 22;
    sz.height = lh + 10;
    break;
  }
  case ControlKind::Textarea: {
    int rows = std::atoi(node.attribute("rows").c_str());
    int cols = std::atoi(node.attribute("cols").c_str());
    if (rows <= 0) {
      rows = 2;
    }
    if (cols <= 0) {
      cols = 20;
    }
    sz.width = cols * Font::textWidth("m", fontSize) + 10;
    sz.height = rows * lh + 8;
    break;
  }
  case ControlKind::Select: {
    sz.width = Font::textWidth(displayText(node).text, fontSize) + 30;
    sz.width = std::max(sz.width, 60);
    sz.height = lh + 12;
    break;
  }
  case ControlKind::Text:
  case ControlKind::Password:
  default: {
    int size = std::atoi(node.attribute("size").c_str());
    sz.width = (size > 0) ? size * Font::textWidth("0", fontSize) + 12 : 200;
    sz.height = lh + 12;
    break;
  }
  }
  return sz;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void paint(Paint::Canvas &canvas, const Wrapper::Node &node,
           const Layout::Rect &rect, int fontSize, bool focused) {
  ControlKind k = classify(node);
  if (k == ControlKind::NoControl || k == ControlKind::Hidden) {
    return;
  }
  if (fontSize <= 0) {
    fontSize = 16;
  }
  int x = static_cast<int>(rect.x), y = static_cast<int>(rect.y);
  int w = static_cast<int>(rect.width), h = static_cast<int>(rect.height);
  if (w <= 0 || h <= 0) {
    return;
  }
  bool disabled = node.hasAttribute("disabled");
  bool checked = node.hasAttribute("checked");
  int lh = Font::lineHeight(fontSize);

  switch (k) {
  case ControlKind::Checkbox: {
    if (checked) {
      FillRect(canvas, x, y, w, h, kAccent);
      Checkmark(canvas, x, y, w, h, kWhite);
      StrokeRect(canvas, x, y, w, h, kAccent);
    } else {
      FillRect(canvas, x, y, w, h, disabled ? kDisabledBg : kWhite);
      StrokeRect(canvas, x, y, w, h, kBorder);
    }
    break;
  }
  case ControlKind::Radio: {
    float cx = x + w / 2.0f, cy = y + h / 2.0f;
    float r = std::min(w, h) / 2.0f - 0.5f;
    FillCircle(canvas, cx, cy, r, disabled ? kDisabledBg : kWhite);
    StrokeCircle(canvas, cx, cy, r, checked ? kAccent : kBorder);
    if (checked) {
      FillCircle(canvas, cx, cy, r * 0.5f, kAccent);
    }
    break;
  }
  case ControlKind::Button: {
    FillRect(canvas, x, y, w, h, disabled ? kDisabledBg : kButtonBg);
    StrokeRect(canvas, x, y, w, h, kBorder);
    std::string label = displayText(node).text;
    int tw = Font::textWidth(label, fontSize);
    int tx = x + std::max(6, (w - tw) / 2);
    int ty = y + (h - lh) / 2;
    Font::drawText(canvas, tx, ty, label, disabled ? kDisabledText : kBlack,
                   fontSize);
    break;
  }
  case ControlKind::Select: {
    FillRect(canvas, x, y, w, h, disabled ? kDisabledBg : kWhite);
    StrokeRect(canvas, x, y, w, h, kBorder);
    std::string label = displayText(node).text;
    int ty = y + (h - lh) / 2;
    Font::drawText(canvas, x + 6, ty, label,
                   disabled ? kDisabledText : kBlack, fontSize);
    // Dropdown arrow on the right.
    int aw = 8, ah = 5;
    DownTriangle(canvas, x + w - aw - 6, y + (h - ah) / 2, aw, ah, kBorder);
    break;
  }
  case ControlKind::Text:
  case ControlKind::Password:
  case ControlKind::Textarea:
  default: {
    FillRect(canvas, x, y, w, h, disabled ? kDisabledBg : kWhite);
    StrokeRect(canvas, x, y, w, h, focused ? kFocus : kBorder);
    if (focused) {
      // Second inset line to make the focus ring read clearly.
      StrokeRect(canvas, x + 1, y + 1, w - 2, h - 2, kFocus);
    }
    DisplayText dt = displayText(node);
    Paint::Color tcol =
        disabled ? kDisabledText : (dt.placeholder ? kPlaceholder : kBlack);
    int ty = (k == ControlKind::Textarea) ? y + 4 : y + (h - lh) / 2;
    Font::drawText(canvas, x + 6, ty, dt.text, tcol, fontSize);
    if (focused && !disabled) {
      int caretX = x + 6 + (dt.placeholder ? 0 : Font::textWidth(dt.text, fontSize));
      FillRect(canvas, caretX + 1, ty, 1, lh, kBlack);
    }
    break;
  }
  }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

bool toggleCheckbox(Wrapper::Node &node) {
  bool now = !node.hasAttribute("checked");
  if (now) {
    node.setAttribute("checked", "checked");
  } else {
    node.removeAttribute("checked");
  }
  return now;
}

void selectRadio(Wrapper::Node &node, Wrapper::HtmlDocument &doc) {
  std::string group = node.attribute("name");
  for (Wrapper::Node &other : doc.getElementsByTagName("input")) {
    if (ToLower(other.attribute("type")) == "radio" &&
        other.attribute("name") == group) {
      other.removeAttribute("checked");
    }
  }
  node.setAttribute("checked", "checked");
}

bool cycleSelect(Wrapper::Node &node) {
  std::vector<Wrapper::Node> options = node.getElementsByTagName("option");
  if (options.empty()) {
    return false;
  }
  int selected = -1;
  for (int i = 0; i < static_cast<int>(options.size()); ++i) {
    if (options[i].hasAttribute("selected")) {
      selected = i;
      break;
    }
  }
  int next = (selected + 1) % static_cast<int>(options.size());
  for (int i = 0; i < static_cast<int>(options.size()); ++i) {
    if (i == next) {
      options[i].setAttribute("selected", "selected");
      node.setAttribute("value", options[i].attribute("value"));
    } else {
      options[i].removeAttribute("selected");
    }
  }
  return true;
}

void insertChar(Wrapper::Node &node, char ch) {
  if (classify(node) == ControlKind::Textarea) {
    node.setText(node.text() + std::string(1, ch));
  } else {
    node.setAttribute("value", node.attribute("value") + std::string(1, ch));
  }
}

bool backspace(Wrapper::Node &node) {
  if (classify(node) == ControlKind::Textarea) {
    std::string t = node.text();
    if (t.empty()) {
      return false;
    }
    t.pop_back();
    node.setText(t);
    return true;
  }
  std::string v = node.attribute("value");
  if (v.empty()) {
    return false;
  }
  v.pop_back();
  node.setAttribute("value", v);
  return true;
}

} // namespace Elements
} // namespace DesktopWebview
