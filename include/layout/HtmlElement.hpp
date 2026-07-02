#ifndef HTML_ELEMENT_HPP
#define HTML_ELEMENT_HPP

#include <string>

namespace DesktopWebview {
namespace HtmlElement {

// Returns true if the tag is a void (self-closing) HTML element that never
// has children: area, base, br, col, embed, hr, img, input, link, meta,
// param, source, track, wbr.
bool IsVoidElement(const std::string &tag);

// Returns true if the tag is a replaced element whose content is rendered
// by the engine (img, video, audio, canvas, iframe, embed, object, input,
// textarea, select, button).
bool IsReplacedElement(const std::string &tag);

// Returns true if the tag forces a line break when encountered in inline
// flow (currently only <br>).
bool IsLineBreakElement(const std::string &tag);

// Returns true if the tag is a <hr> (horizontal rule).
bool IsHorizontalRule(const std::string &tag);

// Returns the user-agent default display value for the given tag, or ""
// if no override is needed (the caller falls back to "inline").
const char *DefaultDisplay(const std::string &tag);

} // namespace HtmlElement
} // namespace DesktopWebview

#endif // HTML_ELEMENT_HPP
