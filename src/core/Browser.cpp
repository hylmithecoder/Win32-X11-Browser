#include "Browser.hpp"
#include "Base64.hpp"
#include "Debugger.hpp"
#include "Documents.hpp"
#include "Font.hpp"
#include "HandlerCssVariable.hpp"
#include "Input.hpp"
#include "JsEngine.hpp"
#include "Net.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

using namespace Debug;

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
      // This walk is its own fresh computeStyle() pass (not the cached,
      // already-var()-resolved m_style tree Layout::styleTree built), so a
      // var(--x) reference here has not been substituted yet -- do that
      // relative to `current` (the node the value actually came from),
      // matching how Layout::styleTree resolves it for every other property.
      std::map<std::string, std::string> single = {{property, it->second}};
      Css::resolveCssVariables(current, sheet, single);
      return single[property];
    }
    current = current.parent();
  }
  return "";
}

int ResolveFontSizeForDomNode(const Wrapper::Node &domNode,
                              const Css::Stylesheet &sheet, int defaultSize) {
  // Walk from domNode upward (self first) looking for the nearest node that
  // declares font-size. This is done as a single self-contained walk (rather
  // than delegating the string lookup to ResolveInheritedPropertyForDomNode
  // and separately calling domNode.parent()) because an em/% value must be
  // resolved relative to the *matched* node's parent, not domNode's parent:
  // domNode is frequently several levels below the declaring ancestor (e.g. a
  // bare text run inside an <h1>), and using domNode's own parent there would
  // silently re-apply the relative factor on top of itself.
  Wrapper::Node current = domNode;
  while (current.valid()) {
    std::map<std::string, std::string> style =
        Css::computeStyle(sheet, current);
    auto it = style.find("font-size");
    if (it != style.end() && !it->second.empty()) {
      const std::string &fs = it->second;
      const char *start = fs.c_str();
      char *end = nullptr;
      double val = std::strtod(start, &end);
      if (end == start) {
        return defaultSize;
      }
      std::string unit = end;
      size_t first = unit.find_first_not_of(" \t\r\n\f");
      unit = (first != std::string::npos) ? unit.substr(first) : "";

      if (unit == "%" || unit == "em") {
        Wrapper::Node parent = current.parent();
        int parentSize = parent.valid()
                             ? ResolveFontSizeForDomNode(parent, sheet, 16)
                             : defaultSize;
        return static_cast<int>(unit == "%" ? (val / 100.0 * parentSize)
                                            : (val * parentSize));
      }
      return static_cast<int>(val); // px (and unrecognised units treated as px)
    }
    current = current.parent();
  }
  return defaultSize;
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Escape HTML special characters so raw text (file contents, JSON, etc.) can
// be embedded inside a generated page without being interpreted as markup.
std::string EscapeHtml(const std::string &text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (unsigned char c : text) {
    if (c == '&') {
      escaped += "&amp;";
    } else if (c == '<') {
      escaped += "&lt;";
    } else if (c == '>') {
      escaped += "&gt;";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

// The colour that should fill the whole page canvas: <html>'s own background
// if it set one, else <body>'s (real browsers propagate body's background to
// the viewport when html doesn't declare one -- CSS Backgrounds §2.11), else
// `fallback`. Reuses the already-cascaded (var()-resolved) StyledNode tree
// Layout::styleTree built rather than a fresh cascade lookup. Without this, a
// page's background only fills <html>/<body>'s own (content-height) box, and
// the rest of a viewport taller than the content shows the raw canvas colour
// instead -- visibly wrong for any page with a coloured background shorter
// than the window.
Paint::Color ResolveCanvasBackground(const Layout::StyledNode &html,
                                     Paint::Color fallback) {
  auto tryColor = [](const std::map<std::string, std::string> &styles,
                     Paint::Color &out) -> bool {
    auto it = styles.find("background-color");
    if (it != styles.end() && Paint::parseColor(it->second, out)) {
      return true;
    }
    it = styles.find("background");
    if (it != styles.end()) {
      std::istringstream ss(it->second);
      std::string tok;
      while (ss >> tok) {
        if (Paint::parseColor(tok, out)) {
          return true;
        }
      }
    }
    return false;
  };
  Paint::Color c;
  if (tryColor(html.styles, c)) {
    return c;
  }
  for (const Layout::StyledNode &child : html.children) {
    if (child.node.isElement() && ToLower(child.node.name()) == "body") {
      if (tryColor(child.styles, c)) {
        return c;
      }
      break;
    }
  }
  return fallback;
}

// Emit one rendered JSON line as its own <div> (a block box, so it reliably
// stacks vertically) rather than relying on a <pre> + literal "\n": this
// engine's inline text layout always collapses runs of whitespace regardless
// of any "white-space" CSS, so newlines inside a single text run would not
// survive to be visible. Leading indentation uses &nbsp; (U+00A0) instead of
// literal spaces for the same reason -- ASCII spaces collapse, a non-breaking
// space does not.
void JsonLine(std::string &out, int indent, const std::string &content) {
  out += "<div>";
  for (int i = 0; i < indent * 2; ++i) {
    out += "&nbsp;";
  }
  out += content + "</div>";
}

// Recursively pretty-print a parsed JSON value as syntax-highlighted,
// indented HTML (keys/strings/numbers/booleans/null each get their own
// <span> class), matching the built-in JSON viewer browsers show for a raw
// .json response. `prefix` is prepended to the value's opening line (a key
// label for object members, empty at the root/for array elements); `suffix`
// is appended after the value's closing line (a trailing "," between
// siblings). Appends <div> lines to `out` rather than returning a string so
// nested calls don't repeatedly reallocate/concatenate.
void JsonToHtml(const nlohmann::json &j, std::string &out, int indent,
                const std::string &prefix, const std::string &suffix) {
  if (j.is_object()) {
    if (j.empty()) {
      JsonLine(out, indent, prefix + "{}" + suffix);
      return;
    }
    JsonLine(out, indent, prefix + "{");
    size_t i = 0, n = j.size();
    for (auto it = j.begin(); it != j.end(); ++it, ++i) {
      std::string keyPrefix =
          "<span class=\"jkey\">\"" + EscapeHtml(it.key()) + "\"</span>: ";
      JsonToHtml(it.value(), out, indent + 1, keyPrefix,
                 (i + 1 < n) ? "," : "");
    }
    JsonLine(out, indent, "}" + suffix);
  } else if (j.is_array()) {
    if (j.empty()) {
      JsonLine(out, indent, prefix + "[]" + suffix);
      return;
    }
    JsonLine(out, indent, prefix + "[");
    for (size_t i = 0; i < j.size(); ++i) {
      JsonToHtml(j[i], out, indent + 1, "", (i + 1 < j.size()) ? "," : "");
    }
    JsonLine(out, indent, "]" + suffix);
  } else if (j.is_string()) {
    JsonLine(out, indent,
             prefix + "<span class=\"jstr\">\"" +
                 EscapeHtml(j.get<std::string>()) + "\"</span>" + suffix);
  } else if (j.is_boolean()) {
    JsonLine(out, indent,
             prefix + "<span class=\"jbool\">" +
                 (j.get<bool>() ? "true" : "false") + "</span>" + suffix);
  } else if (j.is_null()) {
    JsonLine(out, indent,
             prefix + "<span class=\"jnull\">null</span>" + suffix);
  } else {
    // number (int/float)
    JsonLine(out, indent,
             prefix + "<span class=\"jnum\">" + j.dump() + "</span>" + suffix);
  }
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

// True for audio extensions we can decode via ffmpeg.
bool IsAudioExtension(const std::string &ext) {
  return ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac" ||
         ext == "aac" || ext == "m4a";
}

// True for raster image extensions the Image decoder (stb_image) can handle.
bool IsImageExtension(const std::string &ext) {
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
         ext == "bmp" || ext == "ico";
}

// Heuristically decide whether a fetched body is HTML. Used for URLs without a
// usable file extension (e.g. "http://localhost/" or clean routes like
// "/about"), where the renderer cannot key the document type off the extension.
// Sniffing is only applied in that case, so files with an explicit extension
// keep their declared type.
bool LooksLikeHtml(const std::vector<std::uint8_t> &bytes) {
  size_t i = 0;
  // Skip a UTF-8 BOM and any leading whitespace.
  if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
      bytes[2] == 0xBF) {
    i = 3;
  }
  while (i < bytes.size() &&
         std::isspace(static_cast<unsigned char>(bytes[i]))) {
    ++i;
  }
  // Markup must start with '<'.
  if (i >= bytes.size() || bytes[i] != '<') {
    return false;
  }
  // Lowercase a short prefix and look for a common HTML signature.
  size_t n = std::min(bytes.size() - i, static_cast<size_t>(512));
  std::string head;
  head.reserve(n);
  for (size_t k = i; k < i + n; ++k) {
    head +=
        static_cast<char>(std::tolower(static_cast<unsigned char>(bytes[k])));
  }
  static const char *kSignatures[] = {"<!doctype html",
                                      "<html",
                                      "<head",
                                      "<body",
                                      "<meta",
                                      "<title",
                                      "<div",
                                      "<p>",
                                      "<p ",
                                      "<span",
                                      "<table",
                                      "<ul",
                                      "<ol",
                                      "<h1",
                                      "<h2",
                                      "<a ",
                                      "<!--",
                                      "<script"};
  for (const char *sig : kSignatures) {
    if (head.find(sig) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// File type information matching assets/filetypes.json.
struct FileTypeInfo {
  const char *label;
  const char *mime;
  const char *bg; // background color (hex)
  bool inlineDisplay;
};

const FileTypeInfo *LookupFileType(const std::string &ext) {
  static const std::map<std::string, FileTypeInfo> kTypes = {
      {"html", {"HTML page", "text/html", "#ffffff", true}},
      {"htm", {"HTML page", "text/html", "#ffffff", true}},
      {"css", {"CSS stylesheet", "text/css", "#000000ff", true}},
      {"js", {"JavaScript", "application/javascript", "#000000ff", true}},

      {"mp4", {"MP4 video", "video/mp4", "#202020", true}},
      {"webm", {"WebM video", "video/webm", "#202020", true}},
      {"mkv", {"Matroska video", "video/x-matroska", "#202020", true}},
      {"avi", {"AVI video", "video/x-msvideo", "#202020", true}},
      {"mov", {"QuickTime video", "video/quicktime", "#202020", true}},
      {"m4v", {"MPEG-4 video", "video/x-m4v", "#202020", true}},
      {"ogv", {"Ogg video", "video/ogg", "#202020", true}},
      {"rawv", {"Raw video", "video/x-rawv", "#202020", true}},

      {"mp3", {"MP3 audio", "audio/mpeg", "#1a1a2e", false}},
      {"wav", {"WAV audio", "audio/wav", "#1a1a2e", false}},
      {"ogg", {"Ogg audio", "audio/ogg", "#1a1a2e", false}},
      {"flac", {"FLAC audio", "audio/flac", "#1a1a2e", false}},
      {"aac", {"AAC audio", "audio/aac", "#1a1a2e", false}},
      {"m4a", {"MPEG-4 audio", "audio/mp4", "#1a1a2e", false}},

      {"png", {"PNG image", "image/png", "#ffffff", true}},
      {"jpg", {"JPEG image", "image/jpeg", "#ffffff", true}},
      {"jpeg", {"JPEG image", "image/jpeg", "#ffffff", true}},
      {"gif", {"GIF image", "image/gif", "#ffffff", true}},
      {"bmp", {"BMP image", "image/bmp", "#ffffff", true}},
      {"ico", {"Icon", "image/x-icon", "#ffffff", true}},
      {"svg", {"SVG image", "image/svg+xml", "#ffffff", true}},

      {"pdf", {"PDF document", "application/pdf", "#ffffff", true}},
      {"zip", {"ZIP archive", "application/zip", "#333333", false}},
      {"tar", {"TAR archive", "application/x-tar", "#333333", false}},
      {"gz", {"GZip archive", "application/gzip", "#333333", false}},
      {"rar", {"RAR archive", "application/vnd.rar", "#333333", false}},
      {"7z",
       {"7-Zip archive", "application/x-7z-compressed", "#333333", false}},
      {"exe",
       {"Windows executable", "application/x-msdownload", "#2d2d2d", false}},
      {"bin", {"Binary file", "application/octet-stream", "#2d2d2d", false}},
      {"iso", {"Disc image", "application/x-iso9660-image", "#2d2d2d", false}},

      {"md", {"Markdown document", "text/markdown", "#000000", true}},
      {"txt", {"Text file", "text/plain", "#000000", true}},
      {"env", {"Environment config", "text/plain", "#000000", true}},
      {"ini", {"INI config", "text/plain", "#000000", true}},
      {"cfg", {"Config file", "text/plain", "#000000", true}},
      {"conf", {"Config file", "text/plain", "#000000", true}},
      {"log", {"Log file", "text/plain", "#000000", true}},
      {"yml", {"YAML config", "text/yaml", "#000000", true}},
      {"yaml", {"YAML config", "text/yaml", "#000000", true}},
      {"toml", {"TOML config", "text/toml", "#000000", true}},

      {"json", {"JSON data", "application/json", "#1e1e1e", true}},
      {"xml", {"XML data", "application/xml", "#1e1e1e", true}},

      {"sh", {"Shell script", "application/x-sh", "#1e1e1e", true}},
      {"bat", {"Batch script", "application/x-msdos-program", "#1e1e1e", true}},
      {"py", {"Python script", "text/x-python", "#1e1e1e", true}},
      {"cpp", {"C++ source", "text/x-c++src", "#1e1e1e", true}},
      {"hpp", {"C++ header", "text/x-c++hdr", "#1e1e1e", true}},
      {"c", {"C source", "text/x-csrc", "#1e1e1e", true}},
      {"h", {"C header", "text/x-chdr", "#1e1e1e", true}},
  };
  auto it = kTypes.find(ext);
  return it != kTypes.end() ? &it->second : nullptr;
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

// Encode an Image::Bitmap to a BMP byte stream (for data URIs).
std::vector<std::uint8_t> EncodeBmp(const Image::Bitmap &bmp) {
  int rowBytes = ((bmp.width * 24 + 31) / 32) * 4;
  int pixelOff = 14 + 40;
  int fileSize = pixelOff + rowBytes * bmp.height;
  std::vector<std::uint8_t> out(fileSize);

  out[0] = 'B';
  out[1] = 'M';
  auto put32 = [&](size_t off, uint32_t v) {
    out[off] = static_cast<uint8_t>(v & 0xff);
    out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
    out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
  };
  auto put16 = [&](size_t off, uint16_t v) {
    out[off] = static_cast<uint8_t>(v & 0xff);
    out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
  };

  put32(2, fileSize);
  put32(10, pixelOff);
  put32(14, 40);
  put32(18, static_cast<uint32_t>(bmp.width));
  put32(22, static_cast<uint32_t>(bmp.height));
  put16(26, 1);
  put16(28, 24);
  put32(34, rowBytes * bmp.height);

  for (int y = 0; y < bmp.height; ++y) {
    int srcY = bmp.height - 1 - y;
    for (int x = 0; x < bmp.width; ++x) {
      Paint::Color c = bmp.at(x, srcY);
      size_t o = static_cast<size_t>(pixelOff) + y * rowBytes + x * 3;
      out[o] = c.b;
      out[o + 1] = c.g;
      out[o + 2] = c.r;
    }
  }
  return out;
}

} // namespace

Browser::Browser() : m_status("No page loaded") {
  // Try to initialise OpenCL for GPU-accelerated base64. Non-fatal if no GPU.
  Base64::initOpenCL();
  ensureDefaultTab();
}

void Browser::ensureDefaultTab() {
  if (m_tabs.empty()) {
    Tab blank;
    blank.url = "about:blank";
    blank.title = "New Tab";
    m_tabs.push_back(blank);
    m_tabHistory.push_back({HistoryEntry{"about:blank", ""}});
    m_tabHistoryIndex.push_back(0);
    m_activeTab = 0;
  }
}

void Browser::saveTabState(int index) {
  if (index < 0 || index >= static_cast<int>(m_tabs.size()))
    return;
  Tab &t = m_tabs[index];
  t.url = m_currentUrl;
  t.title = m_title;
  t.scrollY = m_scrollY;
  t.pdfPageSizes = m_pdfPageSizes;
  t.pdfZoom = m_pdfZoom;
  t.pdfSidebarOpen = m_pdfSidebarOpen;
  t.pdfCurrentPage = m_pdfCurrentPage;
  t.htmlContent = m_currentHtml;
}

void Browser::restoreTabState(int index) {
  if (index < 0 || index >= static_cast<int>(m_tabs.size()))
    return;
  const Tab &t = m_tabs[index];
  m_currentUrl = t.url;
  m_title = t.title;
  m_scrollY = t.scrollY;
  m_pdfPageSizes = t.pdfPageSizes;
  m_pdfZoom = t.pdfZoom;
  m_pdfSidebarOpen = t.pdfSidebarOpen;
  m_pdfCurrentPage = t.pdfCurrentPage;
  m_currentHtml = t.htmlContent;
  m_urlText = t.url;
  if (m_urlText.rfind("http://", 0) == 0) {
    m_urlText = m_urlText.substr(7);
  }
  m_cursorPos = m_urlText.size();
  if (!m_currentHtml.empty()) {
    loadHtml(m_currentHtml, m_currentUrl);
  } else {
    m_hasDoc = false;
    m_status = "No page loaded";
  }
}

// ---------------------------------------------------------------------------
// URL handling
// ---------------------------------------------------------------------------

std::string Browser::resolveUrl(const std::string &ref) const {
  // DEBUG_LOGF("Browser::resolveUrl(ref: %s)", LogLevel::INFO, ref.c_str());

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
                            std::vector<std::uint8_t> &out,
                            std::string *contentType) const {
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
    if (contentType) {
      *contentType = Net::ExtractContentType(resp);
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

float Browser::getPageYOffset(int pageNum) const {
  float y = 50.0f; // estimated toolbar height + spacing
  for (int i = 0; i < pageNum && i < m_pdfPageSizes.size(); ++i) {
    y += m_pdfPageSizes[i].second * m_pdfZoom + 24.0f;
  }
  return y;
}

void Browser::updatePdfCurrentPageOnScroll() {
  if (m_pdfPageSizes.empty()) {
    return;
  }
  float viewportCenter =
      m_scrollY + (m_lastHeight - kBrowserHeight - kTabBarHeight) / 2.0f;
  int bestPage = 0;
  float y = 50.0f; // toolbar + spacing
  for (size_t i = 0; i < m_pdfPageSizes.size(); ++i) {
    float pageH = m_pdfPageSizes[i].second * m_pdfZoom + 24.0f;
    if (viewportCenter >= y && viewportCenter < y + pageH) {
      bestPage = i;
      break;
    }
    y += pageH;
  }
  if (bestPage != m_pdfCurrentPage) {
    m_pdfCurrentPage = bestPage;
    std::string html = generatePdfHtml(m_currentUrl);
    loadHtml(html, m_currentUrl);
  }
}

std::string Browser::generatePdfHtml(const std::string &target) {
  // Build a clean filename for the title bar
  std::string filename = target;
  auto slashPos = filename.rfind('/');
  if (slashPos != std::string::npos)
    filename = filename.substr(slashPos + 1);
  // URL-decode the filename
  std::string displayName;
  for (size_t k = 0; k < filename.size(); ++k) {
    if (filename[k] == '%' && k + 2 < filename.size()) {
      int hex = 0;
      for (int d = 1; d <= 2; ++d) {
        char c = filename[k + d];
        hex = hex * 16 + (c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : 0);
      }
      displayName += static_cast<char>(hex);
      k += 2;
    } else if (filename[k] == '+') {
      displayName += ' ';
    } else {
      displayName += filename[k];
    }
  }

  int totalPages = static_cast<int>(m_pdfPageSizes.size());

  // Build sidebar thumbnails HTML
  std::string sidebarHtml;
  for (int i = 0; i < totalPages; ++i) {
    int tw = 130;
    int th = static_cast<int>(
        std::round(130.0 * m_pdfPageSizes[i].second / m_pdfPageSizes[i].first));

    bool isActive = (i == m_pdfCurrentPage);

    std::string thumbContainerStyle =
        "display:block; margin:0 8px 14px 8px; text-align:center; "
        "padding:8px 4px; border-radius:6px; cursor:pointer;";

    std::string imgStyle =
        "display:block; margin:0 auto; box-shadow:0 2px 8px rgba(0,0,0,0.5);";

    std::string numStyle = "font-size:11px; margin-top:6px; text-align:center;";

    if (isActive) {
      thumbContainerStyle += " background:#1a2a3a; border:2px solid #4d90fe;";
      imgStyle += " border:1px solid #3a7ad4;";
      numStyle += " color:#4d90fe; font-weight:bold;";
    } else {
      thumbContainerStyle +=
          " background:transparent; border:2px solid transparent;";
      imgStyle += " border:1px solid #444;";
      numStyle += " color:#999;";
    }

    sidebarHtml += "<div style=\"" + thumbContainerStyle +
                   "\">"
                   "<a href=\"action://scroll-to-page/" +
                   std::to_string(i) +
                   "\" style=\"display:block; text-decoration:none;\">"
                   "<img src=\"pdf://page/" +
                   std::to_string(i) + "\" width=\"" + std::to_string(tw) +
                   "\" height=\"" + std::to_string(th) + "\" style=\"" +
                   imgStyle +
                   "\">"
                   "</a>"
                   "<div style=\"" +
                   numStyle + "\">" + std::to_string(i + 1) +
                   "</div>"
                   "</div>";
  }

  // Build main pages HTML
  std::string pagesHtml;
  for (int i = 0; i < totalPages; ++i) {
    int w = static_cast<int>(std::round(m_pdfPageSizes[i].first * m_pdfZoom));
    int h = static_cast<int>(std::round(m_pdfPageSizes[i].second * m_pdfZoom));
    pagesHtml +=
        "<div style=\"margin-bottom:20px; text-align:center; padding:8px 0;\">"
        "<img src=\"pdf://page/" +
        std::to_string(i) + "\" width=\"" + std::to_string(w) + "\" height=\"" +
        std::to_string(h) +
        "\" style=\"box-shadow:0 4px 16px rgba(0,0,0,0.6); border:1px solid "
        "#1a1a1a;\">"
        "<div style=\"color:#888; font-size:11px; margin-top:8px;\">Page " +
        std::to_string(i + 1) + " of " + std::to_string(totalPages) +
        "</div>"
        "</div>";
  }

  std::string zoomPct = std::to_string(static_cast<int>(m_pdfZoom * 100)) + "%";

  // Build layout content (sidebar + main area)
  std::string bodyContent;
  if (m_pdfSidebarOpen) {
    bodyContent +=
        "<td style=\"width:180px; background:#252526; vertical-align:top; "
        "border-right:1px solid #1a1a1a; padding:12px 0; overflow-y:auto; "
        "max-height:calc(100vh - 60px);\">" +
        sidebarHtml + "</td>";
  }
  bodyContent += "<td style=\"background:#3a3a3a; vertical-align:top; "
                 "padding:16px 24px; text-align:center; overflow-y:auto; "
                 "max-height:calc(100vh - 60px);\">" +
                 pagesHtml + "</td>";

  std::string page =
      "<html><head><title>" + displayName +
      "</title>"
      "<style>"
      "*{box-sizing:border-box;margin:0;padding:0;}"
      "body{background:#3a3a3a;font-family:sans-serif;font-size:13px;color:#"
      "d4d4d4;}"
      "a{color:#d4d4d4;text-decoration:none;font-weight:bold;}"
      "a:hover{color:#ffffff;background:#4a4a4a;}"
      "</style>"
      "</head><body>"

      // Toolbar — each <a> gets its own narrow <td> so it doesn't fill a wide
      // cell
      "<div style=\"background:#2d2d2d; border-bottom:1px solid #1a1a1a; "
      "padding:8px 16px; color:#d4d4d4;\">"
      "<table border=\"0\" cellpadding=\"0\" cellspacing=\"0\" "
      "style=\"width:100%;\">"
      "<tr>"
      // Left group: Sidebar button + filename
      "<td style=\"vertical-align:middle; width:60px; padding-right:10px;\">"
      "<a href=\"action://toggle-sidebar\" style=\"background:#4a4a4a; "
      "padding:4px 8px; border-radius:4px; color:#fff; border:1px solid #555; "
      "display:block; text-align:center;\">Sidebar</a>"
      "</td>"
      "<td style=\"vertical-align:middle; overflow:hidden;\">"
      "<span style=\"font-weight:bold; font-size:14px; color:#fff;\">" +
      displayName +
      "</span>"
      "</td>"
      // Spacer
      "<td style=\"width:20px;\"></td>"
      // Center group: Prev + page info + Next
      "<td style=\"vertical-align:middle; width:50px; padding-right:6px;\">"
      "<a href=\"action://prev-page\" style=\"background:#4a4a4a; padding:4px "
      "8px; border-radius:4px; color:#fff; border:1px solid #555; "
      "display:block; text-align:center;\">Prev</a>"
      "</td>"
      "<td style=\"vertical-align:middle; text-align:center; "
      "white-space:nowrap;\">"
      "<span style=\"font-weight:bold;\">Page " +
      std::to_string(m_pdfCurrentPage + 1) + " / " +
      std::to_string(totalPages) +
      "</span>"
      "</td>"
      "<td style=\"vertical-align:middle; width:50px; padding-left:6px;\">"
      "<a href=\"action://next-page\" style=\"background:#4a4a4a; padding:4px "
      "8px; border-radius:4px; color:#fff; border:1px solid #555; "
      "display:block; text-align:center;\">Next</a>"
      "</td>"
      // Spacer
      "<td style=\"width:20px;\"></td>"
      // Right group: Zoom controls
      "<td style=\"vertical-align:middle; width:50px; padding-right:6px;\">"
      "<a href=\"action://zoom-out\" style=\"background:#4a4a4a; padding:4px "
      "8px; border-radius:4px; color:#fff; border:1px solid #555; "
      "display:block; text-align:center;\">Zoom-</a>"
      "</td>"
      "<td style=\"vertical-align:middle; text-align:center; "
      "white-space:nowrap; "
      "padding-right:6px;\">"
      "<span style=\"font-weight:bold;\">" +
      zoomPct +
      "</span>"
      "</td>"
      "<td style=\"vertical-align:middle; width:50px; padding-right:6px;\">"
      "<a href=\"action://zoom-in\" style=\"background:#4a4a4a; padding:4px "
      "8px; border-radius:4px; color:#fff; border:1px solid #555; "
      "display:block; text-align:center;\">Zoom+</a>"
      "</td>"
      "<td style=\"vertical-align:middle; width:50px;\">"
      "<a href=\"action://zoom-reset\" style=\"background:#4a4a4a; padding:4px "
      "8px; border-radius:4px; color:#fff; border:1px solid #555; "
      "display:block; text-align:center;\">Reset</a>"
      "</td>"
      "</tr>"
      "</table>"
      "</div>"

      // Page body
      "<table border=\"0\" cellpadding=\"0\" cellspacing=\"0\" "
      "style=\"width:100%;\">"
      "<tr>" +
      bodyContent +
      "</tr>"
      "</table>"
      "</body></html>";

  return page;
}

// ---------------------------------------------------------------------------
// Navigation / loading
// ---------------------------------------------------------------------------

bool Browser::navigate(const std::string &url) {
  DEBUG_LOGF("Navigating to: %s", LogLevel::INFO, url.c_str());

  std::string target = url;
  bool isAction = (target.rfind("action://", 0) == 0);
  if (isAction) {
    if (target == "action://toggle-sidebar") {
      m_pdfSidebarOpen = !m_pdfSidebarOpen;
    } else if (target == "action://zoom-in") {
      m_pdfZoom = std::min(4.0f, m_pdfZoom * 1.25f);
    } else if (target == "action://zoom-out") {
      m_pdfZoom = std::max(0.25f, m_pdfZoom / 1.25f);
    } else if (target == "action://zoom-reset") {
      m_pdfZoom = 1.0f;
    } else if (target == "action://prev-page") {
      if (m_pdfCurrentPage > 0) {
        m_pdfCurrentPage--;
        m_scrollY = getPageYOffset(m_pdfCurrentPage);
      }
    } else if (target == "action://next-page") {
      if (m_pdfCurrentPage + 1 < m_pdfPageSizes.size()) {
        m_pdfCurrentPage++;
        m_scrollY = getPageYOffset(m_pdfCurrentPage);
      }
    } else if (target.rfind("action://scroll-to-page/", 0) == 0) {
      int pageNum = std::atoi(target.c_str() + 24);
      if (pageNum >= 0 && pageNum < m_pdfPageSizes.size()) {
        m_pdfCurrentPage = pageNum;
        m_scrollY = getPageYOffset(pageNum);
      }
    }

    std::string html = generatePdfHtml(m_currentUrl);
    loadHtml(html, m_currentUrl);
    return true;
  }

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

  // Save current tab state before navigating
  saveTabState(m_activeTab);

  std::vector<std::uint8_t> bytes;
  std::string contentType;
  if (!fetchResource(target, bytes, &contentType)) {
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
    m_currentHtml = page;
    if (!isAction && m_activeTab >= 0 &&
        m_activeTab < static_cast<int>(m_tabHistory.size())) {
      auto &hist = m_tabHistory[m_activeTab];
      int idx = m_tabHistoryIndex[m_activeTab];
      hist.resize(idx + 1);
      hist.push_back({target, page});
      m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
    }
    return loadHtml(page, target);
  }

  // Audio: decode natively + play via AudioOutput (ALSA/WASAPI)
  if (IsAudioExtension(ext)) {
    m_audioPlayer.stop();
    std::string displayName = target;
    auto slash = displayName.rfind('/');
    if (slash != std::string::npos)
      displayName = displayName.substr(slash + 1);

    // Decode first so we can query duration for the HTML page
    bool loaded = m_audioPlayer.loadData(bytes, ext);
    if (!loaded) {
      m_status = "Failed to decode audio: " + target;
      m_hasDoc = false;
      return false;
    }

    double dur = m_audioPlayer.durationSeconds();
    int durMin = static_cast<int>(dur) / 60;
    int durSec = static_cast<int>(dur) % 60;
    char durStr[16];
    std::snprintf(durStr, sizeof(durStr), "%d:%02d", durMin, durSec);
    std::string page =
        "<html><head><title>" + displayName +
        "</title></head>"
        "<body style=\"background:#1a1a2e;color:#d4d4d4;font-family:sans-serif;"
        "text-align:center;padding:60px 20px;margin:0\">"
        "<div style=\"font-size:60px;margin-bottom:16px\">&#9835;</div>"
        "<div style=\"font-size:18px;margin-bottom:16px\">" +
        displayName +
        "</div>"
        // Seek bar
        "<div id=\"seek-bar\" "
        "style=\"width:360px;height:8px;background:#333;"
        "margin:16px auto 4px auto\">"
        "<div id=\"seek-fill\" "
        "style=\"width:0%;height:100%;background:#4a9eff"
        "\"></div>"
        "</div>"
        // Time display
        "<div style=\"width:360px;margin:0 auto\">"
        "<div style=\"float:left;font-size:12px;color:#888\" "
        "id=\"current-time\">0:00</div>"
        "<div style=\"float:right;font-size:12px;color:#888\">" +
        std::string(durStr) +
        "</div>"
        "<div style=\"clear:both\"></div>"
        "</div>"
        // Control buttons
        "<div style=\"margin-top:24px\">"
        "<span id=\"replay\" "
        "style=\"font-size:14px;padding:6px 14px;border:1px solid #555;"
        "background:#2a2a4e;margin:0 8px;cursor:pointer\">"
        "Replay</span>"
        "<span id=\"play-pause\" "
        "style=\"font-size:14px;padding:6px 14px;border:1px solid #555;"
        "background:#2a2a4e;margin:0 8px;cursor:pointer\">"
        "Pause</span>"
        // Stop button
        "<span id=\"stop-btn\" "
        "style=\"font-size:14px;padding:6px 14px;border:1px solid #555;"
        "background:#2a2a4e;margin:0 8px;cursor:pointer\">"
        "Stop</span>"
        "</div>"
        "<div id=\"status\" "
        "style=\"font-size:13px;color:#888;margin-top:20px\">"
        "Playing 0:00 / " +
        std::string(durStr) +
        "</div>"
        "</body></html>";
    m_currentHtml = page;
    if (!isAction && m_activeTab >= 0 &&
        m_activeTab < static_cast<int>(m_tabHistory.size())) {
      auto &hist = m_tabHistory[m_activeTab];
      int idx = m_tabHistoryIndex[m_activeTab];
      hist.resize(idx + 1);
      hist.push_back({target, page});
      m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
    }
    bool ok = loadHtml(page, target);
    if (ok) {
      m_audioPlayer.startPlayback();
      updateAudioPageDom();
    }
    return ok;
  }

  // Check Content-Type header first -- if the server says it's JSON, render as
  // JSON regardless of the URL extension (e.g. api.php returning JSON).
  if (contentType == "application/json") {
    std::string bodyText(reinterpret_cast<const char *>(bytes.data()),
                         bytes.size());
    std::string html = resolveJson(bodyText);
    m_currentHtml = html;
    if (!isAction && m_activeTab >= 0 &&
        m_activeTab < static_cast<int>(m_tabHistory.size())) {
      auto &hist = m_tabHistory[m_activeTab];
      int idx = m_tabHistoryIndex[m_activeTab];
      hist.resize(idx + 1);
      hist.push_back({target, html});
      m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
    }
    return loadHtml(html, target);
  }

  // HTML files are parsed and rendered as documents, not shown as raw text.
  // Extensionless URLs ("/", "/about", ...) have no extension to key off, so we
  // sniff the body and treat it as HTML when it looks like markup.
  if (ext == "html" || ext == "htm" || ext == "php" ||
      (ext.empty() && LooksLikeHtml(bytes))) {
    std::string html(reinterpret_cast<const char *>(bytes.data()),
                     bytes.size());
    m_currentHtml = html;
    // Push to history (skip for action:// internal navigations)
    if (!isAction && m_activeTab >= 0 &&
        m_activeTab < static_cast<int>(m_tabHistory.size())) {
      auto &hist = m_tabHistory[m_activeTab];
      int idx = m_tabHistoryIndex[m_activeTab];
      hist.resize(idx + 1);
      hist.push_back({target, html});
      m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
    }
    return loadHtml(html, target);
  }

  // Directly-opened raster image (e.g. an .png): wrap it in an <img> element so
  // the decode + blit pipeline renders the picture instead of dumping the raw
  // bytes as text.
  if (IsImageExtension(ext)) {
    std::string page = "<html><head><title>" + target +
                       "</title></head><body style=\"margin:0\">"
                       "<img src=\"" +
                       target + "\"></body></html>";
    m_currentHtml = page;
    if (!isAction && m_activeTab >= 0 &&
        m_activeTab < static_cast<int>(m_tabHistory.size())) {
      auto &hist = m_tabHistory[m_activeTab];
      int idx = m_tabHistoryIndex[m_activeTab];
      hist.resize(idx + 1);
      hist.push_back({target, page});
      m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
    }
    return loadHtml(page, target);
  }

  // PDF: render all pages into one tall bitmap and display as an <img>.
  if (ext == "pdf") {
    m_pdfBytes = bytes;
    m_pdfPages.clear();
    m_pdfZoom = 1.0f;
    m_pdfSidebarOpen = true;
    m_pdfCurrentPage = 0;
    if (Documents::pdfPageSizes(bytes, m_pdfPageSizes) &&
        !m_pdfPageSizes.empty()) {
      m_currentUrl = target;
      std::string html = generatePdfHtml(target);
      return loadHtml(html, target);
    }
    // Fall through to generic unknown-file handler below.
  }

  if (ext == "json") {
    DEBUG_LOG("This file is JSON %s", ext.c_str());
    std::string bodyText(reinterpret_cast<const char *>(bytes.data()),
                         bytes.size());
    std::string html = resolveJson(bodyText);
    m_currentHtml = html;
    if (!isAction && m_activeTab >= 0 &&
        m_activeTab < static_cast<int>(m_tabHistory.size())) {
      auto &hist = m_tabHistory[m_activeTab];
      int idx = m_tabHistoryIndex[m_activeTab];
      hist.resize(idx + 1);
      hist.push_back({target, html});
      m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
    }
    return loadHtml(html, target);
  }

  // Check file type configuration for non-video media.
  const FileTypeInfo *ft = LookupFileType(ext);
  if (ft) {
    DEBUG_LOGF("Type file identified: %s", LogLevel::SUCCESS, ft->label);
    if (ft->inlineDisplay) {
      // For text-like files (md, txt, env, code, etc.), show the raw content
      // with a dark background. For other inline types (images, SVG) this path
      // is not reached because they are rendered as <img> in the HTML; we keep
      // the check for future use.
      std::string bodyText(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size());
      std::string escaped = EscapeHtml(bodyText);
      std::string page =
          "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" +
          std::string(ft->label) +
          "</title>"
          "<style>"
          "body { margin:0; background:" +
          std::string(ft->bg) +
          ";"
          " color:#e0e0e0; font-family:monospace; font-size:14px; }"
          "pre { margin:16px; white-space:pre-wrap; word-wrap:break-word; }"
          "</style></head><body><pre>" +
          escaped + "</pre></body></html>";
      m_currentHtml = page;
      if (!isAction && m_activeTab >= 0 &&
          m_activeTab < static_cast<int>(m_tabHistory.size())) {
        auto &hist = m_tabHistory[m_activeTab];
        int idx = m_tabHistoryIndex[m_activeTab];
        hist.resize(idx + 1);
        hist.push_back({target, page});
        m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
      }
      return loadHtml(page, target);
    }
    // Non-inline types (audio, archives, binaries): show a placeholder page.
    std::string page =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" +
        std::string(ft->label) +
        "</title>"
        "<style>"
        "body { margin:40px; background:" +
        std::string(ft->bg) +
        ";"
        " color:#e0e0e0; font-family:sans-serif; text-align:center; }"
        "h1 { font-size:24px; }"
        "p { font-size:14px; color:#aaa; }"
        "</style></head><body>"
        "<h1>" +
        std::string(ft->label) +
        "</h1>"
        "<p>This file type cannot be displayed inline.</p>"
        "<p>" +
        target +
        "</p>"
        "<p>" +
        std::to_string(bytes.size()) +
        " bytes.</p>"
        "</body></html>";
    return loadHtml(page, target);
  }

  // Unknown file type: show raw content with dark background + white text.
  std::string bodyText(reinterpret_cast<const char *>(bytes.data()),
                       bytes.size());
  std::string escaped = EscapeHtml(bodyText);
  std::string page =
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<style>"
      "body { margin:0; background:#000000; color:#ffffff; "
      "font-family:monospace; font-size:14px; }"
      "pre { margin:16px; white-space:pre-wrap; word-wrap:break-word; }"
      "</style></head><body><pre>" +
      escaped + "</pre></body></html>";
  m_currentHtml = page;
  if (!isAction && m_activeTab >= 0 &&
      m_activeTab < static_cast<int>(m_tabHistory.size())) {
    auto &hist = m_tabHistory[m_activeTab];
    int idx = m_tabHistoryIndex[m_activeTab];
    hist.resize(idx + 1);
    hist.push_back({target, page});
    m_tabHistoryIndex[m_activeTab] = static_cast<int>(hist.size()) - 1;
  }
  return loadHtml(page, target);
}

bool Browser::loadHtml(const std::string &html, const std::string &baseUrl) {
  m_currentHtml = html;
  if (!m_doc.parse(html)) {
    m_status = "Failed to parse document";
    m_hasDoc = false;
    return false;
  }
  if (baseUrl != m_currentUrl) {
    m_scrollY = 0.0f;
  }
  m_currentUrl = baseUrl;
  // A new document invalidates any text selection.
  m_selecting = false;
  m_selAnchor = SelPos{};
  m_selFocus = SelPos{};
  if (baseUrl.rfind("http://", 0) == 0) {
    m_urlText = baseUrl.substr(7);
  } else {
    m_urlText = baseUrl;
  }
  m_cursorPos = m_urlText.size();

  if (html.find("pdf://page/") == std::string::npos) {
    m_pdfBytes.clear();
    m_pdfPages.clear();
    m_pdfPageSizes.clear();
  }

  m_title = m_doc.title();
  if (m_activeTab >= 0 && m_activeTab < static_cast<int>(m_tabs.size())) {
    m_tabs[m_activeTab].title = m_title;
  }
  // Stylesheet = UA defaults (assets/ua.css) + link stylesheets + <style>
  // element's text.
  std::string css = LoadUaCss();
  for (const Wrapper::Node &link : m_doc.getElementsByTagName("link")) {
    std::string rel = ToLower(link.attribute("rel"));
    if (rel == "stylesheet") {
      std::string href = link.attribute("href");
      DEBUG_LOG("[Browser] Loading stylesheet: %s (%zu bytes)", href.c_str(),
                href.size());
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
  m_sheet =
      Css::parse(css, m_lastWidth ? static_cast<float>(m_lastWidth) : 960.0f);

  // Fetch and register @font-face fonts so font-family can resolve to them.
  // Sources are tried in order; the first that parses as a font wins (a
  // non-TrueType source such as woff2 fails registerFont and we move on).
  for (const Css::FontFace &face : m_sheet.fontFaces) {
    for (const std::string &src : face.sources) {
      std::string abs = resolveUrl(src);
      std::vector<std::uint8_t> data;
      if (fetchResource(abs, data) &&
          Font::registerFont(face.family, std::move(data))) {
        DEBUG_LOG("[Browser] Registered @font-face '%s' from %s",
                  face.family.c_str(), abs.c_str());
        break;
      }
      DEBUG_LOG("[Browser] @font-face '%s' source failed: %s",
                face.family.c_str(), abs.c_str());
    }
  }

  m_style = Layout::styleTree(m_doc.root(), m_sheet);

  // Preload <img> sources and <video> posters.
  m_images.clear();
  auto preload = [&](const std::string &attr, const char *tag) {
    for (const Wrapper::Node &el : m_doc.getElementsByTagName(tag)) {
      std::string src = el.attribute(attr);
      if (src.empty() || src.rfind("pdf://", 0) == 0) {
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

  // Stop audio on any previously playing videos.
  for (auto &[u, src] : m_videos) {
    if (src) {
      src->stopAudio();
    }
  }
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

  // Start audio playback for any videos that have an audio track.
  for (auto &[u, src] : m_videos) {
    if (src) {
      src->startAudio();
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
  dom.getElementAttribute = [&](const std::string &id,
                                const std::string &attr) -> std::string {
    Wrapper::Node el = m_doc.getElementById(id);
    return el ? el.attribute(attr) : "";
  };
  dom.setElementAttribute = [&](const std::string &id, const std::string &attr,
                                const std::string &value) {
    Wrapper::Node el = m_doc.getElementById(id);
    if (el) {
      el.setAttribute(attr, value);
      m_style = Layout::styleTree(m_doc.root(), m_sheet);
      annotateSizes(m_style);
    }
  };

  // m_jsEngine (not a local) so the page's listeners, timers, and Promise
  // state stay alive after loadHtml returns -- handleClick/render trigger and
  // pump it later. Replacing the old page's engine here (if any) matches real
  // browsers discarding the previous document's JS context on navigation.
  m_jsEngine = std::make_unique<Js::JsEngine>(dom);
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
        DEBUG_LOG("[JS] Loaded external script: %s (%zu bytes)", abs.c_str(),
                  js.size());
        m_jsEngine->execute(js);
      } else {
        DEBUG_LOGF("[JS] Failed to load external script: %s", LogLevel::CRASH,
                   abs.c_str());
      }
    } else {
      m_jsEngine->execute(script.text());
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
    std::string srcVal = node.node.attribute(srcAttr);
    if (srcVal.rfind("pdf://", 0) == std::string::npos) {
      auto it = m_images.find(resolveUrl(srcVal));
      if (it != m_images.end() && it->second.valid()) {
        if (w == 0) {
          w = it->second.width;
        }
        if (h == 0) {
          h = it->second.height;
        }
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
  } else if (Elements::isFormControl(node.node)) {
    // Form controls are "replaced" elements: give them an intrinsic size (from
    // the Elements module -- the single source of truth for control sizing) and
    // drop their children so the layout/composite treats them as a single box
    // (e.g. a <select>'s <option>s must not stack as separate text lines).
    if (Elements::classify(node.node) == Elements::ControlKind::Hidden) {
      node.children.clear();
      return; // no visible box
    }
    int px = ResolveFontSizeForDomNode(node.node, m_sheet, 16);
    Elements::Size sz = Elements::intrinsicSize(node.node, px);
    // Explicit width/height attributes win over the intrinsic default.
    std::string wa = node.node.attribute("width");
    std::string ha = node.node.attribute("height");
    if (!wa.empty()) {
      sz.width = std::atoi(wa.c_str());
    }
    if (!ha.empty()) {
      sz.height = std::atoi(ha.c_str());
    }
    if (sz.width > 0) {
      setIfUnset("width", sz.width);
    }
    if (sz.height > 0) {
      setIfUnset("height", sz.height);
    }
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
  // Bare text-run leaves (isText()) fall through every tag-name check below
  // to the generic text-drawing branch, since Node::name()/Elements::classify
  // both already treat a non-element as "no match" -- no extra dispatch
  // needed beyond widening this gate.
  if (box.node && (box.node->node.isElement() || box.node->node.isText())) {
    const Wrapper::Node &el = box.node->node;
    std::string name = ToLower(el.name());
    const Layout::Rect &c = box.dimensions.content;

    if (name == "img") {
      std::string srcVal = el.attribute("src");
      if (srcVal.rfind("pdf://page/", 0) == 0) {
        int pageNum = std::atoi(srcVal.c_str() + 11);
        float viewportTop = m_scrollY;
        float viewportBottom =
            m_scrollY +
            static_cast<float>(m_lastHeight - kBrowserHeight - kTabBarHeight);
        // Only render and blit if the page intersects the scroll viewport
        if (c.y + c.height >= viewportTop && c.y <= viewportBottom) {
          auto &img = m_pdfPages[pageNum];
          if (!img.valid()) {
            Documents::renderPdfToBitmap(m_pdfBytes, img, pageNum);
          }
          if (img.valid()) {
            BlitScaled(canvas, img, c);
          } else {
            FillRect(canvas, static_cast<int>(c.x), static_cast<int>(c.y),
                     static_cast<int>(c.width), static_cast<int>(c.height),
                     Paint::Color{0xee, 0xee, 0xee, 255});
          }
        }
      } else {
        auto it = m_images.find(resolveUrl(srcVal));
        if (it != m_images.end() && it->second.valid()) {
          BlitScaled(canvas, it->second, c);
        } else {
          // Broken-image placeholder.
          FillRect(canvas, static_cast<int>(c.x), static_cast<int>(c.y),
                   static_cast<int>(c.width), static_cast<int>(c.height),
                   Paint::Color{0xee, 0xee, 0xee, 255});
        }
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
    } else if (Elements::isFormControl(el)) {
      // Form-control rendering lives in the Elements module; box.node->styles
      // is the same already-cascaded (var()-resolved) style map used
      // everywhere else, so a control's CSS styling is honoured consistently
      // with how it would be for any other box.
      int px = ResolveFontSizeForDomNode(el, m_sheet, 16);
      bool focused = m_focusedNode.valid() && m_focusedNode.raw() == el.raw();
      Elements::paint(canvas, el, c, px, focused, box.node->styles);
    } else if (box.children.empty()) {
      TextRun run;
      if (textRunFor(box, run)) {
        int runIdx = m_paintRunCursor++;

        Paint::Color col;
        std::string colStr =
            ResolveInheritedPropertyForDomNode(el, "color", m_sheet);
        if (colStr.empty() || !Paint::parseColor(colStr, col)) {
          col = kBlack;
        }

        // Draw the selection highlight (if this run is within the active
        // selection range) behind the glyphs.
        paintSelection(canvas, runIdx, run);

        Font::drawText(canvas, run.tx, static_cast<int>(run.rect.y), run.text,
                       col, run.fontSize, run.fontFamily);
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

  if (!m_hasDoc) {
    page.clear(kWhite);
    Font::drawText(page, 8, 8, m_status.empty() ? "No page loaded" : m_status,
                   kBlack, 18);
    return page;
  }

  // `height` here is already max(viewport height, content height) (see
  // Browser::render's pageCanvasH), so clearing the full canvas to the
  // resolved html/body background -- rather than a hardcoded white -- makes
  // that colour cover the whole page even where content falls short of the
  // viewport, matching how a real browser paints the canvas.
  page.clear(ResolveCanvasBackground(m_style, kWhite));

  Layout::LayoutBox box = Layout::layout(m_style, static_cast<float>(width),
                                         static_cast<float>(height));
  Paint::DisplayList list = Paint::buildDisplayList(box);
  page.paint(list);
  m_paintRunCursor = 0; // index text runs in paint order for selection
  compositeContent(page, box);
  return page;
}

void Browser::drawTabBar(Paint::Canvas &canvas, int width) {
  Paint::Color tabBg{0x3a, 0x3a, 0x3a, 255};
  Paint::Color activeBg{0x55, 0x55, 0x55, 255};
  Paint::Color inactiveBg{0x3a, 0x3a, 0x3a, 255};
  Paint::Color border{0x22, 0x22, 0x22, 255};
  Paint::Color textWhite{0xff, 0xff, 0xff, 255};
  Paint::Color textGray{0xaa, 0xaa, 0xaa, 255};
  Paint::Color plusBg{0x55, 0x55, 0x55, 255};

  // Full tab bar background
  FillRect(canvas, 0, 0, width, kTabBarHeight, tabBg);

  int tabW = 140;
  int x = 0;
  int closeW = 16;

  for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i) {
    if (x + tabW + closeW + 4 > width - 40)
      break; // leave room for + button

    bool isActive = (i == m_activeTab);
    Paint::Color bg = isActive ? activeBg : inactiveBg;
    FillRect(canvas, x, 0, tabW, kTabBarHeight - 2, bg);
    FillRect(canvas, x, kTabBarHeight - 2, tabW, 2, tabBg); // bottom gap

    // Tab title
    std::string title =
        m_tabs[i].title.empty() ? m_tabs[i].url : m_tabs[i].title;
    if (title.size() > 16) {
      title = title.substr(0, 14) + "..";
    }
    Paint::Color col = isActive ? textWhite : textGray;
    Font::drawText(canvas, x + 6, (kTabBarHeight - Font::lineHeight(12)) / 2,
                   title, col, 12);

    // Close button
    int cbx = x + tabW - closeW - 2;
    int cby = (kTabBarHeight - 14) / 2;
    Font::drawText(canvas, cbx, cby, "x", textGray, 12);

    // Separator
    FillRect(canvas, x + tabW, 2, 1, kTabBarHeight - 4, border);
    x += tabW + 1;
  }

  // + button
  int plusX = x + 4;
  FillRect(canvas, plusX, 4, 24, kTabBarHeight - 8, plusBg);
  Font::drawText(canvas, plusX + 7, (kTabBarHeight - Font::lineHeight(14)) / 2,
                 "+", textWhite, 14);

  // Separator under tab bar
  FillRect(canvas, 0, kTabBarHeight - 1, width, 1, border);
}

void Browser::drawBrowser(Paint::Canvas &canvas, int width) {
  int barY = kTabBarHeight;
  Paint::Color barBg{0xdd, 0xdd, 0xdd, 255};
  Paint::Color border{0x88, 0x88, 0x88, 255};
  Paint::Color btnBg{0xcc, 0xcc, 0xcc, 255};
  Paint::Color btnHover{0xbb, 0xbb, 0xbb, 255};

  FillRect(canvas, 0, barY, width, kBrowserHeight, barBg);

  // Nav buttons: < > ↻  (Back, Forward, Reload)
  int btnY = barY + 6;
  int btnH = kBrowserHeight - 12;
  int btnW = 26;
  int bx = 8;
  auto drawBtn = [&](const char *label, int x) {
    FillRect(canvas, x, btnY, btnW, btnH, btnBg);
    FillRect(canvas, x, btnY, btnW, 1, border);
    FillRect(canvas, x, btnY + btnH - 1, btnW, 1, border);
    FillRect(canvas, x, btnY, 1, btnH, border);
    FillRect(canvas, x + btnW - 1, btnY, 1, btnH, border);
    Font::drawText(canvas, x + 6, btnY + (btnH - Font::lineHeight(14)) / 2,
                   label, kBlack, 14);
  };
  drawBtn("<", bx);
  bx += btnW + 2;
  drawBtn(">", bx);
  bx += btnW + 2;
  drawBtn("R", bx);
  bx += btnW + 6;

  // URL input field
  int ix = bx, iy = btnY, iw = width - ix - 8, ih = btnH;
  if (iw < 1)
    iw = 1;
  FillRect(canvas, ix, iy, iw, ih, kWhite);
  FillRect(canvas, ix, iy, iw, 1, border);
  FillRect(canvas, ix, iy + ih - 1, iw, 1, border);
  FillRect(canvas, ix, iy, 1, ih, border);
  FillRect(canvas, ix + iw - 1, iy, 1, ih, border);

  int px = 14;
  int ty = iy + (ih - Font::lineHeight(px)) / 2;

  // Draw selection highlight in URL bar
  if (m_urlSelAnchor >= 0 && m_urlSelFocus >= 0 &&
      m_urlSelAnchor != m_urlSelFocus) {
    int a = std::min(m_urlSelAnchor, m_urlSelFocus);
    int b = std::max(m_urlSelAnchor, m_urlSelFocus);
    if (a < 0)
      a = 0;
    if (b > static_cast<int>(m_urlText.size()))
      b = static_cast<int>(m_urlText.size());
    int selX1 = ix + 4 + Font::textWidth(m_urlText.substr(0, a), px);
    int selX2 = ix + 4 + Font::textWidth(m_urlText.substr(0, b), px);
    Paint::Color selBg{0x33, 0x99, 0xff, 255};
    FillRect(canvas, selX1, ty - 2, selX2 - selX1, Font::lineHeight(px) + 4,
             selBg);
    // Draw selected text in white, rest in black
    Font::drawText(canvas, ix + 4, ty, m_urlText.substr(0, a), kBlack, px);
    Font::drawText(canvas, selX1, ty, m_urlText.substr(a, b - a), kWhite, px);
    Font::drawText(canvas, selX2, ty, m_urlText.substr(b), kBlack, px);
  } else {
    std::string displayUrl = m_urlText;
    if (m_cursorPos <= displayUrl.size()) {
      displayUrl.insert(m_cursorPos, "|");
    } else {
      displayUrl.push_back('|');
    }
    Font::drawText(canvas, ix + 4, ty, displayUrl, kBlack, px);
  }

  // Separator under the address bar.
  FillRect(canvas, 0, barY + kBrowserHeight - 1, width, 1, border);
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

  // Fire any due setTimeout/setInterval callbacks (and drain the Promise
  // microtask queue after each) before laying out/painting this frame, so a
  // timer-driven DOM mutation shows up in the same frame it becomes due.
  if (m_jsEngine) {
    double elapsedMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - m_startTime)
                           .count();
    m_jsEngine->pump(elapsedMs);
  }

  Paint::Canvas canvas(width, height);
  canvas.clear(kWhite);

  int chromeHeight = kBrowserHeight + kTabBarHeight;
  int pageViewportH = std::max(1, height - chromeHeight);

  if (!m_hasDoc) {
    drawTabBar(canvas, width);
    Font::drawText(canvas, 8, chromeHeight + 8,
                   m_status.empty() ? "No page loaded" : m_status, kBlack, 18);
    drawBrowser(canvas, width);
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

  // Cache dimensions for scrollbar hit-testing
  m_pageCanvasH = pageCanvasH;
  m_pageViewportH = pageViewportH;
  // DEBUG_LOG("[Scrollbar] pageCanvasH=%d pageViewportH=%d maxScrollY=%d "
  //           "contentH=%.1f m_scrollY=%.1f",
  //           pageCanvasH, pageViewportH, maxScrollY, contentH, m_scrollY);

  // 4. Copy the scrolled viewport slice onto canvas
  int scrollbarArea = (maxScrollY > 0) ? kScrollbarWidth : 0;
  int contentWidth = width - scrollbarArea;
  int startY = static_cast<int>(m_scrollY);
  for (int y = 0; y < pageViewportH; ++y) {
    int srcY = startY + y;
    if (srcY >= 0 && srcY < pageCanvasH) {
      for (int x = 0; x < contentWidth; ++x) {
        canvas.blendPixel(x, y + chromeHeight, page.at(x, srcY));
      }
    }
  }

  // 5. Draw scrollbar
  if (maxScrollY > 0) {
    int sbX = width - kScrollbarWidth;
    int sbY = chromeHeight;
    int sbH = pageViewportH;

    // Fill scrollbar area background
    Paint::Color sbBg{0xe0, 0xe0, 0xe0, 255};
    FillRect(canvas, sbX, sbY, kScrollbarWidth, sbH, sbBg);

    // Track border left
    Paint::Color trackBorder{0xaa, 0xaa, 0xaa, 255};
    FillRect(canvas, sbX, sbY, 1, sbH, trackBorder);

    // Thumb: height proportional to viewport/content ratio
    int thumbH = std::max(40, (sbH * sbH) / pageCanvasH);
    int maxThumbY = sbH - thumbH;
    int thumbY = 0;
    if (maxScrollY > 0) {
      thumbY = static_cast<int>(static_cast<float>(maxThumbY) * m_scrollY /
                                static_cast<float>(maxScrollY));
    }
    if (thumbY < 0)
      thumbY = 0;
    if (thumbY > maxThumbY)
      thumbY = maxThumbY;

    // Thumb body
    Paint::Color thumbNorm{0xbb, 0xbb, 0xbb, 255};
    FillRect(canvas, sbX + 2, sbY + thumbY, kScrollbarWidth - 4, thumbH,
             thumbNorm);

    // Thumb highlight
    Paint::Color thumbHi{0xdd, 0xdd, 0xdd, 255};
    FillRect(canvas, sbX + 3, sbY + thumbY + 1, kScrollbarWidth - 6,
             std::max(1, thumbH - 2), thumbHi);
  }

  drawTabBar(canvas, width);
  drawBrowser(canvas, width);
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

const Layout::LayoutBox *FindBoxById(const Layout::LayoutBox &box,
                                     const std::string &id) {
  if (box.node && box.node->node.isElement() &&
      box.node->node.attribute("id") == id) {
    return &box;
  }
  for (const auto &child : box.children) {
    const Layout::LayoutBox *found = FindBoxById(child, id);
    if (found)
      return found;
  }
  return nullptr;
}

// Text-selection highlight colour (light blue, drawn behind selected glyphs).
const Paint::Color kSelectionBg{0xb3, 0xd7, 0xff, 255};

// Character index in `text` nearest to page-x `px`, given the run's left edge
// `tx` and font size. Splits on each glyph's midpoint so the caret snaps to the
// closer side.
int CharIndexAtX(const std::string &text, int fontSize,
                 const std::string &fontFamily, int tx, float px) {
  if (px <= tx) {
    return 0;
  }
  for (size_t i = 1; i <= text.size(); ++i) {
    int w = Font::textWidth(text.substr(0, i), fontSize, fontFamily);
    if (tx + w >= px) {
      int wPrev = Font::textWidth(text.substr(0, i - 1), fontSize, fontFamily);
      float mid = tx + (wPrev + w) / 2.0f;
      return (px < mid) ? static_cast<int>(i - 1) : static_cast<int>(i);
    }
  }
  return static_cast<int>(text.size());
}

} // namespace

void Browser::updateAudioPageDom() {
  if (!m_hasDoc)
    return;
  double dur = m_audioPlayer.durationSeconds();
  double pos = m_audioPlayer.currentPosition();
  double frac = (dur > 0.0) ? std::min(1.0, pos / dur) : 0.0;
  int posMin = static_cast<int>(pos) / 60;
  int posSec = static_cast<int>(pos) % 60;
  char posStr[16];
  std::snprintf(posStr, sizeof(posStr), "%d:%02d", posMin, posSec);
  bool paused = m_audioPlayer.isPaused();
  std::string timeStr(posStr);

  Wrapper::Node statusEl = m_doc.getElementById("status");
  Wrapper::Node timeEl = m_doc.getElementById("current-time");
  Wrapper::Node fillEl = m_doc.getElementById("seek-fill");
  Wrapper::Node ppEl = m_doc.getElementById("play-pause");

  if (statusEl)
    statusEl.setText((paused ? "Paused " : "Playing ") + timeStr);
  if (timeEl)
    timeEl.setText(timeStr);
  if (fillEl) {
    std::string w = std::to_string(static_cast<int>(frac * 100.0)) + "%";
    fillEl.setAttribute("style", "width:" + w +
                                     ";height:100%;background:#4a9eff;"
                                     "border-radius:3px");
  }
  if (ppEl)
    ppEl.setText(paused ? "Play" : "Pause");

  if (statusEl || timeEl || fillEl || ppEl) {
    m_style = Layout::styleTree(m_doc.root(), m_sheet);
    annotateSizes(m_style);
  }
}

bool Browser::handleClick(int x, int y) {
  int chromeHeight = kBrowserHeight + kTabBarHeight;

  // Tab bar clicks
  if (y < kTabBarHeight) {
    int tabW = 140;
    int cx = 0;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i) {
      if (cx + tabW + 16 + 4 > m_lastWidth - 40)
        break;
      // Close button area
      int cbx = cx + tabW - 16 - 2;
      if (x >= cbx && x < cbx + 16 && y >= 8 && y < kTabBarHeight - 8) {
        closeTab(i);
        return true;
      }
      // Tab body
      if (x >= cx && x < cx + tabW) {
        switchTab(i);
        return true;
      }
      cx += tabW + 1;
    }
    // + button
    int plusX = cx + 4;
    if (x >= plusX && x < plusX + 24) {
      newTab("about:blank");
      return true;
    }
    return false;
  }

  // Nav button clicks (inside address bar row)
  if (y >= kTabBarHeight && y < chromeHeight) {
    int btnY = kTabBarHeight + 6;
    int btnH = kBrowserHeight - 12;
    int btnW = 26;
    int bx = 8;
    // Back button
    if (x >= bx && x < bx + btnW && y >= btnY && y < btnY + btnH) {
      goBack();
      return true;
    }
    bx += btnW + 2;
    // Forward button
    if (x >= bx && x < bx + btnW && y >= btnY && y < btnY + btnH) {
      goForward();
      return true;
    }
    bx += btnW + 2;
    // Reload button
    if (x >= bx && x < bx + btnW && y >= btnY && y < btnY + btnH) {
      reload();
      return true;
    }

    // Click in URL bar text area — position cursor
    int urlStartX = bx + btnW + 6; // text starts after reload button + gap
    int urlEndX = m_lastWidth - 8;
    if (x >= urlStartX && x <= urlEndX) {
      int clickInText = x - urlStartX - 4; // 4px text padding
      int px = 14;                         // font size matches drawBrowser
      // Find closest character boundary
      size_t bestPos = 0;
      int bestDist = std::abs(clickInText);
      for (size_t i = 1; i <= m_urlText.size(); ++i) {
        int tw = Font::textWidth(m_urlText.substr(0, i), px);
        int dist = std::abs(clickInText - tw);
        if (dist < bestDist) {
          bestDist = dist;
          bestPos = i;
        }
      }
      m_cursorPos = bestPos;
      m_urlSelAnchor = static_cast<int>(bestPos);
      m_urlSelFocus = static_cast<int>(bestPos);
      m_urlSelecting = true;
      return true;
    }
    return false;
  }

  if (!m_hasDoc) {
    return false;
  }

  // Re-run layout to find where elements are.
  int pageH = std::max(1, m_lastHeight - chromeHeight);
  Layout::LayoutBox box = Layout::layout(
      m_style, static_cast<float>(m_lastWidth), static_cast<float>(pageH));

  float px = static_cast<float>(x);
  float py = static_cast<float>(y - chromeHeight) + m_scrollY;

  const Layout::LayoutBox *found = FindBoxAt(box, px, py);
  if (found && found->node) {
    Wrapper::Node targetEl = found->node->node;

    // Check form submit clicks
    Wrapper::Node sub = targetEl;
    bool isSubmit = false;
    while (sub) {
      if (sub.isElement()) {
        std::string name = ToLower(sub.name());
        std::string type = ToLower(sub.attribute("type"));
        if (name == "button" ||
            (name == "input" && (type == "submit" || type == "button"))) {
          isSubmit = true;
          break;
        }
      }
      sub = sub.parent();
    }

    if (isSubmit) {
      Wrapper::Node form = sub;
      while (form) {
        if (form.isElement() && ToLower(form.name()) == "form") {
          break;
        }
        form = form.parent();
      }
      if (form) {
        std::string formId = form.attribute("id");
        if (formId.empty()) {
          formId = "myForm";
        }
        if (m_jsEngine) {
          m_jsEngine->triggerEvent("submit", formId);
          m_style = Layout::styleTree(m_doc.root(), m_sheet);
          annotateSizes(m_style);
          return true;
        }
      }
    }

    // Handle form-control clicks (checkbox toggle, radio group, select cycle,
    // text focus) via the Elements module.
    if (targetEl.isElement()) {
      auto restyle = [&]() {
        m_style = Layout::styleTree(m_doc.root(), m_sheet);
        annotateSizes(m_style);
      };
      switch (Elements::classify(targetEl)) {
      case Elements::ControlKind::Checkbox:
        Elements::toggleCheckbox(targetEl);
        restyle();
        return true;
      case Elements::ControlKind::Radio:
        Elements::selectRadio(targetEl, m_doc);
        restyle();
        return true;
      case Elements::ControlKind::Select:
        if (Elements::cycleSelect(targetEl)) {
          restyle();
        }
        return true;
      case Elements::ControlKind::Text:
      case Elements::ControlKind::Password:
      case Elements::ControlKind::Textarea:
        m_focusedNode = targetEl;
        return true;
      default:
        break;
      }

      // Fire a generic "click" listener on the nearest id-bearing ancestor
      // (mirrors the id-based dispatch used for audio controls below). Unlike
      // the cases above, this does not return early: a real browser fires
      // the event and then still performs the element's default action
      // (link navigation, etc.), so execution falls through.
      if (m_jsEngine) {
        std::string clickId;
        for (Wrapper::Node walk = targetEl; walk; walk = walk.parent()) {
          if (walk.isElement()) {
            clickId = walk.attribute("id");
            if (!clickId.empty()) {
              break;
            }
          }
        }
        if (!clickId.empty()) {
          m_jsEngine->triggerEvent("click", clickId);
          restyle();
        }
      }
    }

    // Reuse targetEl (captured before any of the handling above), not
    // found->node->node: a click listener dispatched just above can run
    // arbitrary JS that mutates the DOM and rebuilds m_style (e.g. via
    // setElementText), which frees the StyledNode/LayoutBox tree `found`
    // points into. targetEl is a plain xmlNode* copy into m_doc itself, which
    // that rebuild never touches, so it stays valid either way.
    Wrapper::Node n = targetEl;
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

    // Audio player controls (id-based click handling)
    std::string id;
    for (Wrapper::Node walk = targetEl; walk; walk = walk.parent()) {
      if (walk.isElement()) {
        id = walk.attribute("id");
        if (!id.empty())
          break;
      }
    }
    if (!id.empty() && m_audioPlayer.durationSeconds() > 0.0) {
      if (id == "play-pause") {
        if (m_audioPlayer.isPaused())
          m_audioPlayer.resume();
        else
          m_audioPlayer.pause();
        updateAudioPageDom();
        return true;
      }
      if (id == "replay") {
        m_audioPlayer.seek(0.0);
        if (m_audioPlayer.isPaused())
          m_audioPlayer.resume();
        updateAudioPageDom();
        return true;
      }
      if (id == "stop-btn") {
        m_audioPlayer.stop();
        updateAudioPageDom();
        return true;
      }
      if (id == "seek-bar" || id == "seek-progress" || id == "seek-fill" ||
          id == "seek-thumb") {
        // Walk up to the seek-bar container to get its dimensions
        Wrapper::Node seekBar = targetEl;
        while (seekBar && seekBar.attribute("id") != "seek-bar")
          seekBar = seekBar.parent();
        if (seekBar && seekBar.isElement()) {
          // Re-layout to get accurate box dimensions
          Layout::LayoutBox root = Layout::layout(
              m_style, static_cast<float>(m_lastWidth),
              static_cast<float>(std::max(1, m_lastHeight - chromeHeight)));
          const Layout::LayoutBox *seekBox = FindBoxById(root, "seek-bar");
          if (seekBox) {
            const auto &c = seekBox->dimensions.content;
            float relX = px - c.x;
            float frac = std::max(0.0f, std::min(1.0f, relX / c.width));
            double dur = m_audioPlayer.durationSeconds();
            m_audioPlayer.seek(frac * dur);
            updateAudioPageDom();
            return true;
          }
        }
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Text selection
// ---------------------------------------------------------------------------

bool Browser::textRunFor(const Layout::LayoutBox &box, TextRun &out) const {
  if (!box.node || !(box.node->node.isElement() || box.node->node.isText())) {
    return false;
  }
  if (!box.children.empty()) {
    return false;
  }
  const Wrapper::Node &el = box.node->node;
  bool isTextNode = el.isText();
  std::string name = ToLower(el.name()); // "" for a bare text node
  // Replaced elements paint their own content, not selectable text.
  if (name == "img" || name == "video" || name == "input" ||
      name == "textarea" || name == "button" || name == "select") {
    return false;
  }
  // A text-run leaf already carries its final (edge-trimmed) string in
  // box.text (computed once in Layout::BuildLayoutTree); re-deriving it here
  // via a fresh CollapseWhitespace(el.text()) would risk disagreeing with the
  // width Layout already measured it at.
  std::string text = isTextNode ? box.text : CollapseWhitespace(el.text());
  if (text.empty()) {
    return false;
  }
  const Layout::Rect &c = box.dimensions.content;
  int fontSize = ResolveFontSizeForDomNode(el, m_sheet, TextSizeFor(name));
  std::string fontFamily =
      ResolveInheritedPropertyForDomNode(el, "font-family", m_sheet);
  std::string align =
      ToLower(ResolveInheritedPropertyForDomNode(el, "text-align", m_sheet));
  int tx = static_cast<int>(c.x);
  int textW = Font::textWidth(text, fontSize, fontFamily);
  if (align == "center") {
    tx += std::max(0, (static_cast<int>(c.width) - textW) / 2);
  } else if (align == "right") {
    tx += std::max(0, static_cast<int>(c.width) - textW);
  }
  out.text = text;
  out.fontSize = fontSize;
  out.fontFamily = fontFamily;
  out.tx = tx;
  out.rect.x = static_cast<float>(tx);
  out.rect.y = c.y;
  out.rect.width = static_cast<float>(textW);
  out.rect.height = static_cast<float>(Font::lineHeight(fontSize, fontFamily));
  return true;
}

void Browser::collectTextRuns(const Layout::LayoutBox &box,
                              std::vector<TextRun> &runs) const {
  TextRun run;
  if (textRunFor(box, run)) {
    runs.push_back(run);
  }
  for (const Layout::LayoutBox &child : box.children) {
    collectTextRuns(child, runs);
  }
}

std::vector<Browser::TextRun> Browser::layoutTextRuns() const {
  std::vector<TextRun> runs;
  if (!m_hasDoc) {
    return runs;
  }
  int pageH = std::max(1, m_lastHeight - kBrowserHeight - kTabBarHeight);
  Layout::LayoutBox box = Layout::layout(
      m_style, static_cast<float>(m_lastWidth), static_cast<float>(pageH));
  collectTextRuns(box, runs);
  return runs;
}

Browser::SelPos Browser::hitTest(const std::vector<TextRun> &runs, float px,
                                 float py) const {
  SelPos best;
  // 1. Prefer a run whose vertical band contains py; pick the closest in x.
  float bestDx = 0.0f;
  for (size_t i = 0; i < runs.size(); ++i) {
    const TextRun &r = runs[i];
    if (py < r.rect.y || py > r.rect.y + r.rect.height) {
      continue;
    }
    float dx = 0.0f;
    if (px < r.rect.x) {
      dx = r.rect.x - px;
    } else if (px > r.rect.x + r.rect.width) {
      dx = px - (r.rect.x + r.rect.width);
    }
    if (best.run < 0 || dx < bestDx) {
      best.run = static_cast<int>(i);
      best.ch = CharIndexAtX(r.text, r.fontSize, r.fontFamily, r.tx, px);
      bestDx = dx;
    }
  }
  if (best.run >= 0) {
    return best;
  }
  // 2. No run on this line: snap to the vertically nearest run, resolving the
  // column from px so a drag that strays above/below a line still extends
  // horizontally along the closest line.
  float bestDy = 0.0f;
  for (size_t i = 0; i < runs.size(); ++i) {
    const TextRun &r = runs[i];
    float cy = r.rect.y + r.rect.height / 2.0f;
    float dy = std::abs(py - cy);
    if (best.run < 0 || dy < bestDy) {
      best.run = static_cast<int>(i);
      best.ch = CharIndexAtX(r.text, r.fontSize, r.fontFamily, r.tx, px);
      bestDy = dy;
    }
  }
  return best;
}

bool Browser::handleMouseDown(int x, int y) {
  int chromeHeight = kBrowserHeight + kTabBarHeight;

  // Tab bar or nav-button clicks: route through handleClick immediately
  if (y < chromeHeight) {
    m_selecting = false;
    m_scrollbarDragging = false;
    return handleClick(x, y);
  }

  if (!m_hasDoc) {
    m_selecting = false;
    m_scrollbarDragging = false;
    return false;
  }

  // Check scrollbar interaction
  int maxScrollY = std::max(0, m_pageCanvasH - m_pageViewportH);
  if (maxScrollY > 0 && m_lastWidth > kScrollbarWidth) {
    int sbX = m_lastWidth - kScrollbarWidth;
    if (x >= sbX) {
      int sbH = m_pageViewportH;
      int thumbH = std::max(30, (sbH * sbH) / m_pageCanvasH);
      int maxThumbY = sbH - thumbH;
      int thumbY = 0;
      if (maxScrollY > 0) {
        thumbY = static_cast<int>(static_cast<float>(maxThumbY) * m_scrollY /
                                  static_cast<float>(maxScrollY));
      }
      if (thumbY < 0)
        thumbY = 0;
      if (thumbY > maxThumbY)
        thumbY = maxThumbY;

      int localY = y - chromeHeight;

      // Click on thumb: start drag
      if (localY >= thumbY && localY < thumbY + thumbH) {
        m_scrollbarDragging = true;
        m_scrollbarDragOffset = static_cast<float>(localY - thumbY);
        m_selecting = false;
        return false;
      }

      // Click on track: jump to position
      float ratio = static_cast<float>(localY - thumbH / 2) /
                    static_cast<float>(sbH - thumbH);
      if (ratio < 0.0f)
        ratio = 0.0f;
      if (ratio > 1.0f)
        ratio = 1.0f;
      m_scrollY = ratio * static_cast<float>(maxScrollY);
      updatePdfCurrentPageOnScroll();
      m_selecting = false;
      return true;
    }
  }

  float px = static_cast<float>(x);
  float py = static_cast<float>(y - chromeHeight) + m_scrollY;
  std::vector<TextRun> runs = layoutTextRuns();
  SelPos pos = hitTest(runs, px, py);
  m_selAnchor = pos;
  m_selFocus = pos;
  m_selecting = true;
  return true; // clears any previous highlight
}

bool Browser::handleMouseMove(int x, int y) {
  // Handle URL bar drag selection
  if (m_urlSelecting) {
    int chromeHeight = kBrowserHeight + kTabBarHeight;
    int btnW = 26;
    int urlStartX = 8 + btnW + 2 + btnW + 2 + btnW + 6;
    int urlEndX = m_lastWidth - 8;
    int clampedX = x;
    if (clampedX < urlStartX)
      clampedX = urlStartX;
    if (clampedX > urlEndX)
      clampedX = urlEndX;
    int clickInText = clampedX - urlStartX - 4;
    int px = 14;
    size_t bestPos = 0;
    int bestDist = std::abs(clickInText);
    for (size_t i = 1; i <= m_urlText.size(); ++i) {
      int tw = Font::textWidth(m_urlText.substr(0, i), px);
      int dist = std::abs(clickInText - tw);
      if (dist < bestDist) {
        bestDist = dist;
        bestPos = i;
      }
    }
    m_urlSelFocus = static_cast<int>(bestPos);
    m_cursorPos = bestPos;
    return true;
  }

  // Handle scrollbar drag
  if (m_scrollbarDragging && m_hasDoc) {
    int maxScrollY = std::max(0, m_pageCanvasH - m_pageViewportH);
    if (maxScrollY > 0) {
      int chromeHeight = kBrowserHeight + kTabBarHeight;
      int sbH = m_pageViewportH;
      int thumbH = std::max(30, (sbH * sbH) / m_pageCanvasH);
      int maxThumbY = sbH - thumbH;
      float localY =
          static_cast<float>(y - chromeHeight) - m_scrollbarDragOffset;
      float ratio = localY / static_cast<float>(maxThumbY);
      if (ratio < 0.0f)
        ratio = 0.0f;
      if (ratio > 1.0f)
        ratio = 1.0f;
      m_scrollY = ratio * static_cast<float>(maxScrollY);
      updatePdfCurrentPageOnScroll();
    }
    return true;
  }

  if (!m_selecting || !m_hasDoc) {
    return false;
  }
  int chromeHeight = kBrowserHeight + kTabBarHeight;
  float px = static_cast<float>(x);
  float py = static_cast<float>(y - chromeHeight) + m_scrollY;
  std::vector<TextRun> runs = layoutTextRuns();
  m_selFocus = hitTest(runs, px, py);
  return true;
}

bool Browser::handleMouseUp(int x, int y) {
  if (m_urlSelecting) {
    m_urlSelecting = false;
    // No drag: clear selection
    if (m_urlSelAnchor == m_urlSelFocus) {
      m_urlSelAnchor = -1;
      m_urlSelFocus = -1;
    }
    return true;
  }
  if (m_scrollbarDragging) {
    m_scrollbarDragging = false;
    return true;
  }
  if (!m_selecting) {
    return false;
  }
  m_selecting = false;
  // No drag (anchor == focus): treat as a click, e.g. activating a link.
  if (m_selAnchor.run == m_selFocus.run && m_selAnchor.ch == m_selFocus.ch) {
    m_selAnchor = SelPos{};
    m_selFocus = SelPos{};
    return handleClick(x, y);
  }
  return true; // a real selection now exists
}

std::string Browser::selectedText() const {
  if (m_selAnchor.run < 0) {
    return "";
  }
  SelPos s = m_selAnchor, e = m_selFocus;
  if (s.run > e.run || (s.run == e.run && s.ch > e.ch)) {
    std::swap(s, e);
  }
  if (s.run == e.run && s.ch == e.ch) {
    return "";
  }
  std::vector<TextRun> runs = layoutTextRuns();
  if (s.run >= static_cast<int>(runs.size())) {
    return "";
  }
  e.run = std::min(e.run, static_cast<int>(runs.size()) - 1);
  std::string out;
  for (int i = s.run; i <= e.run; ++i) {
    const std::string &t = runs[i].text;
    int from = (i == s.run) ? s.ch : 0;
    int to = (i == e.run) ? e.ch : static_cast<int>(t.size());
    from = std::clamp(from, 0, static_cast<int>(t.size()));
    to = std::clamp(to, 0, static_cast<int>(t.size()));
    if (from > to) {
      std::swap(from, to);
    }
    if (i > s.run) {
      out += "\n";
    }
    out += t.substr(from, to - from);
  }
  return out;
}

void Browser::paintSelection(Paint::Canvas &canvas, int runIdx,
                             const TextRun &run) const {
  if (m_selAnchor.run < 0) {
    return;
  }
  SelPos s = m_selAnchor, e = m_selFocus;
  if (s.run > e.run || (s.run == e.run && s.ch > e.ch)) {
    std::swap(s, e);
  }
  if (s.run == e.run && s.ch == e.ch) {
    return; // empty selection
  }
  if (runIdx < s.run || runIdx > e.run) {
    return;
  }
  int n = static_cast<int>(run.text.size());
  int from = (runIdx == s.run) ? s.ch : 0;
  int to = (runIdx == e.run) ? e.ch : n;
  from = std::clamp(from, 0, n);
  to = std::clamp(to, 0, n);
  if (from > to) {
    std::swap(from, to);
  }
  int x0 = run.tx + Font::textWidth(run.text.substr(0, from), run.fontSize,
                                    run.fontFamily);
  int x1 = run.tx + Font::textWidth(run.text.substr(0, to), run.fontSize,
                                    run.fontFamily);
  FillRect(canvas, x0, static_cast<int>(run.rect.y), std::max(1, x1 - x0),
           static_cast<int>(run.rect.height), kSelectionBg);
}

bool Browser::handleKey(const KeyInput &key) {
  // Route to focused webpage element if valid
  if (m_focusedNode.valid() && !key.ctrl && !key.alt) {
    if (key.kind == KeyInput::Char) {
      Elements::insertChar(m_focusedNode, key.ch);
      m_style = Layout::styleTree(m_doc.root(), m_sheet);
      annotateSizes(m_style);
      return true;
    }
    if (key.kind == KeyInput::Backspace) {
      Elements::backspace(m_focusedNode);
      m_style = Layout::styleTree(m_doc.root(), m_sheet);
      annotateSizes(m_style);
      return true;
    }
    if (key.kind == KeyInput::Enter) {
      m_focusedNode = Wrapper::Node();
      return true;
    }
  }

  // --- Global shortcuts (Ctrl+key) ---
  if (key.ctrl) {
    if (key.kind == KeyInput::Char) {
      if (key.ch == 'c' || key.ch == 'C') {
        std::string sel = urlBarSelectedText();
        if (!sel.empty() && m_copyText) {
          m_copyText(sel);
        }
        return true;
      }
      if (key.ch == 'v' || key.ch == 'V') {
        // Paste from clipboard (async via X11)
        if (m_requestPaste)
          m_requestPaste();
        return true;
      }
      if (key.ch == 'a' || key.ch == 'A') {
        m_urlSelAnchor = 0;
        m_urlSelFocus = static_cast<int>(m_urlText.size());
        m_cursorPos = m_urlText.size();
        return true;
      }
      if (key.ch == 't' || key.ch == 'T') {
        newTab("https://www.google.com/");
        return true;
      }
      if (key.ch == 'w' || key.ch == 'W') {
        closeTab(m_activeTab);
        return true;
      }
      if (key.ch == 'l' || key.ch == 'L') {
        m_urlSelAnchor = 0;
        m_urlSelFocus = static_cast<int>(m_urlText.size());
        m_cursorPos = m_urlText.size();
        return true;
      }
    }
    if (key.kind == KeyInput::Tab) {
      if (key.shift) {
        int prev = m_activeTab - 1;
        if (prev < 0)
          prev = static_cast<int>(m_tabs.size()) - 1;
        switchTab(prev);
      } else {
        int next = m_activeTab + 1;
        if (next >= static_cast<int>(m_tabs.size()))
          next = 0;
        switchTab(next);
      }
      return true;
    }
    if (key.kind == KeyInput::Left) {
      goBack();
      return true;
    }
    if (key.kind == KeyInput::Right) {
      goForward();
      return true;
    }
    return false;
  }

  // --- Alt shortcuts ---
  if (key.alt) {
    if (key.kind == KeyInput::Left) {
      goBack();
      return true;
    }
    if (key.kind == KeyInput::Right) {
      goForward();
      return true;
    }
    return false;
  }

  // --- Shift + Arrow: extend selection ---
  if (key.shift &&
      (key.kind == KeyInput::Left || key.kind == KeyInput::Right)) {
    if (m_urlSelAnchor < 0)
      m_urlSelAnchor = static_cast<int>(m_cursorPos);
    if (key.kind == KeyInput::Left && m_cursorPos > 0)
      m_cursorPos--;
    if (key.kind == KeyInput::Right && m_cursorPos < m_urlText.size())
      m_cursorPos++;
    m_urlSelFocus = static_cast<int>(m_cursorPos);
    return true;
  }
  if (key.shift && (key.kind == KeyInput::Home || key.kind == KeyInput::End)) {
    if (m_urlSelAnchor < 0)
      m_urlSelAnchor = static_cast<int>(m_cursorPos);
    if (key.kind == KeyInput::Home)
      m_cursorPos = 0;
    if (key.kind == KeyInput::End)
      m_cursorPos = m_urlText.size();
    m_urlSelFocus = static_cast<int>(m_cursorPos);
    return true;
  }

  bool hasSelection = (m_urlSelAnchor >= 0 && m_urlSelFocus >= 0 &&
                       m_urlSelAnchor != m_urlSelFocus);

  if (key.kind == KeyInput::Char) {
    if (hasSelection)
      urlBarDeleteSelection();
    m_urlText.insert(m_cursorPos, 1, key.ch);
    m_cursorPos++;
    m_urlSelAnchor = -1;
    m_urlSelFocus = -1;
    return true;
  }

  if (key.kind == KeyInput::Backspace) {
    if (hasSelection) {
      urlBarDeleteSelection();
      return true;
    }
    if (m_cursorPos > 0 && !m_urlText.empty()) {
      m_urlText.erase(m_cursorPos - 1, 1);
      m_cursorPos--;
      return true;
    }
    return false;
  }

  if (key.kind == KeyInput::Delete) {
    if (hasSelection) {
      urlBarDeleteSelection();
      return true;
    }
    if (m_cursorPos < m_urlText.size()) {
      m_urlText.erase(m_cursorPos, 1);
      return true;
    }
    return false;
  }

  if (key.kind == KeyInput::Left) {
    m_urlSelAnchor = -1;
    m_urlSelFocus = -1;
    if (m_cursorPos > 0) {
      m_cursorPos--;
      return true;
    }
    return false;
  }
  if (key.kind == KeyInput::Right) {
    m_urlSelAnchor = -1;
    m_urlSelFocus = -1;
    if (m_cursorPos < m_urlText.size()) {
      m_cursorPos++;
      return true;
    }
    return false;
  }
  if (key.kind == KeyInput::Home) {
    m_urlSelAnchor = -1;
    m_urlSelFocus = -1;
    m_cursorPos = 0;
    return true;
  }
  if (key.kind == KeyInput::End) {
    m_urlSelAnchor = -1;
    m_urlSelFocus = -1;
    m_cursorPos = m_urlText.size();
    return true;
  }

  if (key.kind == KeyInput::Enter) {
    m_urlSelAnchor = -1;
    m_urlSelFocus = -1;
    navigate(m_urlText);
    return true;
  }

  if (key.kind == KeyInput::Up) {
    m_scrollY = std::max(0.0f, m_scrollY - 30.0f);
    updatePdfCurrentPageOnScroll();
    return true;
  }
  if (key.kind == KeyInput::Down) {
    m_scrollY += 30.0f;
    updatePdfCurrentPageOnScroll();
    return true;
  }

  return false;
}

bool Browser::handleScroll(int delta) {
  m_scrollY += delta;
  if (m_scrollY < 0.0f) {
    m_scrollY = 0.0f;
  }
  updatePdfCurrentPageOnScroll();
  return true;
}

// ---------------------------------------------------------------------------
// Navigation: Back / Forward / Reload
// ---------------------------------------------------------------------------

bool Browser::goBack() {
  if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabHistory.size())) {
    return false;
  }
  int &idx = m_tabHistoryIndex[m_activeTab];
  auto &hist = m_tabHistory[m_activeTab];
  if (idx <= 0)
    return false;
  saveTabState(m_activeTab);
  idx--;
  const HistoryEntry &entry = hist[idx];
  m_currentHtml = entry.htmlContent;
  if (!entry.htmlContent.empty()) {
    loadHtml(entry.htmlContent, entry.url);
  } else {
    navigate(entry.url);
  }
  return true;
}

bool Browser::goForward() {
  if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabHistory.size())) {
    return false;
  }
  int &idx = m_tabHistoryIndex[m_activeTab];
  auto &hist = m_tabHistory[m_activeTab];
  if (idx >= static_cast<int>(hist.size()) - 1)
    return false;
  saveTabState(m_activeTab);
  idx++;
  const HistoryEntry &entry = hist[idx];
  m_currentHtml = entry.htmlContent;
  if (!entry.htmlContent.empty()) {
    loadHtml(entry.htmlContent, entry.url);
  } else {
    navigate(entry.url);
  }
  return true;
}

bool Browser::reload() {
  if (!m_currentHtml.empty()) {
    return loadHtml(m_currentHtml, m_currentUrl);
  }
  if (!m_currentUrl.empty()) {
    return navigate(m_currentUrl);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Tab management
// ---------------------------------------------------------------------------

bool Browser::newTab(const std::string &url) {
  saveTabState(m_activeTab);
  Tab blank;
  blank.url = url;
  blank.title = "New Tab";
  m_tabs.push_back(blank);
  m_tabHistory.push_back({HistoryEntry{url, ""}});
  m_tabHistoryIndex.push_back(0);
  m_activeTab = static_cast<int>(m_tabs.size()) - 1;
  if (url == "about:blank" || url.empty()) {
    m_currentUrl = url;
    m_urlText = "";
    m_cursorPos = 0;
    m_hasDoc = false;
    m_status = "No page loaded";
    m_scrollY = 0;
    m_title.clear();
  } else {
    navigate(url);
  }
  return true;
}

bool Browser::closeTab(int index) {
  if (index < 0 || index >= static_cast<int>(m_tabs.size()))
    return false;
  if (m_tabs.size() == 1)
    return false; // don't close last tab
  m_tabs.erase(m_tabs.begin() + index);
  m_tabHistory.erase(m_tabHistory.begin() + index);
  m_tabHistoryIndex.erase(m_tabHistoryIndex.begin() + index);
  if (m_activeTab >= static_cast<int>(m_tabs.size())) {
    m_activeTab = static_cast<int>(m_tabs.size()) - 1;
  }
  restoreTabState(m_activeTab);
  return true;
}

bool Browser::switchTab(int index) {
  if (index < 0 || index >= static_cast<int>(m_tabs.size()))
    return false;
  if (index == m_activeTab)
    return true;
  saveTabState(m_activeTab);
  m_activeTab = index;
  restoreTabState(index);
  return true;
}

// ---------------------------------------------------------------------------
// URL bar selection + clipboard
// ---------------------------------------------------------------------------

std::string Browser::urlBarSelectedText() const {
  if (m_urlSelAnchor < 0 || m_urlSelFocus < 0)
    return "";
  int a = std::min(m_urlSelAnchor, m_urlSelFocus);
  int b = std::max(m_urlSelAnchor, m_urlSelFocus);
  if (a == b)
    return "";
  if (a < 0)
    a = 0;
  if (b > static_cast<int>(m_urlText.size()))
    b = static_cast<int>(m_urlText.size());
  return m_urlText.substr(a, b - a);
}

void Browser::urlBarDeleteSelection() {
  if (m_urlSelAnchor < 0 || m_urlSelFocus < 0)
    return;
  int a = std::min(m_urlSelAnchor, m_urlSelFocus);
  int b = std::max(m_urlSelAnchor, m_urlSelFocus);
  if (a == b)
    return;
  if (a < 0)
    a = 0;
  if (b > static_cast<int>(m_urlText.size()))
    b = static_cast<int>(m_urlText.size());
  m_urlText.erase(a, b - a);
  m_cursorPos = a;
  m_urlSelAnchor = -1;
  m_urlSelFocus = -1;
}

void Browser::handlePaste(const std::string &text) {
  // Replace selection or insert at cursor
  urlBarDeleteSelection();
  if (m_cursorPos > m_urlText.size())
    m_cursorPos = m_urlText.size();
  m_urlText.insert(m_cursorPos, text);
  m_cursorPos += text.size();
}

string Browser::resolveJson(string data) {
  std::string bodyHtml;
  try {
    nlohmann::json parsed = nlohmann::json::parse(data);
    JsonToHtml(parsed, bodyHtml, 0, "", "");
  } catch (const nlohmann::json::parse_error &e) {
    // Malformed JSON: fall back to the escaped raw text (still safe to
    // embed) plus the parser's own error, one <div> per line, rather than
    // showing a blank page.
    JsonLine(bodyHtml, 0,
             "<span class=\"jerr\">Invalid JSON: " + EscapeHtml(e.what()) +
                 "</span>");
    std::istringstream lines(data);
    std::string line;
    while (std::getline(lines, line)) {
      JsonLine(bodyHtml, 0, EscapeHtml(line));
    }
  }
  return "<!DOCTYPE html><html><head><meta "
         "charset=\"utf-8\"><title>JSON</title>"
         "<style>"
         "body { margin:0; padding:16px; background-color:#1e1e1e; "
         "color:#d4d4d4; "
         "font-family:monospace; font-size:14px; }"
         "div { white-space:pre; }"
         ".jkey { color:#9cdcfe; }"
         ".jstr { color:#ce9178; }"
         ".jnum { color:#b5cea8; }"
         ".jbool, .jnull { color:#569cd6; }"
         ".jerr { color:#f48771; }"
         "</style></head><body>" +
         bodyHtml + "</body></html>";
}

} // namespace Browser
} // namespace DesktopWebview
