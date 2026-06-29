#include "../include/Browser.hpp"
#include "../include/Base64.hpp"
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

std::string ResolveInheritedPropertyForDomNode(const Wrapper::Node &domNode,
                                               const std::string &property,
                                               const Css::Stylesheet &sheet) {
  Wrapper::Node current = domNode;
  while (current.valid()) {
    std::map<std::string, std::string> style =
        Css::computeStyle(sheet, current);
    auto it = style.find(property);
    if (it != style.end() && !it->second.empty()) {
      return it->second;
    }
    current = current.parent();
  }
  return "";
}

int ResolveFontSizeForDomNode(const Wrapper::Node &domNode,
                              const Css::Stylesheet &sheet, int defaultSize) {
  std::string fs =
      ResolveInheritedPropertyForDomNode(domNode, "font-size", sheet);
  if (fs.empty()) {
    return defaultSize;
  }

  const char *start = fs.c_str();
  char *end = nullptr;
  double val = std::strtod(start, &end);
  if (end == start) {
    return defaultSize;
  }

  std::string unit = end;
  size_t first = unit.find_first_not_of(" \t\r\n\f");
  if (first != std::string::npos) {
    unit = unit.substr(first);
  }

  if (unit == "%") {
    Wrapper::Node parent = domNode.parent();
    if (parent.valid()) {
      int parentSize = ResolveFontSizeForDomNode(parent, sheet, 16);
      return static_cast<int>(val / 100.0f * parentSize);
    }
    return static_cast<int>(val / 100.0f * defaultSize);
  }

  if (unit == "em") {
    Wrapper::Node parent = domNode.parent();
    if (parent.valid()) {
      int parentSize = ResolveFontSizeForDomNode(parent, sheet, 16);
      return static_cast<int>(val * parentSize);
    }
    return static_cast<int>(val * defaultSize);
  }

  return static_cast<int>(val);
}

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

// Lowercased file extension of a URL/path (without the dot), ignoring any
// query string or fragment. Empty if none.
std::string UrlExtension(const std::string &url) {
  std::string u = url;
  size_t q = u.find_first_of("?#");
  if (q != std::string::npos) {
    u = u.substr(0, q);
  }
  size_t slash = u.find_last_of('/');
  std::string name = (slash == std::string::npos) ? u : u.substr(slash + 1);
  size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) {
    return "";
  }
  return ToLower(name.substr(dot + 1));
}

// True for video container extensions we can decode via ffmpeg.
bool IsVideoExtension(const std::string &ext) {
  return ext == "mp4" || ext == "webm" || ext == "mkv" || ext == "avi" ||
         ext == "mov" || ext == "m4v" || ext == "ogv";
}

// For media/binary resources we cannot render inline, return a human label
// (e.g. "MP4 video"); empty string means "treat as an HTML document".
std::string StandaloneMediaLabel(const std::string &ext) {
  if (IsVideoExtension(ext)) {
    return ext == "mp4" ? "MP4 video" : (ext + " video");
  }
  if (ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac" ||
      ext == "aac" || ext == "m4a") {
    return ext + " audio";
  }
  if (ext == "pdf") {
    return "PDF document";
  }
  if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "rar" ||
      ext == "7z" || ext == "exe" || ext == "bin" || ext == "iso") {
    return ext + " file";
  }
  return "";
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

// Draw a 1px rectangle outline.
void StrokeRect(Paint::Canvas &c, int x, int y, int w, int h,
                Paint::Color col) {
  if (w <= 0 || h <= 0) {
    return;
  }
  FillRect(c, x, y, w, 1, col);
  FillRect(c, x, y + h - 1, w, 1, col);
  FillRect(c, x, y, 1, h, col);
  FillRect(c, x + w - 1, y, 1, h, col);
}

} // namespace

Browser::Browser() : m_status("No page loaded") {
  // Try to initialise OpenCL for GPU-accelerated base64. Non-fatal if no GPU.
  Base64::initOpenCL();
}

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
  // data: URI (e.g. data:image/png;base64,iVBOR...)
  if (absUrl.rfind("data:", 0) == 0) {
    // Format: data:[<mediatype>][;base64],<data>
    size_t comma = absUrl.find(',');
    if (comma == std::string::npos) {
      return false;
    }
    std::string meta = absUrl.substr(5, comma - 5); // after "data:"
    std::string payload = absUrl.substr(comma + 1);
    bool isBase64 = false;
    // Check for ;base64 in the metadata.
    size_t semi = meta.find(';');
    if (semi != std::string::npos) {
      std::string afterSemi = meta.substr(semi + 1);
      // Lowercase and trim to check for "base64".
      std::string lower = afterSemi;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      // Trim whitespace.
      size_t start = lower.find_first_not_of(" \t\r\n");
      if (start != std::string::npos) {
        lower = lower.substr(start);
      }
      if (lower == "base64") {
        isBase64 = true;
      }
    }
    if (isBase64) {
      out = Base64::decode(payload);
      return !out.empty();
    }
    // Plain data: URI (percent-encoded) -- treat as raw text for now.
    out.assign(payload.begin(), payload.end());
    return true;
  }

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
  // Directly-opened video file (e.g. an .mp4): wrap it in a <video> element so
  // the ffmpeg decode + playback pipeline renders it full width.
  std::string ext = UrlExtension(target);
  if (IsVideoExtension(ext)) {
    std::string page =
        "<html><head><title>" + target +
        "</title></head><body style=\"margin:0\">"
        "<video src=\"" +
        target + "\" width=\"" +
        std::to_string(std::max(320, m_lastWidth ? m_lastWidth : 960)) +
        "\"></video></body></html>";
    return loadHtml(page, target);
  }

  // Other media / binary files we cannot render inline (audio/pdf/archives):
  // show a small placeholder page instead of parsing bytes as HTML.
  std::string label = StandaloneMediaLabel(ext);
  if (!label.empty()) {
    std::string page =
        "<html><head><title>" + label + "</title></head><body><h1>" + label +
        "</h1>" + "<p>This file type cannot be displayed yet.</p>" + "<p>" +
        target + "</p>" + "<p>" + std::to_string(bytes.size()) +
        " bytes downloaded.</p></body></html>";
    return loadHtml(page, target);
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
    // openVideoFile picks ffmpeg for compressed media (mp4/webm/...), the raw
    // reader for .rawv, and a synthetic preview as a last resort. For http(s)
    // sources we pass the URL straight to ffmpeg (it has its own protocol
    // handlers); local paths/file:// are opened directly.
    m_videos[abs] = Video::openVideoFile(abs);
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
    // Skip non-JavaScript scripts (e.g. application/json, importmap).
    std::string type = ToLower(script.attribute("type"));
    if (!type.empty() && type != "text/javascript" &&
        type != "application/javascript" && type != "module") {
      continue;
    }
    // External script: fetch the source and execute it; otherwise run the
    // inline body.
    std::string src = script.attribute("src");
    if (!src.empty()) {
      std::string abs = resolveUrl(src);
      std::vector<std::uint8_t> data;
      if (fetchResource(abs, data)) {
        std::string js(data.begin(), data.end());
        std::cout << "[JS] Loaded external script: " << abs << " (" << js.size()
                  << " bytes)" << std::endl;
        engine.execute(js);
      } else {
        std::cout << "[JS] Failed to load external script: " << abs
                  << std::endl;
      }
    } else {
      engine.execute(script.text());
    }
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
    // For <video>, use the decoded source's intrinsic dimensions/aspect ratio.
    if (name == "video") {
      auto vit = m_videos.find(resolveUrl(node.node.attribute("src")));
      if (vit != m_videos.end() && vit->second && vit->second->width() > 0) {
        int vw = vit->second->width(), vh = vit->second->height();
        if (w == 0 && h == 0) {
          w = vw;
          h = vh;
        } else if (h == 0 && w > 0) {
          h = static_cast<int>(static_cast<long long>(w) * vh / vw);
        } else if (w == 0 && h > 0) {
          w = static_cast<int>(static_cast<long long>(h) * vw / vh);
        }
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
  } else if (name == "input" || name == "textarea" || name == "select" ||
             name == "button") {
    // Form controls are "replaced" elements: give them an intrinsic size and
    // drop their children so the layout/composite treats them as a single box
    // (e.g. a <select>'s <option>s must not stack as separate text lines).
    std::string type = ToLower(node.node.attribute("type"));
    if (name == "input" && (type == "hidden")) {
      node.children.clear();
      return; // no visible box
    }
    int w = 0, h = 0;
    std::string wa = node.node.attribute("width");
    std::string ha = node.node.attribute("height");
    if (!wa.empty()) {
      w = std::atoi(wa.c_str());
    }
    if (!ha.empty()) {
      h = std::atoi(ha.c_str());
    }
    int px = 16;
    int lh = Font::lineHeight(px);
    if (name == "input" && (type == "checkbox" || type == "radio")) {
      if (w == 0) {
        w = 15;
      }
      if (h == 0) {
        h = 15;
      }
    } else if (name == "textarea") {
      int rows = std::atoi(node.node.attribute("rows").c_str());
      int cols = std::atoi(node.node.attribute("cols").c_str());
      if (rows <= 0) {
        rows = 2;
      }
      if (cols <= 0) {
        cols = 20;
      }
      if (w == 0) {
        w = cols * (Font::textWidth("m", px)) + 10;
      }
      if (h == 0) {
        h = rows * lh + 8;
      }
    } else if (name == "button" || name == "select" ||
               (name == "input" &&
                (type == "submit" || type == "button" || type == "reset"))) {
      std::string label =
          (name == "button") ? CollapseWhitespace(node.node.text()) : "";
      if (label.empty()) {
        label = node.node.attribute("value");
      }
      if (label.empty()) {
        label = (type == "reset") ? "Reset" : "Submit";
      }
      if (w == 0) {
        w = Font::textWidth(label, px) + 22;
      }
      if (h == 0) {
        h = lh + 10;
      }
    } else {
      // text / search / email / password / number / submit / etc.
      if (h == 0) {
        h = lh + 12;
      }
      if (w == 0) {
        int size = std::atoi(node.node.attribute("size").c_str());
        w = (size > 0) ? size * Font::textWidth("0", px) + 12 : 200;
      }
    }
    setIfUnset("width", w);
    setIfUnset("height", h);
    node.children.clear();
    return;
  } else if (node.children.empty() && node.node.isElement()) {
    // Text-only leaf: give it a line of height so it is visible and stacks.
    if (!CollapseWhitespace(node.node.text()).empty()) {
      int fontSize =
          ResolveFontSizeForDomNode(node.node, m_sheet, TextSizeFor(name));
      setIfUnset("height", Font::lineHeight(fontSize));
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
    } else if (name == "input" || name == "textarea" || name == "button" ||
               name == "select") {
      std::string type = ToLower(el.attribute("type"));
      if (!(name == "input" && type == "hidden")) {
        int x = static_cast<int>(c.x), y = static_cast<int>(c.y);
        int w = static_cast<int>(c.width), h = static_cast<int>(c.height);
        Paint::Color border{0x76, 0x76, 0x76, 255};
        bool isButton = (name == "button" || type == "submit" ||
                         type == "button" || type == "reset");
        Paint::Color bg =
            isButton ? Paint::Color{0xef, 0xef, 0xef, 255} : kWhite;
        FillRect(canvas, x, y, w, h, bg);
        StrokeRect(canvas, x, y, w, h, border);

        if (name == "input" && (type == "checkbox" || type == "radio")) {
          // Draw a filled mark if checked.
          if (!el.attribute("checked").empty() || el.hasAttribute("checked")) {
            FillRect(canvas, x + 3, y + 3, w - 6, h - 6,
                     Paint::Color{0x33, 0x66, 0xcc, 255});
          }
        } else {
          // Determine the text to show and its colour.
          std::string textToDraw;
          Paint::Color tcol = kBlack;
          if (name == "button" || isButton) {
            textToDraw = CollapseWhitespace(el.text());
            if (textToDraw.empty()) {
              textToDraw = el.attribute("value");
            }
            if (textToDraw.empty()) {
              textToDraw = (type == "reset") ? "Reset" : "Submit";
            }
          } else if (name == "select") {
            // Show the first <option>'s text as the selected value.
            for (const Wrapper::Node &opt : el.getElementsByTagName("option")) {
              textToDraw = CollapseWhitespace(opt.text());
              if (!opt.attribute("selected").empty()) {
                break;
              }
              if (!textToDraw.empty()) {
                break;
              }
            }
          } else {
            // text/search/etc: value, else placeholder (greyed).
            std::string val = el.attribute("value");
            if (name == "textarea") {
              val = CollapseWhitespace(el.text());
            }
            if (!val.empty()) {
              textToDraw = val;
              if (type == "password") {
                textToDraw = std::string(val.size(), '*');
              }
            } else {
              textToDraw = el.attribute("placeholder");
              tcol = Paint::Color{0x75, 0x75, 0x75, 255};
            }
          }
          if (!textToDraw.empty()) {
            int px = 16;
            int ty = y + (h - Font::lineHeight(px)) / 2;
            Font::drawText(canvas, x + 6, ty, textToDraw, tcol, px);
          }
        }
      }
    } else if (box.children.empty()) {
      std::string text = CollapseWhitespace(el.text());
      if (!text.empty()) {
        Paint::Color col;
        std::string colStr =
            ResolveInheritedPropertyForDomNode(el, "color", m_sheet);
        if (colStr.empty() || !Paint::parseColor(colStr, col)) {
          col = kBlack;
        }
        int fontSize =
            ResolveFontSizeForDomNode(el, m_sheet, TextSizeFor(name));
        std::string align = ToLower(
            ResolveInheritedPropertyForDomNode(el, "text-align", m_sheet));
        int tx = static_cast<int>(c.x);
        if (align == "center") {
          int textW = Font::textWidth(text, fontSize);
          tx += std::max(0, (static_cast<int>(c.width) - textW) / 2);
        } else if (align == "right") {
          int textW = Font::textWidth(text, fontSize);
          tx += std::max(0, static_cast<int>(c.width) - textW);
        }
        Font::drawText(canvas, tx, static_cast<int>(c.y), text, col, fontSize);
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
  Layout::LayoutBox box = Layout::layout(m_style, static_cast<float>(width),
                                         static_cast<float>(pageViewportH));
  float contentH = box.dimensions.marginBox().height;
  int pageCanvasH =
      std::max(pageViewportH, static_cast<int>(std::ceil(contentH)));

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
