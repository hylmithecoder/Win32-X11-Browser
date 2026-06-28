#include "../include/Layout.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace DesktopWebview {
namespace Layout {

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

std::vector<std::string> SplitWhitespace(const std::string &s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) {
    out.push_back(tok);
  }
  return out;
}

// A parsed CSS length/keyword value.
struct Value {
  enum class Kind { Auto, Px, Percent, Keyword };
  Kind kind = Kind::Auto;
  float number = 0; // px amount, or percentage (0-100)
  std::string keyword;

  bool isAuto() const { return kind == Kind::Auto; }

  // Resolve to pixels. Percentages are taken relative to `base`; auto and
  // unrecognised keywords resolve to 0.
  float toPx(float base) const {
    switch (kind) {
    case Kind::Px:
      return number;
    case Kind::Percent:
      return number / 100.0f * base;
    default:
      return 0;
    }
  }
};

Value ParseValue(const std::string &raw) {
  Value v;
  std::string s = Trim(ToLower(raw));
  if (s.empty() || s == "auto") {
    v.kind = Value::Kind::Auto;
    return v;
  }
  const char *begin = s.c_str();
  char *end = nullptr;
  double num = std::strtod(begin, &end);
  if (end == begin) {
    // Non-numeric keyword (e.g. "bold", "solid").
    v.kind = Value::Kind::Keyword;
    v.keyword = s;
    return v;
  }
  v.number = static_cast<float>(num);
  std::string unit = Trim(std::string(end));
  if (unit == "%") {
    v.kind = Value::Kind::Percent;
  } else {
    // "px" and bare numbers are treated as pixels; other units are unsupported
    // in this MVP and fall back to pixels.
    v.kind = Value::Kind::Px;
  }
  return v;
}

// Expand a 1-4 value box shorthand (e.g. margin: "0 auto") into the four sides
// in CSS order: top, right, bottom, left.
std::array<Value, 4> ExpandShorthand(const std::string &shorthand) {
  std::array<Value, 4> edges; // default-constructed = auto
  std::vector<std::string> toks = SplitWhitespace(shorthand);
  if (toks.empty()) {
    // Padding/border default to 0, not auto; callers override as needed.
    return edges;
  }
  Value t = ParseValue(toks[0]);
  Value r = toks.size() > 1 ? ParseValue(toks[1]) : t;
  Value b = toks.size() > 2 ? ParseValue(toks[2]) : t;
  Value l = toks.size() > 3 ? ParseValue(toks[3]) : r;
  return {t, r, b, l};
}

// Pull the first length token out of a value (used to read a width out of a
// "border: 1px solid red" shorthand). Returns a 0px value if none is found.
Value FirstLength(const std::string &value) {
  for (const std::string &tok : SplitWhitespace(value)) {
    Value v = ParseValue(tok);
    if (v.kind == Value::Kind::Px || v.kind == Value::Kind::Percent) {
      return v;
    }
  }
  Value zero;
  zero.kind = Value::Kind::Px;
  zero.number = 0;
  return zero;
}

const std::array<const char *, 4> kSides = {"top", "right", "bottom", "left"};

// Resolve the four margin edges, honouring the `margin` shorthand and the
// per-side longhands (which take precedence). Auto is preserved.
std::array<Value, 4> ResolveMargin(const LayoutBox &box) {
  std::array<Value, 4> edges;
  std::string shorthand = box.value("margin");
  if (!shorthand.empty()) {
    edges = ExpandShorthand(shorthand);
  } else {
    for (Value &e : edges) {
      e.kind = Value::Kind::Px; // margins default to 0, not auto
      e.number = 0;
    }
  }
  for (size_t i = 0; i < kSides.size(); ++i) {
    std::string longhand = box.value(std::string("margin-") + kSides[i]);
    if (!longhand.empty()) {
      edges[i] = ParseValue(longhand);
    }
  }
  return edges;
}

// Resolve the four padding edges (default 0; auto is not meaningful and treated
// as 0 via toPx).
std::array<Value, 4> ResolvePadding(const LayoutBox &box) {
  std::array<Value, 4> edges;
  std::string shorthand = box.value("padding");
  if (!shorthand.empty()) {
    edges = ExpandShorthand(shorthand);
  } else {
    for (Value &e : edges) {
      e.kind = Value::Kind::Px;
      e.number = 0;
    }
  }
  for (size_t i = 0; i < kSides.size(); ++i) {
    std::string longhand = box.value(std::string("padding-") + kSides[i]);
    if (!longhand.empty()) {
      edges[i] = ParseValue(longhand);
    }
  }
  return edges;
}

// Resolve the four border widths. Reads `border-width`, the
// `border-<side>-width` longhands, and falls back to extracting a length from
// the `border` / `border-<side>` shorthands (e.g. "1px solid red").
std::array<Value, 4> ResolveBorder(const LayoutBox &box) {
  std::array<Value, 4> edges;
  for (Value &e : edges) {
    e.kind = Value::Kind::Px;
    e.number = 0;
  }

  std::string borderShorthand = box.value("border");
  if (!borderShorthand.empty()) {
    Value v = FirstLength(borderShorthand);
    edges = {v, v, v, v};
  }
  std::string widthShorthand = box.value("border-width");
  if (!widthShorthand.empty()) {
    edges = ExpandShorthand(widthShorthand);
  }
  for (size_t i = 0; i < kSides.size(); ++i) {
    std::string side = kSides[i];
    std::string perSide = box.value("border-" + side);
    if (!perSide.empty()) {
      edges[i] = FirstLength(perSide);
    }
    std::string perSideWidth = box.value("border-" + side + "-width");
    if (!perSideWidth.empty()) {
      edges[i] = ParseValue(perSideWidth);
    }
  }
  return edges;
}

// User-agent default `display` for a subset of HTML tags.
std::string DefaultDisplay(const std::string &tag) {
  static const std::vector<std::string> block = {
      "html",  "body",    "div",     "p",          "h1",         "h2",
      "h3",    "h4",      "h5",      "h6",         "ul",         "ol",
      "li",    "section", "article", "header",     "footer",     "nav",
      "main",  "aside",   "figure",  "figcaption", "blockquote", "pre",
      "table", "form",    "hr",      "address",    "dl",         "dt",
      "dd",    "fieldset"};
  static const std::vector<std::string> none = {"head", "title",    "meta",
                                                "link", "style",    "script",
                                                "base", "noscript", "template"};

  std::string t = ToLower(tag);
  if (std::find(none.begin(), none.end(), t) != none.end()) {
    return "none";
  }
  if (std::find(block.begin(), block.end(), t) != block.end()) {
    return "block";
  }
  return "inline";
}

// ---- box tree construction -------------------------------------------------

LayoutBox &InlineContainer(LayoutBox &box) {
  if (box.type == BoxType::Inline || box.type == BoxType::Anonymous) {
    return box;
  }
  // Block box: append inline children into a trailing anonymous block box.
  if (box.children.empty() || box.children.back().type != BoxType::Anonymous) {
    LayoutBox anon;
    anon.type = BoxType::Anonymous;
    box.children.push_back(anon);
  }
  return box.children.back();
}

LayoutBox BuildLayoutTree(const StyledNode &sn) {
  LayoutBox box;
  box.node = &sn;
  box.type = sn.display() == "inline" ? BoxType::Inline : BoxType::Block;

  for (const StyledNode &child : sn.children) {
    std::string disp = child.display();
    if (disp == "none") {
      continue;
    }
    if (disp == "inline") {
      InlineContainer(box).children.push_back(BuildLayoutTree(child));
    } else {
      box.children.push_back(BuildLayoutTree(child));
    }
  }
  return box;
}

// ---- block layout ----------------------------------------------------------

void LayoutBlock(LayoutBox &box, const Dimensions &containing);

void Layout(LayoutBox &box, const Dimensions &containing) {
  // MVP: inline and anonymous boxes use the block algorithm (vertical stack).
  LayoutBlock(box, containing);
}

void CalculateBlockWidth(LayoutBox &box, const Dimensions &containing) {
  Dimensions &d = box.dimensions;
  const float cb = containing.content.width;

  Value width = ParseValue(box.value("width"));

  std::array<Value, 4> margin = ResolveMargin(box);
  std::array<Value, 4> padding = ResolvePadding(box);
  std::array<Value, 4> border = ResolveBorder(box);

  // Indices: 0 top, 1 right, 2 bottom, 3 left.
  Value &mlV = margin[3];
  Value &mrV = margin[1];

  float ml = mlV.toPx(cb);
  float mr = mrV.toPx(cb);
  float pl = padding[3].toPx(cb);
  float pr = padding[1].toPx(cb);
  float bl = border[3].toPx(cb);
  float br = border[1].toPx(cb);
  float w = width.toPx(cb);

  float total = (width.isAuto() ? 0 : w) + ml + mr + pl + pr + bl + br;

  // If the width is fixed and the total is too wide, auto margins collapse to
  // 0.
  if (!width.isAuto() && total > cb) {
    if (mlV.isAuto()) {
      ml = 0;
      mlV.kind = Value::Kind::Px;
    }
    if (mrV.isAuto()) {
      mr = 0;
      mrV.kind = Value::Kind::Px;
    }
  }

  float underflow = cb - total;
  bool wa = width.isAuto();
  bool mla = mlV.isAuto();
  bool mra = mrV.isAuto();

  if (!wa) {
    if (!mla && !mra) {
      // Over-constrained: adjust the right margin.
      mr += underflow;
    } else if (!mla && mra) {
      mr = underflow;
    } else if (mla && !mra) {
      ml = underflow;
    } else { // both margins auto -> centre
      ml = underflow / 2.0f;
      mr = underflow / 2.0f;
    }
  } else {
    // Width auto absorbs the underflow; auto margins become 0.
    if (mla) {
      ml = 0;
    }
    if (mra) {
      mr = 0;
    }
    if (underflow >= 0) {
      w = underflow;
    } else {
      w = 0;
      mr += underflow;
    }
  }

  d.content.width = w;
  d.padding.left = pl;
  d.padding.right = pr;
  d.border.left = bl;
  d.border.right = br;
  d.margin.left = ml;
  d.margin.right = mr;

  // Vertical edges do not affect width resolution but are set here.
  d.padding.top = padding[0].toPx(cb);
  d.padding.bottom = padding[2].toPx(cb);
  d.border.top = border[0].toPx(cb);
  d.border.bottom = border[2].toPx(cb);
  d.margin.top = margin[0].toPx(cb);
  d.margin.bottom = margin[2].toPx(cb);
}

void CalculateBlockPosition(LayoutBox &box, const Dimensions &containing) {
  Dimensions &d = box.dimensions;
  // Stack below previously laid-out siblings: containing.content.height is the
  // running offset accumulated by the parent.
  d.content.x =
      containing.content.x + d.margin.left + d.border.left + d.padding.left;
  d.content.y = containing.content.y + containing.content.height +
                d.margin.top + d.border.top + d.padding.top;
}

void LayoutBlockChildren(LayoutBox &box) {
  Dimensions &d = box.dimensions;
  for (LayoutBox &child : box.children) {
    Layout(child, d);
    // Grow this box to enclose the child's full margin box.
    d.content.height += child.dimensions.marginBox().height;
  }
}

void CalculateBlockHeight(LayoutBox &box) {
  // An explicit pixel height overrides the height derived from children.
  Value h = ParseValue(box.value("height"));
  if (h.kind == Value::Kind::Px) {
    box.dimensions.content.height = h.number;
  }
}

void LayoutBlock(LayoutBox &box, const Dimensions &containing) {
  CalculateBlockWidth(box, containing);
  CalculateBlockPosition(box, containing);
  LayoutBlockChildren(box);
  CalculateBlockHeight(box);
}

void PrintBox(const LayoutBox &box, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }
  const char *type = box.type == BoxType::Block    ? "block"
                     : box.type == BoxType::Inline ? "inline"
                                                   : "anonymous";
  std::string tag =
      box.node && box.node->node.isElement() ? box.node->node.name() : "-";
  const Rect &c = box.dimensions.content;
  std::cout << type << " <" << tag << "> content(x=" << c.x << ", y=" << c.y
            << ", w=" << c.width << ", h=" << c.height << ")" << std::endl;
  for (const LayoutBox &child : box.children) {
    PrintBox(child, depth + 1);
  }
}

} // namespace

// ---- Dimensions helpers ----------------------------------------------------

namespace {
Rect ExpandRect(const Rect &r, const EdgeSizes &e) {
  return Rect{r.x - e.left, r.y - e.top, r.width + e.left + e.right,
              r.height + e.top + e.bottom};
}
} // namespace

Rect Dimensions::paddingBox() const { return ExpandRect(content, padding); }
Rect Dimensions::borderBox() const { return ExpandRect(paddingBox(), border); }
Rect Dimensions::marginBox() const { return ExpandRect(borderBox(), margin); }

// ---- StyledNode / LayoutBox accessors --------------------------------------

std::string StyledNode::value(const std::string &property) const {
  auto it = styles.find(property);
  return it == styles.end() ? "" : it->second;
}

std::string StyledNode::display() const {
  auto it = styles.find("display");
  if (it != styles.end() && !it->second.empty()) {
    return ToLower(it->second);
  }
  return node.isElement() ? DefaultDisplay(node.name()) : "inline";
}

std::string LayoutBox::value(const std::string &property) const {
  return node ? node->value(property) : "";
}

// ---- public entry points ---------------------------------------------------

StyledNode styleTree(const Wrapper::Node &domRoot,
                     const Css::Stylesheet &sheet) {
  StyledNode sn;
  sn.node = domRoot;
  sn.styles = Css::computeStyle(sheet, domRoot);
  for (const Wrapper::Node &child : domRoot.children()) {
    sn.children.push_back(styleTree(child, sheet));
  }
  return sn;
}

LayoutBox layout(const StyledNode &styleRoot, float viewportWidth,
                 float viewportHeight) {
  Dimensions viewport;
  viewport.content.x = 0;
  viewport.content.y = 0;
  viewport.content.width = viewportWidth;
  viewport.content.height = 0; // running offset; root stacks from y = 0
  (void)viewportHeight;        // reserved for percentage-height resolution

  LayoutBox root = BuildLayoutTree(styleRoot);
  Layout(root, viewport);
  return root;
}

void printLayoutTree(const LayoutBox &box) { PrintBox(box, 0); }

} // namespace Layout
} // namespace DesktopWebview
