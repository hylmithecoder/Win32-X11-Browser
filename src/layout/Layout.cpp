#include "Layout.hpp"
#include "Font.hpp"
#include "HandlerCssVariable.hpp"
#include "Input.hpp"

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
  } else if (unit == "rem" || unit == "em") {
    v.kind = Value::Kind::Px;
    v.number = v.number * 16.0f;
  } else {
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
      "html",  "body",     "div",     "p",          "h1",         "h2",
      "h3",    "h4",       "h5",      "h6",         "ul",         "ol",
      "li",    "section",  "article", "header",     "footer",     "nav",
      "main",  "aside",    "figure",  "figcaption", "blockquote", "pre",
      "table", "form",     "hr",      "address",    "dl",         "dt",
      "dd",    "fieldset", "thead",   "tbody",      "tfoot",      "caption"};
  static const std::vector<std::string> none = {"head", "title",    "meta",
                                                "link", "style",    "script",
                                                "base", "noscript", "template"};

  std::string t = ToLower(tag);
  if (std::find(none.begin(), none.end(), t) != none.end()) {
    return "none";
  }
  // Table rows use flex so that <td>/<th> cells lay out horizontally.
  if (t == "tr") {
    return "flex";
  }
  // Table cells and header cells are block containers.
  if (t == "td" || t == "th") {
    return "block";
  }
  if (std::find(block.begin(), block.end(), t) != block.end()) {
    return "block";
  }
  return "inline";
}

// ---- box tree construction -------------------------------------------------

// Defined below, alongside the rest of the text/font-size measurement code;
// forward-declared here since BuildLayoutTree (which needs them) comes first.
std::string CollapseInlineWhitespace(const std::string &s);
int ResolveOwnFontSize(const StyledNode &sn, int inherited);

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

// Strip exactly one leading/trailing collapsed space from the text-run boxes
// at the edges of an inline formatting context. CSS strips whitespace
// adjacent to the start/end of a line's inline content; interior whitespace
// between inline siblings is left as the single separating space
// CollapseInlineWhitespace already produced.
void TrimInlineEdges(std::vector<LayoutBox> &list) {
  if (!list.empty()) {
    LayoutBox &front = list.front();
    if (front.node && front.node->node.isText() && !front.text.empty() &&
        front.text.front() == ' ') {
      front.text.erase(front.text.begin());
    }
  }
  if (!list.empty()) {
    LayoutBox &back = list.back();
    if (back.node && back.node->node.isText() && !back.text.empty() &&
        back.text.back() == ' ') {
      back.text.pop_back();
    }
  }
}

LayoutBox BuildLayoutTree(const StyledNode &sn, int inheritedFontSize) {
  LayoutBox box;
  box.node = &sn;
  box.fontSize = ResolveOwnFontSize(sn, inheritedFontSize);
  if (sn.node.isText()) {
    box.text = CollapseInlineWhitespace(sn.node.text());
  }
  std::string disp = sn.display();
  box.type = (disp == "inline" || disp == "inline-block") ? BoxType::Inline
                                                          : BoxType::Block;

  for (const StyledNode &child : sn.children) {
    std::string childDisp = child.display();
    if (childDisp == "none") {
      continue;
    }
    // display: contents — transparent container; children become siblings.
    if (childDisp == "contents") {
      for (const StyledNode &gc : child.children) {
        std::string gd = gc.display();
        if (gd == "none") {
          continue;
        }
        if (gd == "inline" || gd == "inline-block") {
          InlineContainer(box).children.push_back(
              BuildLayoutTree(gc, box.fontSize));
        } else {
          box.children.push_back(BuildLayoutTree(gc, box.fontSize));
        }
      }
      continue;
    }
    if (childDisp == "inline" || childDisp == "inline-block") {
      InlineContainer(box).children.push_back(
          BuildLayoutTree(child, box.fontSize));
    } else {
      box.children.push_back(BuildLayoutTree(child, box.fontSize));
    }
  }

  // Trim the inline formatting context(s) this box just assembled: its own
  // children directly if it is itself inline, or each anonymous wrapper's
  // children if it is a block box that absorbed inline/text runs.
  if (box.type == BoxType::Block) {
    for (LayoutBox &child : box.children) {
      if (child.type == BoxType::Anonymous) {
        TrimInlineEdges(child.children);
      }
    }
  } else {
    TrimInlineEdges(box.children);
  }
  return box;
}

// ---- block layout ----------------------------------------------------------

void LayoutBlock(LayoutBox &box, const Dimensions &containing);
void LayoutFlex(LayoutBox &box, const Dimensions &containing);
void LayoutGrid(LayoutBox &box, const Dimensions &containing);

void LayoutInlineContainer(LayoutBox &box, const Dimensions &containing);
bool HasInlineChildren(const LayoutBox &box);

void Layout(LayoutBox &box, const Dimensions &containing) {
  // Dispatch on the box's `display`. Flex and grid get dedicated algorithms;
  // everything else (block/inline/anonymous) uses the block (vertical stack)
  // algorithm.
  std::string disp = box.node ? box.node->display() : "block";
  if (disp == "flex" || disp == "inline-flex") {
    LayoutFlex(box, containing);
  } else if (disp == "grid" || disp == "inline-grid") {
    LayoutGrid(box, containing);
  } else if (box.type == BoxType::Anonymous || HasInlineChildren(box)) {
    LayoutInlineContainer(box, containing);
  } else {
    LayoutBlock(box, containing);
  }
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

  Value maxWidth = ParseValue(box.value("max-width"));
  if (maxWidth.kind != Value::Kind::Auto) {
    float maxW = maxWidth.toPx(cb);
    if (w > maxW) {
      w = maxW;
      float newTotal = w + ml + mr + pl + pr + bl + br;
      float newUnderflow = cb - newTotal;
      if (!mla && !mra) {
        mr += newUnderflow;
      } else if (!mla && mra) {
        mr = newUnderflow;
      } else if (mla && !mra) {
        ml = newUnderflow;
      } else {
        ml = newUnderflow / 2.0f;
        mr = newUnderflow / 2.0f;
      }
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

// ---- inline layout ---------------------------------------------------------

bool HasInlineChildren(const LayoutBox &box) {
  for (const LayoutBox &child : box.children) {
    if (child.type == BoxType::Inline) {
      return true;
    }
  }
  return false;
}

std::string CollapseWhitespace(const std::string &s) {
  std::string out;
  bool inSpace = true; // skip leading space
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(c);
      inSpace = false;
    }
  }
  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

// Like CollapseWhitespace, but does not trim the ends: runs of whitespace
// collapse to a single space, keeping up to one leading/trailing space. Used
// for bare DOM text nodes interleaved with inline elements (e.g. "Click " in
// "Click <a>here</a>"), where a leading/trailing space is exactly the
// separator that must survive between this run and its neighbours. The
// caller trims the true edges of each inline formatting context afterwards
// (see TrimInlineEdges).
std::string CollapseInlineWhitespace(const std::string &s) {
  std::string out;
  bool inSpace = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(c);
      inSpace = false;
    }
  }
  return out;
}

// Effective font-size (px) a box should use, already resolved top-down while
// the tree was built (see BuildLayoutTree). Inheritance/em/% resolution
// happens once per box at build time instead of being re-derived here.
int GetFontSizeForBox(const LayoutBox &box) { return box.fontSize; }

// This node's own font-size in px, given the inherited (parent's resolved)
// size. Honours px and em/% (relative to `inherited`); unset/unparseable
// falls back to `inherited`.
int ResolveOwnFontSize(const StyledNode &sn, int inherited) {
  auto it = sn.styles.find("font-size");
  if (it == sn.styles.end() || it->second.empty()) {
    return inherited;
  }
  const std::string &fs = it->second;
  const char *start = fs.c_str();
  char *end = nullptr;
  double val = std::strtod(start, &end);
  if (end == start) {
    return inherited;
  }
  std::string unit = Trim(std::string(end));
  if (unit == "%") {
    return static_cast<int>(val / 100.0 * inherited);
  }
  if (unit == "em") {
    return static_cast<int>(val * inherited);
  }
  return static_cast<int>(val); // px (and unrecognised units treated as px)
}

void CalculateInlineWidth(LayoutBox &box, const Dimensions &containing);

void CalculateInlineWidth(LayoutBox &box, const Dimensions &containing) {
  Dimensions &d = box.dimensions;
  const float cb = containing.content.width;

  Value width = ParseValue(box.value("width"));

  std::array<Value, 4> margin = ResolveMargin(box);
  std::array<Value, 4> padding = ResolvePadding(box);
  std::array<Value, 4> border = ResolveBorder(box);

  float ml = margin[3].toPx(cb);
  float mr = margin[1].toPx(cb);
  float pl = padding[3].toPx(cb);
  float pr = padding[1].toPx(cb);
  float bl = border[3].toPx(cb);
  float br = border[1].toPx(cb);
  float w = 0;

  if (width.kind == Value::Kind::Px) {
    w = width.number;
  } else if (width.kind == Value::Kind::Percent) {
    w = width.toPx(cb);
  } else {
    if (box.children.empty()) {
      if (box.node) {
        // A bare text-run leaf already carries its final (edge-trimmed)
        // string in box.text; an element leaf derives it from the DOM node.
        std::string text = box.node->node.isText()
                               ? box.text
                               : CollapseWhitespace(box.node->node.text());
        int fontSize = GetFontSizeForBox(box);
        w = static_cast<float>(Font::textWidth(text, fontSize));

        // Form controls are replaced elements; their intrinsic width comes from
        // the Elements module (the single source of truth for control sizing).
        if (Elements::isFormControl(box.node->node)) {
          w = static_cast<float>(
              Elements::intrinsicSize(box.node->node, fontSize).width);
        }
      }
    } else {
      float totalChildrenWidth = 0;
      for (const LayoutBox &child : box.children) {
        LayoutBox childCopy = child;
        CalculateInlineWidth(childCopy, containing);
        totalChildrenWidth += childCopy.dimensions.marginBox().width;
      }
      w = totalChildrenWidth;
    }
  }

  d.content.width = w;
  d.padding.left = pl;
  d.padding.right = pr;
  d.border.left = bl;
  d.border.right = br;
  d.margin.left = ml;
  d.margin.right = mr;

  d.padding.top = padding[0].toPx(cb);
  d.padding.bottom = padding[2].toPx(cb);
  d.border.top = border[0].toPx(cb);
  d.border.bottom = border[2].toPx(cb);
  d.margin.top = margin[0].toPx(cb);
  d.margin.bottom = margin[2].toPx(cb);
}

void CalculateInlineHeight(LayoutBox &box) {
  Dimensions &d = box.dimensions;
  Value height = ParseValue(box.value("height"));

  if (height.kind == Value::Kind::Px) {
    d.content.height = height.number;
    return;
  }

  float h = 0;
  if (box.children.empty()) {
    if (box.node) {
      if (box.node->node.isText() && box.text.empty()) {
        // Whitespace collapsed away entirely at this position; a line box
        // with no atomic inline content contributes no height.
        h = 0;
      } else {
        int fontSize = GetFontSizeForBox(box);
        h = static_cast<float>(Font::lineHeight(fontSize));

        if (Elements::isFormControl(box.node->node)) {
          h = static_cast<float>(
              Elements::intrinsicSize(box.node->node, fontSize).height);
        }
      }
    }
  } else {
    for (const LayoutBox &child : box.children) {
      h = std::max(h, child.dimensions.marginBox().height);
    }
  }
  d.content.height = h;
}
void LayoutOwnChildren(LayoutBox &box);

void LayoutInlineChildren(LayoutBox &box) {
  Dimensions &d = box.dimensions;
  float curX = d.content.x;
  float curY = d.content.y;
  float lineHeight = 0;

  for (LayoutBox &child : box.children) {
    CalculateInlineWidth(child, d);

    float childW = child.dimensions.marginBox().width;

    if (curX + childW > d.content.x + d.content.width && curX > d.content.x) {
      curX = d.content.x;
      curY += lineHeight;
      lineHeight = 0;
    }

    child.dimensions.content.x = curX + child.dimensions.margin.left +
                                 child.dimensions.border.left +
                                 child.dimensions.padding.left;
    child.dimensions.content.y = curY + child.dimensions.margin.top +
                                 child.dimensions.border.top +
                                 child.dimensions.padding.top;

    LayoutOwnChildren(child);
    CalculateInlineHeight(child);

    curX += childW;
    lineHeight = std::max(lineHeight, child.dimensions.marginBox().height);
  }

  d.content.height = (curY + lineHeight) - d.content.y;
}

void LayoutInlineContainer(LayoutBox &box, const Dimensions &containing) {
  CalculateBlockWidth(box, containing);
  CalculateBlockPosition(box, containing);
  LayoutInlineChildren(box);
  CalculateBlockHeight(box);
}

// ---- flex / grid layout ----------------------------------------------------

// Resolve an item's margin/border/padding edges (in px, against `cb`) directly
// into its Dimensions, treating auto as 0. Used by the flex/grid item placement
// which positions boxes explicitly rather than via block underflow rules.
void SetItemEdges(LayoutBox &box, float cb) {
  std::array<Value, 4> m = ResolveMargin(box);
  std::array<Value, 4> b = ResolveBorder(box);
  std::array<Value, 4> p = ResolvePadding(box);
  auto px = [&](const Value &v) { return v.isAuto() ? 0.0f : v.toPx(cb); };
  Dimensions &d = box.dimensions;
  // EdgeSizes order: left, right, top, bottom. Resolve* order:
  // top,right,bot,left
  d.margin = {px(m[3]), px(m[1]), px(m[0]), px(m[2])};
  d.border = {px(b[3]), px(b[1]), px(b[0]), px(b[2])};
  d.padding = {px(p[3]), px(p[1]), px(p[0]), px(p[2])};
}

// Child-layout phases, factored out so a flex/grid item that is itself a
// flex/grid container lays its own children out correctly (these assume the
// box's content rect x/y/width are already set).
void GridChildren(LayoutBox &box);
void FlexChildren(LayoutBox &box);

// Lay out a box's children according to its own `display`, then resolve its
// height. Assumes box.dimensions.content {x,y,width} are already established.
void LayoutInlineChildren(LayoutBox &box);

void LayoutOwnChildren(LayoutBox &box) {
  std::string disp = box.node ? box.node->display() : "block";
  box.dimensions.content.height = 0;
  if (disp == "flex" || disp == "inline-flex") {
    FlexChildren(box);
  } else if (disp == "grid" || disp == "inline-grid") {
    GridChildren(box);
  } else if (box.type == BoxType::Anonymous || HasInlineChildren(box)) {
    LayoutInlineChildren(box);
  } else {
    LayoutBlockChildren(box);
  }
  CalculateBlockHeight(box); // explicit pixel height overrides.
}

// Place a flex/grid item whose available outer (margin-box) width is `cellW`,
// with its margin-box top-left at (cellX, cellY). An explicit width wins;
// otherwise the item stretches to fill the cell. `cb` is the container content
// width (for resolving percentages). The item then lays out its own children
// per its display (so nested flex/grid works).
void LayoutItemInCell(LayoutBox &item, float cellX, float cellY, float cellW,
                      float cb) {
  SetItemEdges(item, cb);
  Dimensions &d = item.dimensions;
  float horiz = d.margin.left + d.margin.right + d.border.left +
                d.border.right + d.padding.left + d.padding.right;

  Value w = ParseValue(item.value("width"));
  float contentW;
  if (w.kind == Value::Kind::Px) {
    contentW = w.number;
  } else if (w.kind == Value::Kind::Percent) {
    contentW = w.toPx(cb);
  } else {
    contentW = std::max(0.0f, cellW - horiz); // auto stretches to the cell
  }

  Value maxWidth = ParseValue(item.value("max-width"));
  if (maxWidth.kind != Value::Kind::Auto) {
    float maxW = maxWidth.toPx(cb);
    if (contentW > maxW) {
      contentW = maxW;
    }
  }

  d.content.width = contentW;
  d.content.x = cellX + d.margin.left + d.border.left + d.padding.left;
  d.content.y = cellY + d.margin.top + d.border.top + d.padding.top;

  LayoutOwnChildren(item);
}

// Collect the in-flow item boxes of a flex/grid container (skip anonymous
// whitespace boxes that have no DOM node).
std::vector<LayoutBox *> FlowItems(LayoutBox &box) {
  std::vector<LayoutBox *> items;
  for (LayoutBox &ch : box.children) {
    if (ch.node) {
      items.push_back(&ch);
    }
  }
  return items;
}

// Parse a `grid-template-columns` track list. Recognises `auto`, `<n>fr`,
// lengths and percentages. `repeat(...)`/`minmax(...)` are not expanded.
std::vector<Value> ParseTrackList(const std::string &s) {
  std::vector<Value> out;
  for (const std::string &tok : SplitWhitespace(s)) {
    std::string t = ToLower(tok);
    Value v;
    if (t == "auto") {
      v.kind = Value::Kind::Auto;
    } else if (t.size() > 2 && t.substr(t.size() - 2) == "fr") {
      v.kind = Value::Kind::Keyword;
      v.keyword = "fr";
      v.number = static_cast<float>(std::atof(t.c_str()));
    } else {
      v = ParseValue(tok);
    }
    out.push_back(v);
  }
  return out;
}

// Resolve column/row gaps from the `gap`/`grid-gap` shorthand and the
// row-/column- longhands.
void ResolveGaps(const LayoutBox &box, float cw, float &rowGap, float &colGap) {
  rowGap = colGap = 0;
  std::string g = box.value("gap");
  if (g.empty()) {
    g = box.value("grid-gap");
  }
  if (!g.empty()) {
    std::vector<std::string> toks = SplitWhitespace(g);
    if (!toks.empty()) {
      rowGap = ParseValue(toks[0]).toPx(cw);
      colGap = (toks.size() > 1 ? ParseValue(toks[1]) : ParseValue(toks[0]))
                   .toPx(cw);
    }
  }
  std::string rg = box.value("row-gap");
  if (rg.empty()) {
    rg = box.value("grid-row-gap");
  }
  if (!rg.empty()) {
    rowGap = ParseValue(rg).toPx(cw);
  }
  std::string cg = box.value("column-gap");
  if (cg.empty()) {
    cg = box.value("grid-column-gap");
  }
  if (!cg.empty()) {
    colGap = ParseValue(cg).toPx(cw);
  }
}

// Resolve fixed/fr/auto column tracks into pixel widths. `auto` tracks are
// treated as `1fr` (an even share of the remaining space).
std::vector<float> ResolveTrackWidths(const std::vector<Value> &tracks,
                                      float cw, float colGap) {
  int n = static_cast<int>(tracks.size());
  float fixed = 0, frSum = 0;
  int autoCnt = 0;
  for (const Value &t : tracks) {
    if (t.kind == Value::Kind::Px) {
      fixed += t.number;
    } else if (t.kind == Value::Kind::Percent) {
      fixed += t.toPx(cw);
    } else if (t.kind == Value::Kind::Keyword && t.keyword == "fr") {
      frSum += t.number;
    } else {
      autoCnt++;
    }
  }
  float free = cw - fixed - colGap * (n - 1);
  if (free < 0) {
    free = 0;
  }
  float perFr = (frSum + autoCnt) > 0 ? free / (frSum + autoCnt) : 0;
  std::vector<float> out(n);
  for (int i = 0; i < n; ++i) {
    const Value &t = tracks[i];
    if (t.kind == Value::Kind::Px) {
      out[i] = t.number;
    } else if (t.kind == Value::Kind::Percent) {
      out[i] = t.toPx(cw);
    } else if (t.kind == Value::Kind::Keyword && t.keyword == "fr") {
      out[i] = perFr * t.number;
    } else {
      out[i] = perFr;
    }
  }
  return out;
}

struct Area {
  int rowStart = -1;
  int rowEnd = -1;
  int colStart = -1;
  int colEnd = -1;
};

std::map<std::string, Area> ParseGridTemplateAreas(const std::string &str) {
  std::map<std::string, Area> areas;
  std::vector<std::string> rows;
  size_t i = 0;
  while (i < str.size()) {
    size_t open = str.find('"', i);
    if (open == std::string::npos) {
      open = str.find('\'', i);
    }
    if (open == std::string::npos) {
      break;
    }
    char quoteChar = str[open];
    size_t close = str.find(quoteChar, open + 1);
    if (close == std::string::npos) {
      break;
    }
    rows.push_back(str.substr(open + 1, close - open - 1));
    i = close + 1;
  }

  for (int r = 0; r < (int)rows.size(); ++r) {
    std::vector<std::string> cols = SplitWhitespace(rows[r]);
    for (int c = 0; c < (int)cols.size(); ++c) {
      std::string name = cols[c];
      if (name == "." || name == "none") {
        continue;
      }
      if (areas.find(name) == areas.end()) {
        areas[name] = Area{r + 1, r + 2, c + 1, c + 2};
      } else {
        Area &a = areas[name];
        if (r + 1 < a.rowStart)
          a.rowStart = r + 1;
        if (r + 2 > a.rowEnd)
          a.rowEnd = r + 2;
        if (c + 1 < a.colStart)
          a.colStart = c + 1;
        if (c + 2 > a.colEnd)
          a.colEnd = c + 2;
      }
    }
  }
  return areas;
}

void ParseGridLine(const std::string &startVal, const std::string &endVal,
                   const std::string &shorthand, int &start, int &span) {
  start = -1;
  span = 1;

  std::string val = shorthand;
  std::string s_val = startVal;
  std::string e_val = endVal;

  if (!val.empty()) {
    size_t slash = val.find('/');
    if (slash != std::string::npos) {
      s_val = Trim(val.substr(0, slash));
      e_val = Trim(val.substr(slash + 1));
    } else {
      s_val = Trim(val);
      e_val = "";
    }
  }

  auto parseToken = [](const std::string &tok, int &line, int &sp) {
    std::string t = ToLower(Trim(tok));
    if (t.empty() || t == "auto") {
      return;
    }
    if (t.rfind("span", 0) == 0) {
      std::string numPart = Trim(t.substr(4));
      if (numPart.empty()) {
        sp = 1;
      } else {
        sp = std::atoi(numPart.c_str());
        if (sp < 1)
          sp = 1;
      }
    } else {
      int num = std::atoi(t.c_str());
      if (num > 0) {
        line = num;
      }
    }
  };

  parseToken(s_val, start, span);
  int endLine = -1;
  int endSpan = 1;
  parseToken(e_val, endLine, endSpan);

  if (endLine > 0 && start > 0) {
    if (endLine > start) {
      span = endLine - start;
    } else {
      span = start - endLine;
      start = endLine;
    }
  } else if (endLine > 0) {
    start = endLine - span;
    if (start < 1)
      start = 1;
  } else if (endSpan > 1) {
    span = endSpan;
  }
}

struct YShifter {
  static void shift(LayoutBox &b, float dy) {
    b.dimensions.content.y += dy;
    for (LayoutBox &child : b.children) {
      shift(child, dy);
    }
  }
};

void GridChildren(LayoutBox &box) {
  Dimensions &d = box.dimensions;
  const float cw = d.content.width;

  std::vector<Value> colTracks =
      ParseTrackList(box.value("grid-template-columns"));
  std::vector<Value> rowTracks =
      ParseTrackList(box.value("grid-template-rows"));
  std::vector<LayoutBox *> items = FlowItems(box);
  if (items.empty()) {
    return;
  }

  int numTemplateCols = colTracks.size();
  int numTemplateRows = rowTracks.size();
  int maxCols = numTemplateCols > 0 ? numTemplateCols : 1;
  int maxRows = numTemplateRows > 0 ? numTemplateRows : 1;

  std::map<std::string, Area> areas =
      ParseGridTemplateAreas(box.value("grid-template-areas"));

  std::vector<std::vector<bool>> occupied;
  auto isFree = [&](int r, int c, int rs, int cs) -> bool {
    for (int i = 0; i < rs; ++i) {
      int currRow = r + i;
      if (currRow >= (int)occupied.size()) {
        continue;
      }
      for (int j = 0; j < cs; ++j) {
        int currCol = c + j;
        if (currCol >= (int)occupied[currRow].size()) {
          continue;
        }
        if (occupied[currRow][currCol]) {
          return false;
        }
      }
    }
    return true;
  };

  auto markOccupied = [&](int r, int c, int rs, int cs) {
    if (r + rs > (int)occupied.size()) {
      occupied.resize(r + rs, std::vector<bool>(maxCols, false));
    }
    for (int i = 0; i < rs; ++i) {
      if (c + cs > (int)occupied[r + i].size()) {
        for (auto &rowVec : occupied) {
          rowVec.resize(c + cs, false);
        }
        if (c + cs > maxCols) {
          maxCols = c + cs;
        }
      }
    }
    for (int i = 0; i < rs; ++i) {
      for (int j = 0; j < cs; ++j) {
        occupied[r + i][c + j] = true;
      }
    }
  };

  struct PlacedItem {
    LayoutBox *box;
    int col;
    int row;
    int colSpan;
    int rowSpan;
  };
  std::vector<PlacedItem> placedItems;

  for (LayoutBox *item : items) {
    int colStart = -1, colSpan = 1;
    int rowStart = -1, rowSpan = 1;

    std::string c_start_val = item->value("grid-column-start");
    std::string c_end_val = item->value("grid-column-end");
    std::string c_shorthand = item->value("grid-column");
    ParseGridLine(c_start_val, c_end_val, c_shorthand, colStart, colSpan);

    std::string r_start_val = item->value("grid-row-start");
    std::string r_end_val = item->value("grid-row-end");
    std::string r_shorthand = item->value("grid-row");
    ParseGridLine(r_start_val, r_end_val, r_shorthand, rowStart, rowSpan);

    std::string areaVal = item->value("grid-area");
    if (!areaVal.empty() && areas.find(areaVal) != areas.end()) {
      const Area &a = areas[areaVal];
      colStart = a.colStart;
      colSpan = a.colEnd - a.colStart;
      rowStart = a.rowStart;
      rowSpan = a.rowEnd - a.rowStart;
    } else if (!areaVal.empty()) {
      std::vector<std::string> parts;
      std::stringstream ss(areaVal);
      std::string part;
      while (std::getline(ss, part, '/')) {
        parts.push_back(Trim(part));
      }
      std::string r_start = parts.size() > 0 ? parts[0] : "";
      std::string c_start = parts.size() > 1 ? parts[1] : "";
      std::string r_end = parts.size() > 2 ? parts[2] : "";
      std::string c_end = parts.size() > 3 ? parts[3] : "";

      ParseGridLine(r_start, r_end, "", rowStart, rowSpan);
      ParseGridLine(c_start, c_end, "", colStart, colSpan);
    }

    int colIdx = -1;
    int rowIdx = -1;

    if (colStart > 0 && rowStart > 0) {
      colIdx = colStart - 1;
      rowIdx = rowStart - 1;
    } else if (colStart > 0) {
      colIdx = colStart - 1;
      int r = 0;
      while (!isFree(r, colIdx, rowSpan, colSpan)) {
        r++;
      }
      rowIdx = r;
    } else if (rowStart > 0) {
      rowIdx = rowStart - 1;
      int c = 0;
      while (!isFree(rowIdx, c, rowSpan, colSpan)) {
        c++;
      }
      colIdx = c;
    } else {
      int r = 0;
      int c = 0;
      bool found = false;
      while (!found) {
        int colsLimit = std::max(maxCols, 1);
        for (c = 0; c < colsLimit; ++c) {
          if (isFree(r, c, rowSpan, colSpan)) {
            found = true;
            break;
          }
        }
        if (!found) {
          r++;
        }
      }
      rowIdx = r;
      colIdx = c;
    }

    markOccupied(rowIdx, colIdx, rowSpan, colSpan);
    placedItems.push_back({item, colIdx, rowIdx, colSpan, rowSpan});

    if (rowIdx + rowSpan > maxRows) {
      maxRows = rowIdx + rowSpan;
    }
    if (colIdx + colSpan > maxCols) {
      maxCols = colIdx + colSpan;
    }
  }

  float rowGap, colGap;
  ResolveGaps(box, cw, rowGap, colGap);

  if ((int)colTracks.size() < maxCols) {
    colTracks.resize(maxCols, Value{Value::Kind::Auto, 0, ""});
  }
  std::vector<float> colWidths = ResolveTrackWidths(colTracks, cw, colGap);

  if ((int)rowTracks.size() < maxRows) {
    rowTracks.resize(maxRows, Value{Value::Kind::Auto, 0, ""});
  }

  for (auto &pi : placedItems) {
    float cellW = 0;
    for (int c = 0; c < pi.colSpan; ++c) {
      cellW += colWidths[pi.col + c];
    }
    cellW += colGap * (pi.colSpan - 1);
    LayoutItemInCell(*pi.box, 0, 0, cellW, cw);
  }

  std::vector<float> rowHeights(maxRows, 0.0f);
  for (int r = 0; r < maxRows; ++r) {
    const Value &track = rowTracks[r];
    if (track.kind == Value::Kind::Px) {
      rowHeights[r] = track.number;
    } else if (track.kind == Value::Kind::Percent) {
      Value containerH = ParseValue(box.value("height"));
      if (containerH.kind == Value::Kind::Px) {
        rowHeights[r] = track.toPx(containerH.number);
      }
    }
  }

  for (auto &pi : placedItems) {
    if (pi.rowSpan == 1) {
      float itemH = pi.box->dimensions.marginBox().height;
      if (rowTracks[pi.row].isAuto() ||
          (rowTracks[pi.row].kind == Value::Kind::Keyword &&
           rowTracks[pi.row].keyword == "fr")) {
        rowHeights[pi.row] = std::max(rowHeights[pi.row], itemH);
      }
    }
  }
  for (auto &pi : placedItems) {
    if (pi.rowSpan > 1) {
      float itemH = pi.box->dimensions.marginBox().height;
      float currentSum = 0;
      int autoCount = 0;
      for (int r = 0; r < pi.rowSpan; ++r) {
        currentSum += rowHeights[pi.row + r];
        const Value &track = rowTracks[pi.row + r];
        if (track.isAuto() ||
            (track.kind == Value::Kind::Keyword && track.keyword == "fr")) {
          autoCount++;
        }
      }
      if (currentSum < itemH) {
        float diff = itemH - currentSum;
        if (autoCount > 0) {
          for (int r = 0; r < pi.rowSpan; ++r) {
            const Value &track = rowTracks[pi.row + r];
            if (track.isAuto() ||
                (track.kind == Value::Kind::Keyword && track.keyword == "fr")) {
              rowHeights[pi.row + r] += diff / autoCount;
            }
          }
        } else {
          for (int r = 0; r < pi.rowSpan; ++r) {
            rowHeights[pi.row + r] += diff / pi.rowSpan;
          }
        }
      }
    }
  }

  float fixedAndAutoSum = 0;
  float frSum = 0;
  for (int r = 0; r < maxRows; ++r) {
    if (rowTracks[r].kind == Value::Kind::Keyword &&
        rowTracks[r].keyword == "fr") {
      frSum += rowTracks[r].number;
    } else {
      fixedAndAutoSum += rowHeights[r];
    }
  }
  Value containerH = ParseValue(box.value("height"));
  if (containerH.kind == Value::Kind::Px && frSum > 0) {
    float freeH = containerH.number - fixedAndAutoSum - rowGap * (maxRows - 1);
    if (freeH < 0) {
      freeH = 0;
    }
    float perFr = freeH / frSum;
    for (int r = 0; r < maxRows; ++r) {
      if (rowTracks[r].kind == Value::Kind::Keyword &&
          rowTracks[r].keyword == "fr") {
        rowHeights[r] = perFr * rowTracks[r].number;
      }
    }
  }

  std::vector<float> colX(maxCols, d.content.x);
  float curX = d.content.x;
  for (int c = 0; c < maxCols; ++c) {
    colX[c] = curX;
    curX += colWidths[c] + colGap;
  }

  std::vector<float> rowY(maxRows, d.content.y);
  float curY = d.content.y;
  for (int r = 0; r < maxRows; ++r) {
    rowY[r] = curY;
    curY += rowHeights[r] + rowGap;
  }

  for (auto &pi : placedItems) {
    float finalX = colX[pi.col];
    float finalY = rowY[pi.row];
    float cellW = 0;
    for (int c = 0; c < pi.colSpan; ++c) {
      cellW += colWidths[pi.col + c];
    }
    cellW += colGap * (pi.colSpan - 1);
    LayoutItemInCell(*pi.box, finalX, finalY, cellW, cw);
  }

  if (curY > d.content.y) {
    d.content.height = curY - d.content.y - rowGap;
  } else {
    d.content.height = 0;
  }
}

void LayoutGrid(LayoutBox &box, const Dimensions &containing) {
  CalculateBlockWidth(box, containing);
  CalculateBlockPosition(box, containing);
  box.dimensions.content.height = 0;
  GridChildren(box);
  CalculateBlockHeight(box);
}

void FlexChildren(LayoutBox &box) {
  Dimensions &d = box.dimensions;
  const float cw = d.content.width;

  std::vector<LayoutBox *> items = FlowItems(box);
  if (items.empty()) {
    return;
  }

  float rowGap, colGap;
  ResolveGaps(box, cw, rowGap, colGap);

  int n = static_cast<int>(items.size());
  std::vector<float> outerW(n);
  std::vector<bool> isAuto(n, false);
  float fixedSum = 0;
  int autoCnt = 0;
  for (int i = 0; i < n; ++i) {
    SetItemEdges(*items[i], cw);
    const Dimensions &id = items[i]->dimensions;
    float horiz = id.margin.left + id.margin.right + id.border.left +
                  id.border.right + id.padding.left + id.padding.right;
    Value w = ParseValue(items[i]->value("width"));
    if (w.kind == Value::Kind::Px) {
      outerW[i] = w.number + horiz;
      fixedSum += outerW[i];
    } else if (w.kind == Value::Kind::Percent) {
      outerW[i] = w.toPx(cw) + horiz;
      fixedSum += outerW[i];
    } else {
      isAuto[i] = true;
      autoCnt++;
      outerW[i] = horiz; // content width filled in below
    }
  }

  float totalGap = colGap * (n - 1);
  float autoContent =
      autoCnt > 0 ? std::max(0.0f, (cw - fixedSum - totalGap) / autoCnt) : 0;
  for (int i = 0; i < n; ++i) {
    if (isAuto[i]) {
      outerW[i] += autoContent;
    }
  }

  std::string wrapVal = ToLower(box.value("flex-wrap"));
  bool wrap = (wrapVal == "wrap" || wrapVal == "wrap-reverse");

  struct FlexLine {
    std::vector<int> itemIndices;
    float occupiedWidth = 0;
  };
  std::vector<FlexLine> lines;
  FlexLine currentLine;
  for (int i = 0; i < n; ++i) {
    float itemW = outerW[i];
    float gap = currentLine.itemIndices.empty() ? 0 : colGap;
    if (wrap && !currentLine.itemIndices.empty() &&
        currentLine.occupiedWidth + gap + itemW > cw) {
      lines.push_back(currentLine);
      currentLine = FlexLine{};
    }
    currentLine.occupiedWidth +=
        (currentLine.itemIndices.empty() ? 0.0f : colGap) + itemW;
    currentLine.itemIndices.push_back(i);
  }
  if (!currentLine.itemIndices.empty()) {
    lines.push_back(currentLine);
  }

  float y = d.content.y;
  float totalContainerH = 0;

  for (size_t l = 0; l < lines.size(); ++l) {
    const FlexLine &line = lines[l];
    int lineN = line.itemIndices.size();
    float lineW = line.occupiedWidth;
    float free = std::max(0.0f, cw - lineW);

    std::string justify = ToLower(box.value("justify-content"));
    float offset = 0, extra = 0;
    if (justify == "center") {
      offset = free / 2;
    } else if (justify == "flex-end" || justify == "end" ||
               justify == "right") {
      offset = free;
    } else if (justify == "space-between" && lineN > 1) {
      extra = free / (lineN - 1);
    } else if (justify == "space-around" && lineN > 0) {
      offset = free / (2 * lineN);
      extra = free / lineN;
    } else if (justify == "space-evenly" && lineN > 0) {
      offset = free / (lineN + 1);
      extra = free / (lineN + 1);
    }

    float x = d.content.x + offset;
    float lineH = 0;

    for (int idx : line.itemIndices) {
      LayoutItemInCell(*items[idx], x, y, outerW[idx], cw);
      lineH = std::max(lineH, items[idx]->dimensions.marginBox().height);
      x += outerW[idx] + colGap + extra;
    }

    std::string align = ToLower(box.value("align-items"));
    for (int idx : line.itemIndices) {
      float itemH = items[idx]->dimensions.marginBox().height;
      float diff = lineH - itemH;
      if (diff > 0) {
        if (align == "center") {
          YShifter::shift(*items[idx], diff / 2);
        } else if (align == "flex-end" || align == "end") {
          YShifter::shift(*items[idx], diff);
        }
      }
    }

    y += lineH + rowGap;
    totalContainerH += lineH + rowGap;
  }

  if (totalContainerH > 0) {
    totalContainerH -= rowGap;
  }
  d.content.height = totalContainerH;
}

void LayoutFlex(LayoutBox &box, const Dimensions &containing) {
  CalculateBlockWidth(box, containing);
  CalculateBlockPosition(box, containing);
  box.dimensions.content.height = 0;
  FlexChildren(box);
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
  // Only elements participate in the cascade; a bare text node carries no
  // styles of its own (and Css::matches would reject it from every selector
  // anyway -- computing it would just waste a pass over every rule).
  sn.styles = domRoot.isElement() ? Css::computeStyle(sheet, domRoot)
                                  : std::map<std::string, std::string>{};
  if (domRoot.isElement()) {
    // Resolve var(--x) references against the cascade now, once per element,
    // rather than leaving literal "var(...)" text for every later consumer
    // (Layout's own box-model parsing, Paint's colour parsing, Browser's
    // inline-text colour resolution) to separately fail to understand.
    Css::resolveCssVariables(domRoot, sheet, sn.styles);
  }
  // Walk every child node (elements *and* text), not just domRoot.children()
  // (which is element-only): a bare text node interleaved with elements, e.g.
  // "Click " in "Click <a>here</a>", must still become a box, or its text
  // silently vanishes from layout/paint entirely.
  for (Wrapper::Node child = domRoot.firstChild(); child.valid();
       child = child.nextSibling()) {
    if (child.isElement() || child.isText()) {
      sn.children.push_back(styleTree(child, sheet));
    }
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

  LayoutBox root = BuildLayoutTree(styleRoot, /*inheritedFontSize=*/16);
  Layout(root, viewport);
  return root;
}

void printLayoutTree(const LayoutBox &box) { PrintBox(box, 0); }

} // namespace Layout
} // namespace DesktopWebview
