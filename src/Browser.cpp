#include "../include/Browser.hpp"
#include "../include/Base64.hpp"
#include "../include/Debugger.hpp"
#include "../include/Documents.hpp"
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

      {"pdf", {"PDF document", "application/pdf", "#ffffff", false}},
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
  DEBUG_LOGF("Navigating to: %s", LogLevel::INFO, url.c_str());

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

  // HTML files are parsed and rendered as documents, not shown as raw text.
  // Extensionless URLs ("/", "/about", ...) have no extension to key off, so we
  // sniff the body and treat it as HTML when it looks like markup.
  if (ext == "html" || ext == "htm" || ext == "php" ||
      (ext.empty() && LooksLikeHtml(bytes))) {
    std::string html(reinterpret_cast<const char *>(bytes.data()),
                     bytes.size());
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
    return loadHtml(page, target);
  }

  // PDF: render the first page to a bitmap and display as an <img>.
  if (ext == "pdf") {
    Image::Bitmap bmp;
    if (Documents::renderPdfToBitmap(bytes, bmp, 0)) {
      std::vector<std::uint8_t> bmpFile = EncodeBmp(bmp);
      std::string dataUri = "data:image/bmp;base64," +
                            Base64::encode(bmpFile.data(), bmpFile.size());
      int totalPages = Documents::pdfPageCount(bytes);
      std::string page =
          "<html><head><title>" + target +
          "</title>"
          "<style>body{margin:0;background:#ccc;text-align:center;"
          "font-family:sans-serif;font-size:13px;color:#333;}"
          "img{display:block;margin:0 auto;}</style>"
          "</head><body>"
          "<div style=\"padding:6px;background:#f5f5f5;border-bottom:1px "
          "solid #aaa;\">" +
          std::to_string(totalPages) + " halaman &mdash; " + target +
          "</div>"
          "<img src=\"" +
          dataUri + "\"></body></html>";
      return loadHtml(page, target);
    }
    // Fall through to generic unknown-file handler below.
  }

  // Check file type configuration for non-video media.
  const FileTypeInfo *ft = LookupFileType(ext);
  if (ft) {
    if (ft->inlineDisplay) {
      // For text-like files (md, txt, env, code, etc.), show the raw content
      // with a dark background. For other inline types (images, SVG) this path
      // is not reached because they are rendered as <img> in the HTML; we keep
      // the check for future use.
      std::string bodyText(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size());
      // Escape HTML special characters for safe display.
      std::string escaped;
      escaped.reserve(bodyText.size());
      for (unsigned char c : bodyText) {
        if (c == '&')
          escaped += "&amp;";
        else if (c == '<')
          escaped += "&lt;";
        else if (c == '>')
          escaped += "&gt;";
        else
          escaped += c;
      }
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
  std::string escaped;
  escaped.reserve(bodyText.size());
  for (unsigned char c : bodyText) {
    if (c == '&')
      escaped += "&amp;";
    else if (c == '<')
      escaped += "&lt;";
    else if (c == '>')
      escaped += "&gt;";
    else
      escaped += c;
  }
  std::string page =
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<style>"
      "body { margin:0; background:#000000; color:#ffffff; "
      "font-family:monospace; font-size:14px; }"
      "pre { margin:16px; white-space:pre-wrap; word-wrap:break-word; }"
      "</style></head><body><pre>" +
      escaped + "</pre></body></html>";
  return loadHtml(page, target);
}

bool Browser::loadHtml(const std::string &html, const std::string &baseUrl) {
  if (!m_doc.parse(html)) {
    m_status = "Failed to parse document";
    m_hasDoc = false;
    return false;
  }
  m_currentUrl = baseUrl;
  m_scrollY = 0.0f;
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

  // Stylesheet = UA defaults (assets/ua.css) + link stylesheets + <style>
  // element's text.
  std::string css = LoadUaCss();
  for (const Wrapper::Node &link : m_doc.getElementsByTagName("link")) {
    std::string rel = ToLower(link.attribute("rel"));
    if (rel == "stylesheet") {
      std::string href = link.attribute("href");
      DEBUG_LOG("[Browser] Loading stylesheet: %s", href.c_str());
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
                       col, run.fontSize);
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
  m_paintRunCursor = 0; // index text runs in paint order for selection
  compositeContent(page, box);
  return page;
}

void Browser::drawBrowser(Paint::Canvas &canvas, int width) {
  Paint::Color barBg{0xdd, 0xdd, 0xdd, 255};
  Paint::Color border{0x88, 0x88, 0x88, 255};

  FillRect(canvas, 0, 0, width, kBrowserHeight, barBg);

  int ix = 8, iy = 6, iw = width - 16, ih = kBrowserHeight - 12;
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

  // Separator under the browser.
  FillRect(canvas, 0, kBrowserHeight - 1, width, 1, border);
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

  int pageViewportH = std::max(1, height - kBrowserHeight);

  if (!m_hasDoc) {
    Font::drawText(canvas, 8, kBrowserHeight + 8,
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

  // 4. Copy the scrolled viewport slice onto canvas
  int startY = static_cast<int>(m_scrollY);
  for (int y = 0; y < pageViewportH; ++y) {
    int srcY = startY + y;
    if (srcY >= 0 && srcY < pageCanvasH) {
      for (int x = 0; x < width; ++x) {
        canvas.blendPixel(x, y + kBrowserHeight, page.at(x, srcY));
      }
    }
  }

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

// Text-selection highlight colour (light blue, drawn behind selected glyphs).
const Paint::Color kSelectionBg{0xb3, 0xd7, 0xff, 255};

// Character index in `text` nearest to page-x `px`, given the run's left edge
// `tx` and font size. Splits on each glyph's midpoint so the caret snaps to the
// closer side.
int CharIndexAtX(const std::string &text, int fontSize, int tx, float px) {
  if (px <= tx) {
    return 0;
  }
  for (size_t i = 1; i <= text.size(); ++i) {
    int w = Font::textWidth(text.substr(0, i), fontSize);
    if (tx + w >= px) {
      int wPrev = Font::textWidth(text.substr(0, i - 1), fontSize);
      float mid = tx + (wPrev + w) / 2.0f;
      return (px < mid) ? static_cast<int>(i - 1) : static_cast<int>(i);
    }
  }
  return static_cast<int>(text.size());
}

} // namespace

bool Browser::handleClick(int x, int y) {
  if (y < kBrowserHeight) {
    return false; // clicked in browser area
  }
  if (!m_hasDoc) {
    return false;
  }

  // Re-run layout to find where elements are.
  int pageH = std::max(1, m_lastHeight - kBrowserHeight);
  Layout::LayoutBox box = Layout::layout(
      m_style, static_cast<float>(m_lastWidth), static_cast<float>(pageH));

  float px = static_cast<float>(x);
  float py = static_cast<float>(y - kBrowserHeight) + m_scrollY;

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

// ---------------------------------------------------------------------------
// Text selection
// ---------------------------------------------------------------------------

bool Browser::textRunFor(const Layout::LayoutBox &box, TextRun &out) const {
  if (!box.node || !box.node->node.isElement()) {
    return false;
  }
  if (!box.children.empty()) {
    return false;
  }
  const Wrapper::Node &el = box.node->node;
  std::string name = ToLower(el.name());
  // Replaced elements paint their own content, not selectable text.
  if (name == "img" || name == "video" || name == "input" ||
      name == "textarea" || name == "button" || name == "select") {
    return false;
  }
  std::string text = CollapseWhitespace(el.text());
  if (text.empty()) {
    return false;
  }
  const Layout::Rect &c = box.dimensions.content;
  int fontSize = ResolveFontSizeForDomNode(el, m_sheet, TextSizeFor(name));
  std::string align =
      ToLower(ResolveInheritedPropertyForDomNode(el, "text-align", m_sheet));
  int tx = static_cast<int>(c.x);
  int textW = Font::textWidth(text, fontSize);
  if (align == "center") {
    tx += std::max(0, (static_cast<int>(c.width) - textW) / 2);
  } else if (align == "right") {
    tx += std::max(0, static_cast<int>(c.width) - textW);
  }
  out.text = text;
  out.fontSize = fontSize;
  out.tx = tx;
  out.rect.x = static_cast<float>(tx);
  out.rect.y = c.y;
  out.rect.width = static_cast<float>(textW);
  out.rect.height = static_cast<float>(Font::lineHeight(fontSize));
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
  int pageH = std::max(1, m_lastHeight - kBrowserHeight);
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
      best.ch = CharIndexAtX(r.text, r.fontSize, r.tx, px);
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
      best.ch = CharIndexAtX(r.text, r.fontSize, r.tx, px);
      bestDy = dy;
    }
  }
  return best;
}

bool Browser::handleMouseDown(int x, int y) {
  if (y < kBrowserHeight || !m_hasDoc) {
    m_selecting = false;
    m_selAnchor = SelPos{};
    m_selFocus = SelPos{};
    return false;
  }
  float px = static_cast<float>(x);
  float py = static_cast<float>(y - kBrowserHeight) + m_scrollY;
  std::vector<TextRun> runs = layoutTextRuns();
  SelPos pos = hitTest(runs, px, py);
  m_selAnchor = pos;
  m_selFocus = pos;
  m_selecting = true;
  return true; // clears any previous highlight
}

bool Browser::handleMouseMove(int x, int y) {
  if (!m_selecting || !m_hasDoc) {
    return false;
  }
  float px = static_cast<float>(x);
  float py = static_cast<float>(y - kBrowserHeight) + m_scrollY;
  std::vector<TextRun> runs = layoutTextRuns();
  m_selFocus = hitTest(runs, px, py);
  return true;
}

bool Browser::handleMouseUp(int x, int y) {
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
  int x0 = run.tx + Font::textWidth(run.text.substr(0, from), run.fontSize);
  int x1 = run.tx + Font::textWidth(run.text.substr(0, to), run.fontSize);
  FillRect(canvas, x0, static_cast<int>(run.rect.y), std::max(1, x1 - x0),
           static_cast<int>(run.rect.height), kSelectionBg);
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
