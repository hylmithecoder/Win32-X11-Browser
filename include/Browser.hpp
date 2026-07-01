#ifndef BROWSER_HPP
#define BROWSER_HPP

#include "Css.hpp"
#include "Image.hpp"
#include "Layout.hpp"
#include "Paint.hpp"
#include "Wrapper.hpp"

#include "Video.hpp"
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Browser {

// A key event delivered to the address bar.
struct KeyInput {
  enum Kind { Char, Backspace, Enter, Left, Right, Up, Down };
  Kind kind = Char;
  char ch = 0;
};

// Ties the engine together: fetch a URL, parse the DOM, load <img>/<video>
// resources, style + lay out, and paint the page beneath a simple address-bar
// browser with an editable URL field.
class Browser {
public:
  Browser();

  // Height of the address-bar browser drawn above the page.
  static constexpr int kBrowserHeight = 36;

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

  // Mouse press / drag / release for text selection. A press starts a
  // selection at the cursor; dragging extends it; a release with no drag is
  // treated as a click (link activation). Each returns true if the view should
  // be repainted.
  bool handleMouseDown(int x, int y);
  bool handleMouseMove(int x, int y);
  bool handleMouseUp(int x, int y);

  // The currently selected page text (empty if nothing is selected). The host
  // window copies this to the system clipboard when a selection completes.
  std::string selectedText() const;

  // Handle a scroll event with the given pixel delta. Returns true if the
  // view should be repainted.
  bool handleScroll(int delta);

  // Render browser + page into a fresh canvas of the given size.
  Paint::Canvas render(int width, int height);

  const std::string &urlText() const { return m_urlText; }
  const std::string &currentUrl() const { return m_currentUrl; }
  void setUrlText(const std::string &text) { m_urlText = text; }

private:
  // Render just the page area (without browser) into a canvas.
  Paint::Canvas renderPage(int width, int height);
  void drawBrowser(Paint::Canvas &canvas, int width);

  // Resolve `ref` against the current base URL into an absolute URL/path.
  std::string resolveUrl(const std::string &ref) const;
  // Fetch bytes for an absolute url/path. Returns false on failure.
  bool fetchResource(const std::string &absUrl,
                     std::vector<std::uint8_t> &out) const;
  std::string generatePdfHtml(const std::string &target);
  float getPageYOffset(int pageNum) const;
  void updatePdfCurrentPageOnScroll();

  // After styling, set width/height on <img>/<video> boxes (from attributes or
  // intrinsic image size) and give text-only blocks a line of height so they
  // are visible and stack.
  void annotateSizes(Layout::StyledNode &node);
  // Walk the layout tree compositing images, video placeholders, and text.
  void compositeContent(Paint::Canvas &canvas, const Layout::LayoutBox &box);

  // ---- Text selection -----------------------------------------------------
  // A run of selectable text: one text-leaf box, with its painted geometry in
  // page-canvas coordinates.
  struct TextRun {
    Layout::Rect rect; // x=tx, y=content top, width=text width, height=line
    std::string text;
    int fontSize = 16;
    int tx = 0;
  };
  // A caret position: an index into the text-run list plus a character offset.
  struct SelPos {
    int run = -1;
    int ch = 0;
  };

  // True if `box` is a paintable text leaf; fills `out` with its run geometry.
  // Mirrors the text branch of compositeContent so run indices line up.
  bool textRunFor(const Layout::LayoutBox &box, TextRun &out) const;
  // Pre-order collection of every text run, in paint order.
  void collectTextRuns(const Layout::LayoutBox &box,
                       std::vector<TextRun> &runs) const;
  // Map a page-space point to the nearest caret position.
  SelPos hitTest(const std::vector<TextRun> &runs, float px, float py) const;
  // Re-run layout at the last rendered size and collect the current runs.
  std::vector<TextRun> layoutTextRuns() const;
  // Paint the selection highlight for the text run at paint index `runIdx`.
  void paintSelection(Paint::Canvas &canvas, int runIdx,
                      const TextRun &run) const;

  SelPos m_selAnchor; // where the drag started
  SelPos m_selFocus;  // where it currently ends
  bool m_selecting = false;
  int m_paintRunCursor = 0; // text-run counter during compositeContent

  std::string m_urlText;    // editable address-bar contents
  std::string m_currentUrl; // last successfully loaded URL (also the base)
  std::string m_status;     // status / error line shown when no page
  std::string m_title;      // document title set by JS or parsed

  bool m_hasDoc = false;
  Wrapper::HtmlDocument m_doc;
  Css::Stylesheet m_sheet;
  Layout::StyledNode m_style;

  // Decoded resources keyed by absolute URL.
  std::map<std::string, Image::Bitmap> m_images;

  // Raw PDF file data (cached).
  std::vector<std::uint8_t> m_pdfBytes;

  // Cached individual PDF pages.
  std::map<int, Image::Bitmap> m_pdfPages;

  // Decoded videos keyed by absolute URL.
  std::map<std::string, std::unique_ptr<Video::VideoSource>> m_videos;

  // High-resolution clock start time for video playback.
  std::chrono::steady_clock::time_point m_startTime;

  // Last rendered dimensions used for mapping click coordinates back to page
  // layout space.
  int m_lastWidth = 1024;
  int m_lastHeight = 720;
  float m_scrollY = 0.0f;

  // Text cursor position inside the address bar (m_urlText).
  size_t m_cursorPos = 0;

  // PDF layout state variables
  std::vector<std::pair<double, double>> m_pdfPageSizes;
  float m_pdfZoom = 1.0f;
  bool m_pdfSidebarOpen = true;
  int m_pdfCurrentPage = 0;
};

} // namespace Browser
} // namespace DesktopWebview

#endif // BROWSER_HPP
