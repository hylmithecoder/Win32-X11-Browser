#ifndef LAYOUT_HPP
#define LAYOUT_HPP

#include "Css.hpp"
#include "Wrapper.hpp"

#include <map>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Layout {

// An axis-aligned rectangle in CSS pixels. (x, y) is the top-left corner.
struct Rect {
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
};

// The four edges of a box (margin, border, or padding thickness).
struct EdgeSizes {
  float left = 0;
  float right = 0;
  float top = 0;
  float bottom = 0;
};

// The full box-model geometry of a single box: the content rectangle plus the
// surrounding padding/border/margin edges. The helpers expand the content rect
// outward through each edge.
struct Dimensions {
  Rect content;
  EdgeSizes padding;
  EdgeSizes border;
  EdgeSizes margin;

  Rect paddingBox() const;
  Rect borderBox() const;
  Rect marginBox() const;
};

// Block: participates in vertical block flow. Inline: an inline-level box.
// Anonymous: a generated block box wrapping a run of inline children (no DOM
// node). NOTE: this MVP lays inline/anonymous boxes out with the block
// algorithm; real horizontal inline flow (text wrapping) is deferred.
enum class BoxType { Block, Inline, Anonymous };

// A DOM element paired with its computed style, forming the style tree. Text
// and other non-element nodes are not included at this stage.
struct StyledNode {
  Wrapper::Node node;
  std::map<std::string, std::string> styles;
  std::vector<StyledNode> children;

  // Computed value of a property, or "" when unset.
  std::string value(const std::string &property) const;
  // Resolved `display` value: explicit style if present, else the user-agent
  // default for the tag. Returns "none" for boxes that generate no layout.
  std::string display() const;
};

// A node in the layout (box) tree. `node` points into the StyledNode tree and
// is null for anonymous boxes, so the StyledNode tree must outlive the
// LayoutBox tree.
struct LayoutBox {
  BoxType type = BoxType::Block;
  Dimensions dimensions;
  const StyledNode *node = nullptr;
  std::vector<LayoutBox> children;

  // Computed value of a property on the backing StyledNode, or "" when the box
  // is anonymous or the property is unset.
  std::string value(const std::string &property) const;
};

// Build the style tree by pairing each element under `domRoot` with the result
// of the CSS cascade against `sheet`.
StyledNode styleTree(const Wrapper::Node &domRoot,
                     const Css::Stylesheet &sheet);

// Build and lay out the box tree for `styleRoot` inside a viewport of the given
// width (and optional height). Returns the root box with all dimensions filled.
LayoutBox layout(const StyledNode &styleRoot, float viewportWidth,
                 float viewportHeight = 0);

// Print an indented dump of the box tree (type, tag, content rectangle) to
// stdout for debugging.
void printLayoutTree(const LayoutBox &box);

} // namespace Layout
} // namespace DesktopWebview

#endif // LAYOUT_HPP
