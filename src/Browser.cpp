#include "../include/Browser.hpp"
#include "../include/Font.hpp"
#include "../include/JsEngine.hpp"
#include "../include/Net.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace DesktopWebview {
namespace Browser {

namespace {

const Paint::Color kBlack{0, 0, 0, 255};
const Paint::Color kWhite{255, 255, 255, 255};

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Collapse runs of whitespace to single spaces and trim, for display.
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
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

// Rendered text size (pixel height) per tag.
int TextSizeFor(const std::string &tag) {
  if (tag == "h1") {
    return 32;
  }
  if (tag == "h2") {
    return 26;
  }
  if (tag == "h3") {
    return 20;
  }
  return 16; // body text
}

// Built-in user-agent stylesheet, used as a fallback when assets/ua.css is not
// found on disk.
const char *kUserAgentCss = "body { margin: 8px; }"
                            "p { margin: 8px 0; }"
                            "h1 { margin: 12px 0; }"
                            "h2 { margin: 10px 0; }"
                            "h3 { margin: 8px 0; }"
                            "ul { margin: 8px 0; }"
                            "ol { margin: 8px 0; }"
                            "li { margin: 2px 0; }"
                            "a { color: #0000ee; }";

// Load the default stylesheet from $DWV_UA_CSS or assets/ua.css, falling back
// to the built-in string.
std::string LoadUaCss() {
  std::vector<std::string> candidates;
  if (const char *env = std::getenv("DWV_UA_CSS")) {
    candidates.push_back(env);
  }
  candidates.push_back("assets/ua.css");
  candidates.push_back("../assets/ua.css");
  candidates.push_back("../../assets/ua.css");
  for (const std::string &path : candidates) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      std::string s((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
      if (!s.empty()) {
        return s;
      }
    }
  }
  return kUserAgentCss;
}

// Nearest-neighbour blit of a bitmap into a destination rect.
void BlitScaled(Paint::Canvas &canvas, const Image::Bitmap &bmp,
                const Layout::Rect &rect) {
  int dx = static_cast<int>(std::lround(rect.x));
  int dy = static_cast<int>(std::lround(rect.y));
  int dw = static_cast<int>(std::lround(rect.width));
  int dh = static_cast<int>(std::lround(rect.height));
  if (dw <= 0 || dh <= 0) {
    dw = bmp.width;
    dh = bmp.height;
  }
  for (int j = 0; j < dh; ++j) {
    int sy = bmp.height * j / dh;
    for (int i = 0; i < dw; ++i) {
      int sx = bmp.width * i / dw;
      canvas.blendPixel(dx + i, dy + j, bmp.at(sx, sy));
    }
  }
}

// A right-pointing play triangle centred in `rect` (video placeholder).
void DrawPlayTriangle(Paint::Canvas &canvas, const Layout::Rect &rect,
                      Paint::Color color) {
  int cx = static_cast<int>(rect.x + rect.width / 2);
  int cy = static_cast<int>(rect.y + rect.height / 2);
  int s = std::max(4, static_cast<int>(std::min(rect.width, rect.height) / 5));
  for (int ry = -s; ry <= s; ++ry) {
    int rowLen = s - std::abs(ry);
    for (int rx = 0; rx < rowLen; ++rx) {
      canvas.blendPixel(cx - s / 2 + rx, cy + ry, color);
    }
  }
}

void FillRect(Paint::Canvas &c, int x, int y, int w, int h, Paint::Color col) {
  c.fillRect(Layout::Rect{static_cast<float>(x), static_cast<float>(y),
                          static_cast<float>(w), static_cast<float>(h)},
             col);
}

} // namespace

Browser::Browser() : m_status("No page loaded") {}

// ---------------------------------------------------------------------------
// URL handling
// ---------------------------------------------------------------------------

std::string Browser::resolveUrl(const std::string &ref) const {
  if (ref.empty()) {
    return "";
  }
  if (ref.find("://") != std::string::npos) {
    return ref; // already absolute
  }
  const std::string &base = m_currentUrl;
  if (base.empty()) {
    return ref;
  }

  if (base.rfind("http://", 0) == 0 || base.rfind("https://", 0) == 0) {
    size_t schemeEnd = base.find("://") + 3;
    size_t hostEnd = base.find('/', schemeEnd);
    std::string schemeHost =
        (hostEnd == std::string::npos) ? base : base.substr(0, hostEnd);
    if (ref[0] == '/') {
      return schemeHost + ref;
    }
    std::string path =
        (hostEnd == std::string::npos) ? "/" : base.substr(hostEnd);
    size_t lastSlash = path.find_last_of('/');
    std::string dir = path.substr(0, lastSlash + 1);
    return schemeHost + dir + ref;
  }

  // Local filesystem base.
  if (ref[0] == '/') {
    return ref;
  }
  size_t lastSlash = base.find_last_of('/');
  std::string dir =
      (lastSlash == std::string::npos) ? "" : base.substr(0, lastSlash + 1);
  return dir + ref;
}

bool Browser::fetchResource(const std::string &absUrl,
                            std::vector<std::uint8_t> &out) const {
  if (absUrl.rfind("http://", 0) == 0 || absUrl.rfind("https://", 0) == 0) {
    std::string resp = Net::Get(absUrl);
    if (resp.empty()) {
      return false;
    }
    std::string body = Net::ExtractBody(resp);
    out.assign(body.begin(), body.end());
    return true;
  }
  std::string path = absUrl;
  if (path.rfind("file://", 0) == 0) {
    path = path.substr(7);
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(file),
             std::istreambuf_iterator<char>());
  return true;
}

// ---------------------------------------------------------------------------
// Navigation / loading
// ---------------------------------------------------------------------------

bool Browser::navigate(const std::string &url) {
  std::string target = url;
  // Bare host like "localhost:8080" -> assume http.
  if (target.find("://") == std::string::npos && !target.empty() &&
      target[0] != '/') {
    target = "http://" + target;
  }

  if (target.rfind("http://", 0) == 0) {
    m_urlText = target.substr(7);
  } else {
    m_urlText = target;
  }
  m_cursorPos = m_urlText.size();

  std::vector<std::uint8_t> bytes;
  if (!fetchResource(target, bytes)) {
    m_status = "Failed to load: " + target;
    m_hasDoc = false;
    m_urlText = url;
    m_cursorPos = m_urlText.size();
    return false;
  }
  std::string html(bytes.begin(), bytes.end());
  return loadHtml(html, target);
}

bool Browser::loadHtml(const std::string &html, const std::string &baseUrl) {
  if (!m_doc.parse(html)) {
    m_status = "Failed to parse document";
    m_hasDoc = false;
    return false;
  }
  m_currentUrl = baseUrl;
  m_scrollY = 0.0f;
  if (baseUrl.rfind("http://", 0) == 0) {
    m_urlText = baseUrl.substr(7);
  } else {
    m_urlText = baseUrl;
  }
  m_cursorPos = m_urlText.size();

  // Stylesheet = UA defaults (assets/ua.css) + link stylesheets + <style>
  // element's text.
  std::string css = LoadUaCss();
  for (const Wrapper::Node &link : m_doc.getElementsByTagName("link")) {
    std::string rel = ToLower(link.attribute("rel"));
    if (rel == "stylesheet") {
      std::string href = link.attribute("href");
      if (!href.empty()) {
        std::string absHref = resolveUrl(href);
        std::vector<std::uint8_t> data;
        if (fetchResource(absHref, data)) {
          css.append(reinterpret_cast<const char *>(data.data()), data.size());
          css.append("\n");
        }
      }
    }
  }
  for (const Wrapper::Node &style : m_doc.getElementsByTagName("style")) {
    css += style.text();
  }
  m_sheet = Css::parse(css);
  m_style = Layout::styleTree(m_doc.root(), m_sheet);

  // Preload <img> sources and <video> posters.
  m_images.clear();
  auto preload = [&](const std::string &attr, const char *tag) {
    for (const Wrapper::Node &el : m_doc.getElementsByTagName(tag)) {
      std::string src = el.attribute(attr);
      if (src.empty()) {
        continue;
      }
      std::string abs = resolveUrl(src);
      if (m_images.count(abs)) {
        continue;
      }
      std::vector<std::uint8_t> data;
      Image::Bitmap bmp;
      if (fetchResource(abs, data) && Image::decode(data, bmp)) {
        m_images[abs] = std::move(bmp);
      }
    }
  };
  preload("src", "img");
  preload("poster", "video");

  m_videos.clear();
  for (const Wrapper::Node &el : m_doc.getElementsByTagName("video")) {
    std::string src = el.attribute("src");
    if (src.empty()) {
      continue;
    }
    std::string abs = resolveUrl(src);
    if (m_videos.count(abs)) {
      continue;
    }
    if (abs.size() >= 5 && abs.substr(abs.size() - 5) == ".rawv") {
      auto fvs = std::make_unique<Video::FileVideoSource>(abs);
      if (fvs->valid()) {
        m_videos[abs] = std::move(fvs);
      } else {
        m_videos[abs] =
            std::make_unique<Video::SyntheticVideoSource>(320, 240, 30.0, 300);
      }
    } else {
      m_videos[abs] =
          std::make_unique<Video::SyntheticVideoSource>(320, 240, 30.0, 300);
    }
  }

  m_startTime = std::chrono::steady_clock::now();

  annotateSizes(m_style);

  m_hasDoc = true;
  m_status.clear();

  // Run script tags with custom JsEngine
  Js::DomInterface dom;
  dom.setTitle = [&](const std::string &title) {
    m_title = title;
    std::cout << "[JS] Document title set to: " << title << std::endl;
  };
  dom.setElementText = [&](const std::string &id, const std::string &text) {
    Wrapper::Node el = m_doc.getElementById(id);
    if (el) {
      el.setText(text);
      m_style = Layout::styleTree(m_doc.root(), m_sheet);
      annotateSizes(m_style);
    }
  };
  dom.getElementText = [&](const std::string &id) -> std::string {
    Wrapper::Node el = m_doc.getElementById(id);
    return el ? el.text() : "";
  };

  Js::JsEngine engine(dom);
  for (const Wrapper::Node &script : m_doc.getElementsByTagName("script")) {
    engine.execute(script.text());
  }

  return true;
}

// ---------------------------------------------------------------------------
// Size annotation (so <img>/<video>/text boxes have dimensions)
// ---------------------------------------------------------------------------

void Browser::annotateSizes(Layout::StyledNode &node) {
  std::string name = node.node.isElement() ? ToLower(node.node.name()) : "";

  auto setIfUnset = [&](const std::string &prop, int value) {
    if (node.styles.find(prop) == node.styles.end()) {
      node.styles[prop] = std::to_string(value) + "px";
    }
  };

  if (name == "img" || name == "video") {
    int w = 0, h = 0;
    std::string wa = node.node.attribute("width");
    std::string ha = node.node.attribute("height");
    if (!wa.empty()) {
      w = std::atoi(wa.c_str());
    }
    if (!ha.empty()) {
      h = std::atoi(ha.c_str());
    }
    // Fall back to the decoded image's intrinsic size.
    std::string srcAttr = (name == "img") ? "src" : "poster";
    auto it = m_images.find(resolveUrl(node.node.attribute(srcAttr)));
    if (it != m_images.end() && it->second.valid()) {
      if (w == 0) {
        w = it->second.width;
      }
      if (h == 0) {
        h = it->second.height;
      }
    }
    if (w == 0) {
      w = (name == "video") ? 320 : 0;
    }
    if (h == 0) {
      h = (name == "video") ? 180 : 0;
    }
    if (w > 0) {
      setIfUnset("width", w);
    }
    if (h > 0) {
      setIfUnset("height", h);
    }
  } else if (node.children.empty() && node.node.isElement()) {
    // Text-only leaf: give it a line of height so it is visible and stacks.
    if (!CollapseWhitespace(node.node.text()).empty()) {
      setIfUnset("height", Font::lineHeight(TextSizeFor(name)));
    }
  }

  for (Layout::StyledNode &child : node.children) {
    annotateSizes(child);
  }
}

// ---------------------------------------------------------------------------
// Compositing images / video / text over the painted boxes
// ---------------------------------------------------------------------------

void Browser::compositeContent(Paint::Canvas &canvas,
                               const Layout::LayoutBox &box) {
  if (box.node && box.node->node.isElement()) {
    const Wrapper::Node &el = box.node->node;
    std::string name = ToLower(el.name());
    const Layout::Rect &c = box.dimensions.content;

    if (name == "img") {
      auto it = m_images.find(resolveUrl(el.attribute("src")));
      if (it != m_images.end() && it->second.valid()) {
        BlitScaled(canvas, it->second, c);
      } else {
        // Broken-image placeholder.
        FillRect(canvas, static_cast<int>(c.x), static_cast<int>(c.y),
                 static_cast<int>(c.width), static_cast<int>(c.height),
                 Paint::Color{0xee, 0xee, 0xee, 255});
      }
    } else if (name == "video") {
      bool drew = false;
      std::string src = el.attribute("src");
      if (!src.empty()) {
        std::string absSrc = resolveUrl(src);
        auto vit = m_videos.find(absSrc);
        if (vit != m_videos.end() && vit->second) {
          double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - m_startTime)
                               .count();
          int index = Video::frameIndexForTime(*vit->second, elapsed);
          Image::Bitmap frame;
          if (vit->second->frameAt(index, frame)) {
            BlitScaled(canvas, frame, c);
            drew = true;
          }
        }
      }
      if (!drew) {
        std::string poster = el.attribute("poster");
        if (!poster.empty()) {
          auto it = m_images.find(resolveUrl(poster));
          if (it != m_images.end() && it->second.valid()) {
            BlitScaled(canvas, it->second, c);
            drew = true;
          }
        }
      }
      if (!drew) {
        FillRect(canvas, static_cast<int>(c.x), static_cast<int>(c.y),
                 static_cast<int>(c.width), static_cast<int>(c.height),
                 Paint::Color{0x20, 0x20, 0x20, 255});
        DrawPlayTriangle(canvas, c, kWhite);
      }
    } else if (box.children.empty()) {
      std::string text = CollapseWhitespace(el.text());
      if (!text.empty()) {
        Paint::Color col;
        if (!Paint::parseColor(box.value("color"), col)) {
          col = kBlack;
        }
        Font::drawText(canvas, static_cast<int>(c.x), static_cast<int>(c.y),
                       text, col, TextSizeFor(name));
      }
    }
  }

  for (const Layout::LayoutBox &child : box.children) {
    compositeContent(canvas, child);
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

Paint::Canvas Browser::renderPage(int width, int height) {
  Paint::Canvas page(width, std::max(1, height));
  page.clear(kWhite);

  if (!m_hasDoc) {
    Font::drawText(page, 8, 8, m_status.empty() ? "No page loaded" : m_status,
                   kBlack, 18);
    return page;
  }

  Layout::LayoutBox box = Layout::layout(m_style, static_cast<float>(width),
                                         static_cast<float>(height));
  Paint::DisplayList list = Paint::buildDisplayList(box);
  page.paint(list);
  compositeContent(page, box);
  return page;
}

void Browser::drawChrome(Paint::Canvas &canvas, int width) {
  Paint::Color barBg{0xdd, 0xdd, 0xdd, 255};
  Paint::Color border{0x88, 0x88, 0x88, 255};

  FillRect(canvas, 0, 0, width, kChromeHeight, barBg);

  int ix = 8, iy = 6, iw = width - 16, ih = kChromeHeight - 12;
  if (iw < 1) {
    iw = 1;
  }
  FillRect(canvas, ix, iy, iw, ih, kWhite);
  // 1px border around the input field.
  FillRect(canvas, ix, iy, iw, 1, border);
  FillRect(canvas, ix, iy + ih - 1, iw, 1, border);
  FillRect(canvas, ix, iy, 1, ih, border);
  FillRect(canvas, ix + iw - 1, iy, 1, ih, border);

  int px = 18;
  int ty = iy + (ih - Font::lineHeight(px)) / 2;

  std::string displayUrl = m_urlText;
  if (m_cursorPos <= displayUrl.size()) {
    displayUrl.insert(m_cursorPos, "|");
  } else {
    displayUrl.push_back('|');
  }
  Font::drawText(canvas, ix + 4, ty, displayUrl, kBlack, px);

  // Separator under the chrome.
  FillRect(canvas, 0, kChromeHeight - 1, width, 1, border);
}

Paint::Canvas Browser::render(int width, int height) {
  if (width < 1) {
    width = 1;
  }
  if (height < 1) {
    height = 1;
  }
  m_lastWidth = width;
  m_lastHeight = height;

  Paint::Canvas canvas(width, height);
  canvas.clear(kWhite);

  int pageViewportH = std::max(1, height - kChromeHeight);

  if (!m_hasDoc) {
    Font::drawText(canvas, 8, kChromeHeight + 8,
                   m_status.empty() ? "No page loaded" : m_status, kBlack, 18);
    drawChrome(canvas, width);
    return canvas;
  }

  // 1. Run layout with viewport width to determine content height
  Layout::LayoutBox box = Layout::layout(
      m_style, static_cast<float>(width), static_cast<float>(pageViewportH));
  float contentH = box.dimensions.marginBox().height;
  int pageCanvasH = std::max(pageViewportH, static_cast<int>(std::ceil(contentH)));

  // 2. Render the full page canvas
  Paint::Canvas page = renderPage(width, pageCanvasH);

  // 3. Clamp scroll offset
  int maxScrollY = std::max(0, pageCanvasH - pageViewportH);
  if (m_scrollY < 0.0f) {
    m_scrollY = 0.0f;
  }
  if (m_scrollY > maxScrollY) {
    m_scrollY = static_cast<float>(maxScrollY);
  }

  // 4. Copy the scrolled viewport slice onto canvas
  int startY = static_cast<int>(m_scrollY);
  for (int y = 0; y < pageViewportH; ++y) {
    int srcY = startY + y;
    if (srcY >= 0 && srcY < pageCanvasH) {
      for (int x = 0; x < width; ++x) {
        canvas.blendPixel(x, y + kChromeHeight, page.at(x, srcY));
      }
    }
  }

  drawChrome(canvas, width);
  return canvas;
}

// ---------------------------------------------------------------------------
// Input / Mouse Click Handling
// ---------------------------------------------------------------------------

namespace {

bool IsPointInRect(float px, float py, const Layout::Rect &r) {
  return px >= r.x && px <= (r.x + r.width) && py >= r.y &&
         py <= (r.y + r.height);
}

const Layout::LayoutBox *FindBoxAt(const Layout::LayoutBox &box, float px,
                                   float py) {
  if (!IsPointInRect(px, py, box.dimensions.borderBox())) {
    return nullptr;
  }
  for (auto it = box.children.rbegin(); it != box.children.rend(); ++it) {
    const Layout::LayoutBox *found = FindBoxAt(*it, px, py);
    if (found) {
      return found;
    }
  }
  return &box;
}

} // namespace

bool Browser::handleClick(int x, int y) {
  if (y < kChromeHeight) {
    return false; // clicked in chrome area
  }
  if (!m_hasDoc) {
    return false;
  }

  // Re-run layout to find where elements are.
  int pageH = std::max(1, m_lastHeight - kChromeHeight);
  Layout::LayoutBox box = Layout::layout(
      m_style, static_cast<float>(m_lastWidth), static_cast<float>(pageH));

  float px = static_cast<float>(x);
  float py = static_cast<float>(y - kChromeHeight) + m_scrollY;

  const Layout::LayoutBox *found = FindBoxAt(box, px, py);
  if (found && found->node) {
    Wrapper::Node n = found->node->node;
    while (n) {
      if (n.isElement() && ToLower(n.name()) == "a") {
        std::string href = n.attribute("href");
        if (!href.empty()) {
          navigate(resolveUrl(href));
          return true;
        }
      }
      n = n.parent();
    }
  }
  return false;
}

bool Browser::handleKey(const KeyInput &key) {
  switch (key.kind) {
  case KeyInput::Char:
    if (m_cursorPos <= m_urlText.size()) {
      m_urlText.insert(m_cursorPos, 1, key.ch);
      m_cursorPos++;
    } else {
      m_urlText.push_back(key.ch);
      m_cursorPos = m_urlText.size();
    }
    return true;

  case KeyInput::Backspace:
    if (m_cursorPos > 0 && !m_urlText.empty()) {
      m_urlText.erase(m_cursorPos - 1, 1);
      m_cursorPos--;
      return true;
    }
    return false;

  case KeyInput::Left:
    if (m_cursorPos > 0) {
      m_cursorPos--;
      return true;
    }
    return false;

  case KeyInput::Right:
    if (m_cursorPos < m_urlText.size()) {
      m_cursorPos++;
      return true;
    }
    return false;

  case KeyInput::Enter:
    navigate(m_urlText);
    return true;

  case KeyInput::Up:
    m_scrollY = std::max(0.0f, m_scrollY - 30.0f);
    return true;

  case KeyInput::Down:
    m_scrollY += 30.0f;
    return true;
  }
  return false;
}

bool Browser::handleScroll(int delta) {
  m_scrollY += delta;
  if (m_scrollY < 0.0f) {
    m_scrollY = 0.0f;
  }
  return true;
}

} // namespace Browser
} // namespace DesktopWebview
