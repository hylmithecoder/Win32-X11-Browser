#ifndef BROWSER_HPP
#define BROWSER_HPP

#include "Css.hpp"
#include "Image.hpp"
#include "Layout.hpp"
#include "Paint.hpp"
#include "Wrapper.hpp"
#include "JsEngine.hpp"

#include "AudioPlayer.hpp"
#include "Video.hpp"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Browser {

// A key event delivered to the address bar / browser chrome.
struct KeyInput {
  enum Kind {
    Char,
    Backspace,
    Enter,
    Left,
    Right,
    Up,
    Down,
    Tab,
    Delete,
    Home,
    End
  };
  Kind kind = Char;
  char ch = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
};

// Saved state for a single tab (everything needed to restore it).
struct Tab {
  std::string url;
  std::string title;
  std::string htmlContent; // cached HTML for reload
  float scrollY = 0.0f;
  // PDF state
  std::vector<std::pair<double, double>> pdfPageSizes;
  float pdfZoom = 1.0f;
  bool pdfSidebarOpen = true;
  int pdfCurrentPage = 0;
};

// A navigation history entry.
struct HistoryEntry {
  std::string url;
  std::string htmlContent;
};

class Browser {
public:
  Browser();

  static constexpr int kBrowserHeight = 36;
  static constexpr int kTabBarHeight = 32;

  bool navigate(const std::string &url);
  bool loadHtml(const std::string &html, const std::string &baseUrl);
  bool handleKey(const KeyInput &key);
  bool handleClick(int x, int y);
  bool handleMouseDown(int x, int y);
  bool handleMouseMove(int x, int y);
  bool handleMouseUp(int x, int y);
  std::string selectedText() const;
  bool handleScroll(int delta);
  Paint::Canvas render(int width, int height);

  const std::string &urlText() const { return m_urlText; }
  const std::string &currentUrl() const { return m_currentUrl; }
  void setUrlText(const std::string &text) { m_urlText = text; }

  // Navigation
  bool goBack();
  bool goForward();
  bool reload();

  // Tab management
  int tabCount() const { return static_cast<int>(m_tabs.size()); }
  int activeTabIndex() const { return m_activeTab; }
  bool newTab(const std::string &url = "about:blank");
  bool closeTab(int index);
  bool switchTab(int index);

  // Clipboard
  void handlePaste(const std::string &text);
  std::function<void()> m_requestPaste;
  std::function<void(const std::string &)> m_copyText;

private:
  Paint::Canvas renderPage(int width, int height);
  void drawBrowser(Paint::Canvas &canvas, int width);
  void drawTabBar(Paint::Canvas &canvas, int width);

  std::string resolveUrl(const std::string &ref) const;
  bool fetchResource(const std::string &absUrl,
                     std::vector<std::uint8_t> &out) const;
  std::string generatePdfHtml(const std::string &target);
  float getPageYOffset(int pageNum) const;
  void updatePdfCurrentPageOnScroll();

  void annotateSizes(Layout::StyledNode &node);
  void compositeContent(Paint::Canvas &canvas, const Layout::LayoutBox &box);
  void updateAudioPageDom();

  // Tab state save/restore
  void saveTabState(int index);
  void restoreTabState(int index);
  void ensureDefaultTab();

  // ---- Text selection -----------------------------------------------------
  struct TextRun {
    Layout::Rect rect;
    std::string text;
    int fontSize = 16;
    int tx = 0;
  };
  struct SelPos {
    int run = -1;
    int ch = 0;
  };

  bool textRunFor(const Layout::LayoutBox &box, TextRun &out) const;
  void collectTextRuns(const Layout::LayoutBox &box,
                       std::vector<TextRun> &runs) const;
  SelPos hitTest(const std::vector<TextRun> &runs, float px, float py) const;
  std::vector<TextRun> layoutTextRuns() const;
  void paintSelection(Paint::Canvas &canvas, int runIdx,
                      const TextRun &run) const;

  SelPos m_selAnchor;
  SelPos m_selFocus;
  bool m_selecting = false;
  int m_paintRunCursor = 0;

  // ---- Tab + history state ------------------------------------------------
  std::vector<Tab> m_tabs;
  int m_activeTab = 0;

  // Per-tab navigation history (index into m_tabs)
  // Each tab has its own history stack
  std::vector<std::vector<HistoryEntry>> m_tabHistory;
  std::vector<int> m_tabHistoryIndex;

  // ---- Current page state (live, belonging to active tab) ------------------
  std::string m_urlText;
  std::string m_currentUrl;
  std::string m_status;
  std::string m_title;

  bool m_hasDoc = false;
  Wrapper::HtmlDocument m_doc;
  Css::Stylesheet m_sheet;
  Layout::StyledNode m_style;
  Wrapper::Node m_focusedNode;
  std::unique_ptr<Js::JsEngine> m_jsEngine;

  std::map<std::string, Image::Bitmap> m_images;
  std::vector<std::uint8_t> m_pdfBytes;
  std::map<int, Image::Bitmap> m_pdfPages;
  std::map<std::string, std::unique_ptr<Video::VideoSource>> m_videos;
  Audio::AudioPlayer m_audioPlayer;
  std::chrono::steady_clock::time_point m_startTime;

  int m_lastWidth = 1024;
  int m_lastHeight = 720;
  float m_scrollY = 0.0f;

  // Scrollbar interaction state
  static constexpr int kScrollbarWidth = 14;
  bool m_scrollbarDragging = false;
  float m_scrollbarDragOffset = 0.0f;
  int m_pageCanvasH = 0;
  int m_pageViewportH = 0;

  size_t m_cursorPos = 0;

  // URL bar text selection
  int m_urlSelAnchor = -1; // -1 = no selection
  int m_urlSelFocus = -1;
  bool m_urlSelecting = false; // true while dragging in URL bar

  std::string urlBarSelectedText() const;
  void urlBarDeleteSelection();

  // PDF state
  std::vector<std::pair<double, double>> m_pdfPageSizes;
  float m_pdfZoom = 1.0f;
  bool m_pdfSidebarOpen = true;
  int m_pdfCurrentPage = 0;

  // Cached HTML for current tab reload
  std::string m_currentHtml;
};

} // namespace Browser
} // namespace DesktopWebview

#endif // BROWSER_HPP
