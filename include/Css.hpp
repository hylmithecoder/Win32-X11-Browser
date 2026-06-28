#ifndef CSS_HPP
#define CSS_HPP

#include "Wrapper.hpp"

#include <map>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Css {

// CSS specificity as the (ids, classes, types) triple. Universal selectors and
// combinators contribute nothing. Ordered lexicographically: ids dominate
// classes, which dominate type selectors.
struct Specificity {
  int ids = 0;
  int classes = 0;
  int types = 0;

  bool operator<(const Specificity &o) const {
    if (ids != o.ids) {
      return ids < o.ids;
    }
    if (classes != o.classes) {
      return classes < o.classes;
    }
    return types < o.types;
  }
  bool operator==(const Specificity &o) const {
    return ids == o.ids && classes == o.classes && types == o.types;
  }
};

// One compound selector: an optional type/universal plus any number of class
// and a single id constraint, e.g. "div.foo.bar#baz". MVP scope: no attribute
// selectors, pseudo-classes, or pseudo-elements.
struct SimpleSelector {
  std::string tag;                  // lower-cased tag name, or "" if none
  std::string id;                   // id without '#', or "" if none
  std::vector<std::string> classes; // class names without '.'
  bool universal = false;           // true when '*' is present

  // True when this selector imposes no constraints (e.g. bare "*").
  bool isUnconstrained() const {
    return tag.empty() && id.empty() && classes.empty();
  }
};

// A full selector: a chain of compound selectors joined by descendant
// combinators (whitespace). The last component is the "subject" matched
// against the candidate element; earlier components must match ancestors.
// MVP scope: only the descendant combinator (child/sibling combinators are
// treated as descendant).
struct Selector {
  std::vector<SimpleSelector> components;

  Specificity specificity() const;
};

// A single "property: value" pair, with the !important flag parsed out.
struct Declaration {
  std::string property; // lower-cased
  std::string value;
  bool important = false;
};

// One rule: a group of selectors (comma-separated) sharing a declaration block.
struct Rule {
  std::vector<Selector> selectors;
  std::vector<Declaration> declarations;
};

// A parsed stylesheet: an ordered list of rules. Source order is preserved and
// used to break specificity ties during the cascade.
struct Stylesheet {
  std::vector<Rule> rules;
};

// Parse CSS text into a Stylesheet. Tolerant of malformed input: comments are
// stripped, at-rules (e.g. @media) are skipped along with their blocks, and
// incomplete rules are dropped rather than throwing.
Stylesheet parse(const std::string &css);

// Parse a stand-alone declaration list, as found in an inline style="" value.
std::vector<Declaration> parseDeclarations(const std::string &block);

// True if `node` matches `selector`, honouring descendant combinators by
// walking the node's ancestors.
bool matches(const Selector &selector, const Wrapper::Node &node);

// Resolve the cascade for `node` against `sheet` and return the winning
// property -> value map. Accounts for !important, specificity, source order,
// and the element's own inline style="" (treated with the usual inline
// priority). Values are specified values; inheritance/initial resolution is
// out of scope for this stage.
std::map<std::string, std::string> computeStyle(const Stylesheet &sheet,
                                                const Wrapper::Node &node);

} // namespace Css
} // namespace DesktopWebview

#endif // CSS_HPP
