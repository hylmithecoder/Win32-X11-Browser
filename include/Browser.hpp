#ifndef BROWSER_HPP
#define BROWSER_HPP

#include "Css.hpp"
#include "Image.hpp"
#include "Layout.hpp"
#include "Paint.hpp"
#include "Wrapper.hpp"

#include <map>
#include <string>

namespace DesktopWebview {
namespace Browser {

// A key event delivered to the address bar.
struct KeyInput {
  enum Kind { Char, Backspace, Enter, Left, Right };
  Kind kind = Char;
  char ch = 0;
};

// Ties the engine together: fetch a URL, parse the DOM, load <img>/<video>
// resources, style + lay out, and paint the page beneath a simple address-bar
// chrome with an editable URL field.
class Browser {
public:
  Browser();

  // Height of the address-bar chrome drawn above the page.
  static constexpr int kChromeHeight = 36;

  // Fetch `url` (http/https via Net, or a local file path), then load it.
  // Updates the address bar to the resolved URL. Returns false on fetch/parse
  // failure (a status message is shown instead).
  bool navigate(const std::string &url);

  // Load an already-fetched HTML document with the given base URL (used by
  // navigate, and directly in tests). Resolves and decodes resources.
  bool loadHtml(const std::string &html, const std::string &baseUrl);

  // Feed a key event to the address bar. Returns true if the view should be
  // repainted. Enter navigates to the typed URL.
  bool handleKey(const KeyInput &key);

  // Handle a click event at the given canvas coordinates. Returns true if the
  // view should be repainted (e.g. if navigation was triggered).
  bool handleClick(int x, int y);

  // Render chrome + page into a fresh canvas of the given size.
  Paint::Canvas render(int width, int height);

  const std::string &urlText() const { return m_urlText; }
  const std::string &currentUrl() const { return m_currentUrl; }
  void setUrlText(const std::string &text) { m_urlText = text; }

private:
  // Render just the page area (without chrome) into a canvas.
  Paint::Canvas renderPage(int width, int height);
  void drawChrome(Paint::Canvas &canvas, int width);

  // Resolve `ref` against the current base URL into an absolute URL/path.
  std::string resolveUrl(const std::string &ref) const;
  // Fetch bytes for an absolute url/path. Returns false on failure.
  bool fetchResource(const std::string &absUrl,
                     std::vector<std::uint8_t> &out) const;

  // After styling, set width/height on <img>/<video> boxes (from attributes or
  // intrinsic image size) and give text-only blocks a line of height so they
  // are visible and stack.
  void annotateSizes(Layout::StyledNode &node);
  // Walk the layout tree compositing images, video placeholders, and text.
  void compositeContent(Paint::Canvas &canvas, const Layout::LayoutBox &box);

  std::string m_urlText;    // editable address-bar contents
  std::string m_currentUrl; // last successfully loaded URL (also the base)
  std::string m_status;     // status / error line shown when no page

  bool m_hasDoc = false;
  Wrapper::HtmlDocument m_doc;
  Css::Stylesheet m_sheet;
  Layout::StyledNode m_style;

  // Decoded resources keyed by absolute URL.
  std::map<std::string, Image::Bitmap> m_images;

  // Last rendered dimensions used for mapping click coordinates back to page
  // layout space.
  int m_lastWidth = 1024;
  int m_lastHeight = 720;

  // Text cursor position inside the address bar (m_urlText).
  size_t m_cursorPos = 0;
};

} // namespace Browser
} // namespace DesktopWebview

#endif // BROWSER_HPP
