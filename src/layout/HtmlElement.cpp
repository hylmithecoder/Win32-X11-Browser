#include "HtmlElement.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace HtmlElement {

namespace {

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// ---- Void (self-closing) elements ------------------------------------------
// https://html.spec.whatwg.org/multipage/syntax.html#void-elements
bool IsVoidElementRaw(const std::string &t) {
  static const std::vector<std::string> kVoid = {
      "area",  "base", "br",   "col",   "embed",  "hr",    "img",
      "input", "link", "meta", "param", "source", "track", "wbr"};
  return std::find(kVoid.begin(), kVoid.end(), t) != kVoid.end();
}

// ---- Replaced elements -----------------------------------------------------
// Elements whose content is replaced by an external resource or widget.
bool IsReplacedElementRaw(const std::string &t) {
  static const std::vector<std::string> kReplaced = {
      "img",   "video",    "audio",  "canvas", "iframe",   "embed", "object",
      "input", "textarea", "select", "button", "progress", "meter"};
  return std::find(kReplaced.begin(), kReplaced.end(), t) != kReplaced.end();
}

// ---- Line-break elements ---------------------------------------------------
bool IsLineBreakElementRaw(const std::string &t) { return t == "br"; }

// ---- Horizontal rule -------------------------------------------------------
bool IsHorizontalRuleRaw(const std::string &t) { return t == "hr"; }

// ---- Default display values -----------------------------------------------
// Only tags that differ from the fallback "inline" need an entry here.
// Block elements listed here match the UA stylesheet / CSS spec defaults.
const char *DefaultDisplayRaw(const std::string &t) {
  // Non-rendered metadata
  if (t == "head" || t == "title" || t == "meta" || t == "link" ||
      t == "style" || t == "script" || t == "base" || t == "noscript" ||
      t == "template") {
    return "none";
  }
  // Table layout helpers
  if (t == "tr") {
    return "flex";
  }
  if (t == "td" || t == "th") {
    return "block";
  }
  if (t == "thead" || t == "tbody" || t == "tfoot" || t == "caption") {
    return "contents";
  }
  // Standard block elements
  if (t == "html" || t == "body" || t == "div" || t == "p" || t == "h1" ||
      t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6" ||
      t == "ul" || t == "ol" || t == "li" || t == "section" || t == "article" ||
      t == "header" || t == "footer" || t == "nav" || t == "main" ||
      t == "aside" || t == "figure" || t == "figcaption" || t == "blockquote" ||
      t == "pre" || t == "table" || t == "form" || t == "hr" ||
      t == "address" || t == "dl" || t == "dt" || t == "dd" ||
      t == "fieldset" || t == "details" || t == "summary" || t == "dialog" ||
      t == "center") {
    return "block";
  }
  // Replaced elements default to inline-block
  if (IsReplacedElementRaw(t)) {
    return "inline-block";
  }
  // Everything else (span, a, em, strong, br, etc.) is inline
  return nullptr;
}

} // namespace

bool IsVoidElement(const std::string &tag) {
  return IsVoidElementRaw(ToLower(tag));
}

bool IsReplacedElement(const std::string &tag) {
  return IsReplacedElementRaw(ToLower(tag));
}

bool IsLineBreakElement(const std::string &tag) {
  return IsLineBreakElementRaw(ToLower(tag));
}

bool IsHorizontalRule(const std::string &tag) {
  return IsHorizontalRuleRaw(ToLower(tag));
}

const char *DefaultDisplay(const std::string &tag) {
  return DefaultDisplayRaw(ToLower(tag));
}

} // namespace HtmlElement
} // namespace DesktopWebview
