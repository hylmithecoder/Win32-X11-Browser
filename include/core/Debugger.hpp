#pragma once
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <typeinfo>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winsock2.h>
#if defined(INFO)
#undef INFO
#endif
#if defined(WARNING)
#undef WARNING
#endif
#if defined(ERROR)
#undef ERROR
#endif
#endif

#define TITLE "Desktop Webview"

namespace Debug {

enum class LogLevel { INFO, WARNING, CRASH, SUCCESS, ERROR };

// ANSI escape codes for colors
inline const char *getColorCode(LogLevel level) {
  switch (level) {
  case LogLevel::INFO:
    return "\033[1;34m"; // Blue
  case LogLevel::WARNING:
    return "\033[1;33m"; // Yellow
  case LogLevel::CRASH:
  case LogLevel::ERROR:
    return "\033[1;31m"; // Red
  case LogLevel::SUCCESS:
    return "\033[1;32m"; // Green
  default:
    return "\033[0m"; // Reset
  }
}

inline const char *getLevelString(LogLevel level) {
  switch (level) {
  case LogLevel::INFO:
    return "[INFO]";
  case LogLevel::WARNING:
    return "[WARNING]";
  case LogLevel::CRASH:
  case LogLevel::ERROR:
    return "[ERROR]";
  case LogLevel::SUCCESS:
    return "[SUCCESS]";
  default:
    return "[LOG]";
  }
}

// Internal function to handle format string
static inline void LogFormattedWithLocation(const char *format, LogLevel level,
                                            const char *file, int line,
                                            va_list args) {
  const char *colorCode = getColorCode(level);
  const char *levelStr = getLevelString(level);
  const char *reset = "\033[0m";

  std::cout << colorCode << levelStr << " ";

  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), format, args);
  std::cout << buffer << " (" << file << ":" << line << ")" << reset
            << std::endl;
}

static inline void LogWithLocation(const char *format, LogLevel level,
                                   const char *file, int line) {
  const char *colorCode = getColorCode(level);
  const char *levelStr = getLevelString(level);
  const char *reset = "\033[0m";

  std::cout << colorCode << levelStr << " " << format << " (" << file << ":"
            << line << ")" << reset << std::endl;
}

static inline void Log(const std::string &message,
                       LogLevel level = LogLevel::INFO) {
  const char *colorCode = getColorCode(level);
  const char *levelStr = getLevelString(level);
  const char *reset = "\033[0m";

  std::cout << colorCode << levelStr << " " << message << reset << std::endl;
}

template <typename... Args>
static inline void Log(const char *format, LogLevel level, Args... args) {
  const char *colorCode = getColorCode(level);
  const char *levelStr = getLevelString(level);
  const char *reset = "\033[0m";

  std::cout << colorCode << levelStr << " ";

  char buffer[1024];
  if constexpr (sizeof...(args) == 0) {
    snprintf(buffer, sizeof(buffer), "%s", format);
  } else {
    snprintf(buffer, sizeof(buffer), format, args...);
  }
  std::cout << buffer << reset << std::endl;
}

template <typename... Args>
static inline void Log(const char *format, Args... args) {
  Log(format, LogLevel::INFO, args...);
}

template <typename T>
static inline void LogPointer(const std::string &name, T handle,
                              LogLevel level = LogLevel::INFO) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s | Type: %s | Address: %p | Decimal: %lu",
           name.c_str(), typeid(handle).name(), (void *)handle,
           (unsigned long)handle);
  Log(buffer, level);
}

inline bool g_DebugMode = false;
inline bool g_InspectModeActive = false;

struct DebugLabel {
  std::string message;
  std::string file;
  int line;
  double timestamp;
};

inline std::vector<DebugLabel> &GetDebugLabels() {
  static std::vector<DebugLabel> labels;
  return labels;
}

inline double GetTimeSeconds() {
  auto now = std::chrono::steady_clock::now();
  auto duration = now.time_since_epoch();
  return std::chrono::duration<double>(duration).count();
}

inline void AddDebugLabel(const std::string &msg, const char *file, int line) {
  auto &labels = GetDebugLabels();
  double now = GetTimeSeconds();
  for (auto &l : labels) {
    if (l.file == file && l.line == line) {
      l.message = msg;
      l.timestamp = now;
      return;
    }
  }
  labels.push_back({msg, file, line, now});
}

} // namespace Debug

// Namespace alias for compatibility
namespace DesktopWebview {
namespace Debug = ::Debug;
}

// ============================================
// MACRO DEFINITIONS
// ============================================

#define DEBUG_LOG(format, ...)                                                 \
  do {                                                                         \
    const char *colorCode = Debug::getColorCode((Debug::LogLevel::INFO));      \
    const char *levelStr = Debug::getLevelString((Debug::LogLevel::INFO));     \
    const char *reset = "\033[0m";                                             \
    std::cout << colorCode << levelStr << " ";                                 \
    char buffer[1024];                                                         \
    if (std::string(#__VA_ARGS__).empty()) {                                   \
      snprintf(buffer, sizeof(buffer), "%s", (format));                        \
    } else {                                                                   \
      snprintf(buffer, sizeof(buffer), (format), ##__VA_ARGS__);               \
    }                                                                          \
    std::cout << buffer << " (" << __FILE__ << ":" << __LINE__ << ")" << reset \
              << std::endl;                                                    \
  } while (0)

#define DEBUG_LOGF(format, level, ...)                                         \
  do {                                                                         \
    const char *colorCode = Debug::getColorCode((level));                      \
    const char *levelStr = Debug::getLevelString((level));                     \
    const char *reset = "\033[0m";                                             \
    std::cout << colorCode << levelStr << " ";                                 \
    char buffer[1024];                                                         \
    if (std::string(#__VA_ARGS__).empty()) {                                   \
      snprintf(buffer, sizeof(buffer), "%s", (format));                        \
    } else {                                                                   \
      snprintf(buffer, sizeof(buffer), (format), ##__VA_ARGS__);               \
    }                                                                          \
    std::cout << buffer << " (" << __FILE__ << ":" << __LINE__ << ")" << reset \
              << std::endl;                                                    \
  } while (0)

#define DEBUG_LOG_POINTER(name, handle, level)                                 \
  do {                                                                         \
    char buffer[256];                                                          \
    snprintf(buffer, sizeof(buffer),                                           \
             "%s | Type: %s | Address: %p | Decimal: %lu", (name),             \
             typeid(handle).name(), (void *)(handle),                          \
             (unsigned long)(handle));                                         \
    Debug::LogWithLocation(buffer, (level), __FILE__, __LINE__);               \
  } while (0)

#define DEBUG_ASSERT(condition, message, level)                                \
  do {                                                                         \
    if (!(condition)) {                                                        \
      const char *colorCode = Debug::getColorCode((level));                    \
      const char *levelStr = Debug::getLevelString((level));                   \
      const char *reset = "\033[0m";                                           \
      std::cout << colorCode << levelStr << " ASSERTION FAILED: " << (message) \
                << " (" << __FILE__ << ":" << __LINE__ << ")" << reset         \
                << std::endl;                                                  \
      assert((condition));                                                     \
    }                                                                          \
  } while (0)

// ============================================
// WINDOW MESSAGE BOX MACROS
// ============================================

#if defined(_WIN32)

namespace Debug {
static inline void ShowMsgBoxWithLocation(const wchar_t *title,
                                          const std::wstring &message,
                                          const char *file, int line,
                                          UINT type) {
  wchar_t wFile[1024];
  size_t converted = 0;
  mbstowcs_s(&converted, wFile, sizeof(wFile) / sizeof(wchar_t), file,
             _TRUNCATE);

  wchar_t wLine[32];
  swprintf_s(wLine, sizeof(wLine) / sizeof(wchar_t), L"%d", line);

  std::wstring fullMessage = message + L"\n\n(" + wFile + L":" + wLine + L")";
  MessageBoxW(NULL, fullMessage.c_str(), title, type);
}
} // namespace Debug

#define MSGBOX_TITLE L"Desktop Webview"

#define MSGBOX_INFO(message)                                                   \
  Debug::ShowMsgBoxWithLocation(MSGBOX_TITLE, (std::wstring)(message),         \
                                __FILE__, __LINE__,                            \
                                MB_OK | MB_ICONINFORMATION)

#define MSGBOX_SUCCESS(message)                                                \
  Debug::ShowMsgBoxWithLocation(MSGBOX_TITLE, (std::wstring)(message),         \
                                __FILE__, __LINE__,                            \
                                MB_OK | MB_ICONINFORMATION)

#define MSGBOX_WARNING(message)                                                \
  Debug::ShowMsgBoxWithLocation(MSGBOX_TITLE, (std::wstring)(message),         \
                                __FILE__, __LINE__, MB_OK | MB_ICONWARNING)

#define MSGBOX_ERROR(message)                                                  \
  Debug::ShowMsgBoxWithLocation(MSGBOX_TITLE, (std::wstring)(message),         \
                                __FILE__, __LINE__, MB_OK | MB_ICONERROR)

#define MSGBOX_CRASH(message)                                                  \
  do {                                                                         \
    Debug::ShowMsgBoxWithLocation(MSGBOX_TITLE, (std::wstring)(message),       \
                                  __FILE__, __LINE__,                          \
                                  MB_ABORTRETRYIGNORE | MB_ICONERROR);         \
    abort();                                                                   \
  } while (0)

#define MSGBOX_INFOF(title, format, ...)                                       \
  do {                                                                         \
    wchar_t buffer[1024];                                                      \
    swprintf_s(buffer, sizeof(buffer) / sizeof(wchar_t), (format),             \
               ##__VA_ARGS__);                                                 \
    MessageBoxW(NULL, buffer, (title), MB_OK | MB_ICONINFORMATION);            \
  } while (0)

#define MSGBOX_WARNINGF(title, format, ...)                                    \
  do {                                                                         \
    wchar_t buffer[1024];                                                      \
    swprintf_s(buffer, sizeof(buffer) / sizeof(wchar_t), (format),             \
               ##__VA_ARGS__);                                                 \
    MessageBoxW(NULL, buffer, (title), MB_OK | MB_ICONWARNING);                \
  } while (0)

#define MSGBOX_ERRORF(title, format, ...)                                      \
  do {                                                                         \
    wchar_t buffer[1024];                                                      \
    swprintf_s(buffer, sizeof(buffer) / sizeof(wchar_t), (format),             \
               ##__VA_ARGS__);                                                 \
    MessageBoxW(NULL, buffer, (title), MB_OK | MB_ICONERROR);                  \
  } while (0)

#define MSGBOX_INFO_A(title, message)                                          \
  do {                                                                         \
    MessageBoxA(NULL, (message), (title), MB_OK | MB_ICONINFORMATION);         \
  } while (0)

#define MSGBOX_SUCCESS_A(title, message)                                       \
  do {                                                                         \
    MessageBoxA(NULL, (message), (title), MB_OK | MB_ICONINFORMATION);         \
  } while (0)

#define MSGBOX_WARNING_A(title, message)                                       \
  do {                                                                         \
    MessageBoxA(NULL, (message), (title), MB_OK | MB_ICONWARNING);             \
  } while (0)

#define MSGBOX_ERROR_A(title, message)                                         \
  do {                                                                         \
    MessageBoxA(NULL, (message), (title), MB_OK | MB_ICONERROR);               \
  } while (0)

#else

// ============================================
// NON-WINDOWS GUI DIALOGS (GTK / Zenity fallback)
// ============================================

#ifdef DESKTOP_WEBVIEW_WITH_GTK
#include <gtk/gtk.h>

namespace Debug {
static inline void ShowBox(GtkWindow *parent, const char *message,
                           GtkMessageType type = GTK_MESSAGE_INFO) {
  GtkWidget *dialog =
      gtk_message_dialog_new(parent, GTK_DIALOG_DESTROY_WITH_PARENT, type,
                             GTK_BUTTONS_CLOSE, "%s", message);
  gtk_window_set_title(GTK_WINDOW(dialog), TITLE);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static inline void ShowBox(GtkWindow *parent, const std::string &message,
                           GtkMessageType type = GTK_MESSAGE_INFO) {
  ShowBox(parent, message.c_str(), type);
}

static inline gboolean _msgbox_auto_close(gpointer data) {
  GtkWidget *dialog = GTK_WIDGET(data);
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
  return FALSE;
}

#define MSGBOX_AUTO_CLOSE_MS 5000

static inline void _msgbox_pump_events() {
  while (gtk_events_pending()) {
    gtk_main_iteration();
  }
}
} // namespace Debug

#define MSGBOX_INFO(parent, message)                                           \
  do {                                                                         \
    if (!gtk_init_check(NULL, NULL))                                           \
      break;                                                                   \
    GtkWidget *dialog;                                                         \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), "%s\n\n[%s:%d]", (message),     \
             __FILE__, __LINE__);                                              \
    dialog = gtk_message_dialog_new(                                           \
        (parent ? GTK_WINDOW(parent) : NULL), GTK_DIALOG_DESTROY_WITH_PARENT,  \
        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", _msgbox_buf);               \
    if (dialog) {                                                              \
      gtk_window_set_title(GTK_WINDOW(dialog), TITLE);                         \
      guint timeout_id = g_timeout_add(MSGBOX_AUTO_CLOSE_MS,                   \
                                       Debug::_msgbox_auto_close, dialog);     \
      gtk_dialog_run(GTK_DIALOG(dialog));                                      \
      g_source_remove(timeout_id);                                             \
      gtk_widget_destroy(dialog);                                              \
      Debug::_msgbox_pump_events();                                            \
    }                                                                          \
  } while (0)

#define MSGBOX_INFOF(parent, format, ...)                                      \
  do {                                                                         \
    if (!gtk_init_check(NULL, NULL))                                           \
      break;                                                                   \
    GtkWidget *dialog;                                                         \
    char _msgbox_msg[1024];                                                    \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_msg, sizeof(_msgbox_msg), format, ##__VA_ARGS__);         \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), "%s\n\n[%s:%d]", _msgbox_msg,   \
             __FILE__, __LINE__);                                              \
    dialog = gtk_message_dialog_new(                                           \
        (parent ? GTK_WINDOW(parent) : NULL), GTK_DIALOG_DESTROY_WITH_PARENT,  \
        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", _msgbox_buf);               \
    if (dialog) {                                                              \
      gtk_window_set_title(GTK_WINDOW(dialog), TITLE);                         \
      guint timeout_id = g_timeout_add(MSGBOX_AUTO_CLOSE_MS,                   \
                                       Debug::_msgbox_auto_close, dialog);     \
      gtk_dialog_run(GTK_DIALOG(dialog));                                      \
      g_source_remove(timeout_id);                                             \
      gtk_widget_destroy(dialog);                                              \
      Debug::_msgbox_pump_events();                                            \
    }                                                                          \
  } while (0)

#define MSGBOX_ERRORF(parent, format, ...)                                     \
  do {                                                                         \
    if (!gtk_init_check(NULL, NULL))                                           \
      break;                                                                   \
    GtkWidget *dialog;                                                         \
    char _msgbox_msg[1024];                                                    \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_msg, sizeof(_msgbox_msg), format, ##__VA_ARGS__);         \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), "%s\n\n[%s:%d]", _msgbox_msg,   \
             __FILE__, __LINE__);                                              \
    dialog = gtk_message_dialog_new(                                           \
        (parent ? GTK_WINDOW(parent) : NULL), GTK_DIALOG_DESTROY_WITH_PARENT,  \
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", _msgbox_buf);              \
    if (dialog) {                                                              \
      gtk_window_set_title(GTK_WINDOW(dialog), TITLE);                         \
      guint timeout_id = g_timeout_add(MSGBOX_AUTO_CLOSE_MS,                   \
                                       Debug::_msgbox_auto_close, dialog);     \
      gtk_dialog_run(GTK_DIALOG(dialog));                                      \
      g_source_remove(timeout_id);                                             \
      gtk_widget_destroy(dialog);                                              \
      Debug::_msgbox_pump_events();                                            \
    }                                                                          \
  } while (0)

#define MSGBOX_WARNINGF(parent, format, ...)                                   \
  do {                                                                         \
    if (!gtk_init_check(NULL, NULL))                                           \
      break;                                                                   \
    GtkWidget *dialog;                                                         \
    char _msgbox_msg[1024];                                                    \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_msg, sizeof(_msgbox_msg), format, ##__VA_ARGS__);         \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), "%s\n\n[%s:%d]", _msgbox_msg,   \
             __FILE__, __LINE__);                                              \
    dialog = gtk_message_dialog_new(                                           \
        (parent ? GTK_WINDOW(parent) : NULL), GTK_DIALOG_DESTROY_WITH_PARENT,  \
        GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE, "%s", _msgbox_buf);            \
    if (dialog) {                                                              \
      gtk_window_set_title(GTK_WINDOW(dialog), TITLE);                         \
      guint timeout_id = g_timeout_add(MSGBOX_AUTO_CLOSE_MS,                   \
                                       Debug::_msgbox_auto_close, dialog);     \
      gtk_dialog_run(GTK_DIALOG(dialog));                                      \
      g_source_remove(timeout_id);                                             \
      gtk_widget_destroy(dialog);                                              \
      Debug::_msgbox_pump_events();                                            \
    }                                                                          \
  } while (0)

#elif !defined(DESKTOP_WEBVIEW_NO_X11_ALERT)

// ============================================
// NATIVE X11 CUSTOM ALERT (raw Xlib, no GTK)
// ============================================
//
// A self-contained modal alert drawn directly with Xlib -- no toolkit, no GTK
// look. It renders a dark "card" with a level-coloured accent bar, a title, the
// word-wrapped message, a faint file:line footer, and a single rounded OK
// button that lightens on hover. The window is override-redirect (no window-
// manager titlebar) so its appearance is fully ours. It auto-dismisses after
// MSGBOX_AUTO_CLOSE_MS so it never blocks a headless run, and also closes on
// click, Enter, Space or Escape.
//
// Define DESKTOP_WEBVIEW_NO_X11_ALERT to fall back to the zenity path instead.

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cstring>
#include <sys/select.h>

#ifndef MSGBOX_AUTO_CLOSE_MS
#define MSGBOX_AUTO_CLOSE_MS 5000
#endif

namespace Debug {

// Allocate an X pixel from 8-bit-per-channel RGB on the given colormap.
inline unsigned long _x11AllocRGB(Display *dpy, Colormap cmap, int r, int g,
                                  int b) {
  XColor c;
  c.red = (unsigned short)(r * 257);
  c.green = (unsigned short)(g * 257);
  c.blue = (unsigned short)(b * 257);
  c.flags = DoRed | DoGreen | DoBlue;
  if (XAllocColor(dpy, cmap, &c)) {
    return c.pixel;
  }
  return 0; // fall back to black on failure
}

// Per-level accent colour (the bar + the OK button).
inline void _x11AccentFor(LogLevel level, int out[3]) {
  switch (level) {
  case LogLevel::WARNING:
    out[0] = 240;
    out[1] = 180;
    out[2] = 40;
    break; // amber
  case LogLevel::CRASH:
  case LogLevel::ERROR:
    out[0] = 235;
    out[1] = 70;
    out[2] = 70;
    break; // red
  case LogLevel::SUCCESS:
    out[0] = 70;
    out[1] = 200;
    out[2] = 120;
    break; // green
  case LogLevel::INFO:
  default:
    out[0] = 90;
    out[1] = 150;
    out[2] = 245;
    break; // blue
  }
}

inline const char *_x11LevelTitle(LogLevel level) {
  switch (level) {
  case LogLevel::WARNING:
    return "Warning";
  case LogLevel::CRASH:
    return "Fatal Error";
  case LogLevel::ERROR:
    return "Error";
  case LogLevel::SUCCESS:
    return "Success";
  case LogLevel::INFO:
  default:
    return "Information";
  }
}

inline XFontStruct *_x11LoadFont(Display *dpy, const char *const *names) {
  for (int i = 0; names[i]; ++i) {
    if (XFontStruct *f = XLoadQueryFont(dpy, names[i])) {
      return f;
    }
  }
  return XLoadQueryFont(dpy, "fixed");
}

// Greedy word-wrap to at most `maxW` pixels per line; honours embedded '\n'.
inline std::vector<std::string> _x11Wrap(XFontStruct *font,
                                         const std::string &text, int maxW) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t nl = text.find('\n', start);
    std::string para = text.substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    std::string line;
    std::size_t i = 0;
    while (i < para.size()) {
      std::size_t sp = para.find(' ', i);
      std::string word =
          para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
      std::string trial = line.empty() ? word : line + " " + word;
      if (!line.empty() &&
          XTextWidth(font, trial.c_str(), (int)trial.size()) > maxW) {
        lines.push_back(line);
        line = word;
      } else {
        line = trial;
      }
      if (sp == std::string::npos)
        break;
      i = sp + 1;
    }
    lines.push_back(line);
    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
  if (lines.empty())
    lines.push_back("");
  return lines;
}

inline void ShowX11Alert(LogLevel level, const char *windowTitle,
                         const std::string &message, const char *file, int line,
                         int timeoutMs = MSGBOX_AUTO_CLOSE_MS) {
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    // No display available: never lose the message -- print it instead.
    std::cerr << "[ALERT:" << _x11LevelTitle(level) << "] " << message << " ("
              << file << ":" << line << ")" << std::endl;
    return;
  }
  int screen = DefaultScreen(dpy);
  Window root = RootWindow(dpy, screen);
  Colormap cmap = DefaultColormap(dpy, screen);

  const char *bodyFontNames[] = {
      "-*-helvetica-medium-r-normal--13-*-*-*-*-*-iso8859-1",
      "-*-dejavu sans-medium-r-normal--13-*-*-*-*-*-*-*",
      "-misc-fixed-medium-r-normal--13-*-*-*-*-*-*-*", nullptr};
  const char *titleFontNames[] = {
      "-*-helvetica-bold-r-normal--17-*-*-*-*-*-iso8859-1",
      "-*-dejavu sans-bold-r-normal--17-*-*-*-*-*-*-*",
      "-misc-fixed-bold-r-normal--15-*-*-*-*-*-*-*", nullptr};
  XFontStruct *bodyFont = _x11LoadFont(dpy, bodyFontNames);
  XFontStruct *titleFont = _x11LoadFont(dpy, titleFontNames);

  const int pad = 22;
  const int accentW = 6;
  const int maxTextW = 420;
  const char *titleStr = _x11LevelTitle(level);
  std::string footer = std::string(file) + ":" + std::to_string(line);

  std::vector<std::string> lines = _x11Wrap(bodyFont, message, maxTextW);

  int bodyLineH = bodyFont->ascent + bodyFont->descent + 6;
  int titleH = titleFont->ascent + titleFont->descent;

  int textW = XTextWidth(titleFont, titleStr, (int)strlen(titleStr));
  textW =
      std::max(textW, XTextWidth(bodyFont, footer.c_str(), (int)footer.size()));
  for (auto &l : lines) {
    textW = std::max(textW, XTextWidth(bodyFont, l.c_str(), (int)l.size()));
  }

  const int btnW = 100, btnH = 36;
  int contentW = std::max(textW, btnW);
  int winW = std::max(accentW + pad + contentW + pad, 320);

  int titleArea = pad + titleH + 14;
  int bodyArea = (int)lines.size() * bodyLineH;
  int footerArea = bodyFont->ascent + bodyFont->descent + 12;
  int btnArea = 16 + btnH + pad;
  int winH = titleArea + bodyArea + footerArea + btnArea;

  int accent[3];
  _x11AccentFor(level, accent);
  unsigned long cBg = _x11AllocRGB(dpy, cmap, 24, 25, 33);
  unsigned long cBorder = _x11AllocRGB(dpy, cmap, 60, 62, 78);
  unsigned long cTitle =
      _x11AllocRGB(dpy, cmap, accent[0], accent[1], accent[2]);
  unsigned long cBody = _x11AllocRGB(dpy, cmap, 176, 180, 196);
  unsigned long cFooter = _x11AllocRGB(dpy, cmap, 108, 111, 128);
  unsigned long cAccent =
      _x11AllocRGB(dpy, cmap, accent[0], accent[1], accent[2]);
  unsigned long cAccentHi = _x11AllocRGB(
      dpy, cmap, std::min(255, accent[0] + 28), std::min(255, accent[1] + 28),
      std::min(255, accent[2] + 28));
  unsigned long cBtnText = _x11AllocRGB(dpy, cmap, 16, 17, 22);
  (void)cTitle;

  int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
  int wx = std::max(0, (sw - winW) / 2), wy = std::max(0, (sh - winH) / 2);

  XSetWindowAttributes attrs;
  attrs.override_redirect = True; // no WM chrome -> fully custom appearance
  attrs.background_pixel = cBg;
  attrs.border_pixel = cBorder;
  attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     KeyPressMask | PointerMotionMask;
  Window win = XCreateWindow(
      dpy, root, wx, wy, winW, winH, 1, CopyFromParent, InputOutput,
      CopyFromParent,
      CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
  XStoreName(dpy, win, windowTitle ? windowTitle : TITLE);

  GC gc = XCreateGC(dpy, win, 0, nullptr);
  XMapRaised(dpy, win);
  XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  XFlush(dpy);

  int btnX = winW - pad - btnW;
  int btnY = winH - pad - btnH;
  bool hover = false;
  auto inBtn = [&](int px, int py) {
    return px >= btnX && px <= btnX + btnW && py >= btnY && py <= btnY + btnH;
  };

  auto draw = [&]() {
    XSetForeground(dpy, gc, cBg);
    XFillRectangle(dpy, win, gc, 0, 0, winW, winH);
    XSetForeground(dpy, gc, cAccent);
    XFillRectangle(dpy, win, gc, 0, 0, accentW, winH);

    int tx = accentW + pad;
    XSetFont(dpy, gc, titleFont->fid);
    XSetForeground(dpy, gc, cAccent);
    XDrawString(dpy, win, gc, tx, pad + titleFont->ascent, titleStr,
                (int)strlen(titleStr));

    XSetForeground(dpy, gc, cBorder);
    int sepY = pad + titleH + 7;
    XDrawLine(dpy, win, gc, tx, sepY, winW - pad, sepY);

    XSetFont(dpy, gc, bodyFont->fid);
    XSetForeground(dpy, gc, cBody);
    int by = titleArea + bodyFont->ascent;
    for (auto &l : lines) {
      XDrawString(dpy, win, gc, tx, by, l.c_str(), (int)l.size());
      by += bodyLineH;
    }
    XSetForeground(dpy, gc, cFooter);
    XDrawString(dpy, win, gc, tx, by + 6, footer.c_str(), (int)footer.size());

    int r = 7;
    XSetForeground(dpy, gc, hover ? cAccentHi : cAccent);
    XFillRectangle(dpy, win, gc, btnX + r, btnY, btnW - 2 * r, btnH);
    XFillRectangle(dpy, win, gc, btnX, btnY + r, btnW, btnH - 2 * r);
    XFillArc(dpy, win, gc, btnX, btnY, 2 * r, 2 * r, 90 * 64, 90 * 64);
    XFillArc(dpy, win, gc, btnX + btnW - 2 * r, btnY, 2 * r, 2 * r, 0, 90 * 64);
    XFillArc(dpy, win, gc, btnX, btnY + btnH - 2 * r, 2 * r, 2 * r, 180 * 64,
             90 * 64);
    XFillArc(dpy, win, gc, btnX + btnW - 2 * r, btnY + btnH - 2 * r, 2 * r,
             2 * r, 270 * 64, 90 * 64);
    const char *ok = "OK";
    int okW = XTextWidth(bodyFont, ok, 2);
    XSetForeground(dpy, gc, cBtnText);
    XDrawString(dpy, win, gc, btnX + (btnW - okW) / 2,
                btnY + (btnH + bodyFont->ascent - bodyFont->descent) / 2, ok,
                2);
    XFlush(dpy);
  };

  int xfd = ConnectionNumber(dpy);
  bool done = false;
  auto startT = std::chrono::steady_clock::now();
  while (!done) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      switch (ev.type) {
      case Expose:
        draw();
        break;
      case MotionNotify: {
        bool h = inBtn(ev.xmotion.x, ev.xmotion.y);
        if (h != hover) {
          hover = h;
          draw();
        }
      } break;
      case ButtonPress:
        if (ev.xbutton.button == Button1 && inBtn(ev.xbutton.x, ev.xbutton.y)) {
          done = true;
        }
        break;
      case KeyPress: {
        KeySym ks = XLookupKeysym(&ev.xkey, 0);
        if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_Escape ||
            ks == XK_space) {
          done = true;
        }
      } break;
      }
    }
    if (done)
      break;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    struct timeval tv = {0, 50 * 1000};
    select(xfd + 1, &fds, nullptr, nullptr, &tv);
    if (timeoutMs > 0) {
      double el = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - startT)
                      .count();
      if (el >= timeoutMs)
        done = true;
    }
  }

  XUngrabKeyboard(dpy, CurrentTime);
  XFreeGC(dpy, gc);
  if (bodyFont)
    XFreeFont(dpy, bodyFont);
  if (titleFont)
    XFreeFont(dpy, titleFont);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
}

} // namespace Debug

// The `parent` argument is accepted for API parity with the GTK/Win32 paths but
// is unused: the alert is centred on screen as an override-redirect window.
#define MSGBOX_INFO(parent, message)                                           \
  do {                                                                         \
    (void)(parent);                                                            \
    Debug::ShowX11Alert(Debug::LogLevel::INFO, TITLE, std::string(message),    \
                        __FILE__, __LINE__);                                   \
  } while (0)

#define MSGBOX_SUCCESS(parent, message)                                        \
  do {                                                                         \
    (void)(parent);                                                            \
    Debug::ShowX11Alert(Debug::LogLevel::SUCCESS, TITLE, std::string(message), \
                        __FILE__, __LINE__);                                   \
  } while (0)

#define MSGBOX_WARNING(parent, message)                                        \
  do {                                                                         \
    (void)(parent);                                                            \
    Debug::ShowX11Alert(Debug::LogLevel::WARNING, TITLE, std::string(message), \
                        __FILE__, __LINE__);                                   \
  } while (0)

#define MSGBOX_ERROR(parent, message)                                          \
  do {                                                                         \
    (void)(parent);                                                            \
    Debug::ShowX11Alert(Debug::LogLevel::ERROR, TITLE, std::string(message),   \
                        __FILE__, __LINE__);                                   \
  } while (0)

#define MSGBOX_CRASH(parent, message)                                          \
  do {                                                                         \
    (void)(parent);                                                            \
    Debug::ShowX11Alert(Debug::LogLevel::CRASH, TITLE, std::string(message),   \
                        __FILE__, __LINE__);                                   \
    abort();                                                                   \
  } while (0)

#define MSGBOX_INFOF(parent, format, ...)                                      \
  do {                                                                         \
    (void)(parent);                                                            \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), (format), ##__VA_ARGS__);       \
    Debug::ShowX11Alert(Debug::LogLevel::INFO, TITLE,                          \
                        std::string(_msgbox_buf), __FILE__, __LINE__);         \
  } while (0)

#define MSGBOX_WARNINGF(parent, format, ...)                                   \
  do {                                                                         \
    (void)(parent);                                                            \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), (format), ##__VA_ARGS__);       \
    Debug::ShowX11Alert(Debug::LogLevel::WARNING, TITLE,                       \
                        std::string(_msgbox_buf), __FILE__, __LINE__);         \
  } while (0)

#define MSGBOX_ERRORF(parent, format, ...)                                     \
  do {                                                                         \
    (void)(parent);                                                            \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), (format), ##__VA_ARGS__);       \
    Debug::ShowX11Alert(Debug::LogLevel::ERROR, TITLE,                         \
                        std::string(_msgbox_buf), __FILE__, __LINE__);         \
  } while (0)

#else

// Zenity command-line dialog fallback (zero dependencies, works on typical
// Linux)
#define MSGBOX_INFO(parent, message)                                           \
  do {                                                                         \
    std::cout << "[MSGBOX INFO] " << (message) << std::endl;                   \
    std::string cmd = "zenity --info --title=\"" TITLE "\" --text=\"" +        \
                      std::string(message) + "\" --timeout=5 2>/dev/null &";   \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define MSGBOX_INFOF(parent, format, ...)                                      \
  do {                                                                         \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), (format), ##__VA_ARGS__);       \
    std::cout << "[MSGBOX INFO] " << _msgbox_buf << std::endl;                 \
    std::string cmd = "zenity --info --title=\"" TITLE "\" --text=\"" +        \
                      std::string(_msgbox_buf) +                               \
                      "\" --timeout=5 2>/dev/null &";                          \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define MSGBOX_ERRORF(parent, format, ...)                                     \
  do {                                                                         \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), (format), ##__VA_ARGS__);       \
    std::cerr << "[MSGBOX ERROR] " << _msgbox_buf << std::endl;                \
    std::string cmd = "zenity --error --title=\"" TITLE "\" --text=\"" +       \
                      std::string(_msgbox_buf) +                               \
                      "\" --timeout=5 2>/dev/null &";                          \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define MSGBOX_WARNINGF(parent, format, ...)                                   \
  do {                                                                         \
    char _msgbox_buf[2048];                                                    \
    snprintf(_msgbox_buf, sizeof(_msgbox_buf), (format), ##__VA_ARGS__);       \
    std::cerr << "[MSGBOX WARNING] " << _msgbox_buf << std::endl;              \
    std::string cmd = "zenity --warning --title=\"" TITLE "\" --text=\"" +     \
                      std::string(_msgbox_buf) +                               \
                      "\" --timeout=5 2>/dev/null &";                          \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#endif
#endif

// ============================================
// NATIVE NOTIFICATION MACROS
// ============================================

#ifdef DESKTOP_WEBVIEW_WITH_LIBNOTIFY
#include <libnotify/notify.h>

#define NOTIF_INFO(title, message)                                             \
  do {                                                                         \
    if (!notify_init("Desktop Webview"))                                       \
      break;                                                                   \
    NotifyNotification *notif = notify_notification_new(title, message, NULL); \
    notify_notification_show(notif, NULL);                                     \
    g_object_unref(notif);                                                     \
  } while (0)
#define NOTIF_WARNING(title, message)                                          \
  do {                                                                         \
    if (!notify_init("Desktop Webview"))                                       \
      break;                                                                   \
    NotifyNotification *notif = notify_notification_new(title, message, NULL); \
    notify_notification_show(notif, NULL);                                     \
    g_object_unref(notif);                                                     \
  } while (0)
#define NOTIF_ERROR(title, message)                                            \
  do {                                                                         \
    if (!notify_init("Desktop Webview"))                                       \
      break;                                                                   \
    NotifyNotification *notif = notify_notification_new(title, message, NULL); \
    notify_notification_show(notif, NULL);                                     \
    g_object_unref(notif);                                                     \
  } while (0)
#define NOTIF_SUCCESS(title, message)                                          \
  do {                                                                         \
    if (!notify_init("Desktop Webview"))                                       \
      break;                                                                   \
    NotifyNotification *notif = notify_notification_new(title, message, NULL); \
    notify_notification_show(notif, NULL);                                     \
    g_object_unref(notif);                                                     \
  } while (0)
#define NOTIF_LOG(title, message)                                              \
  do {                                                                         \
    if (!notify_init("Desktop Webview"))                                       \
      break;                                                                   \
    NotifyNotification *notif = notify_notification_new(title, message, NULL); \
    notify_notification_show(notif, NULL);                                     \
    g_object_unref(notif);                                                     \
  } while (0)
#define NOTIF_LOGF(title, format, ...)                                         \
  do {                                                                         \
    if (!notify_init("Desktop Webview"))                                       \
      break;                                                                   \
    char _notif_msg[1024];                                                     \
    if (std::string(#__VA_ARGS__).empty()) {                                   \
      snprintf(_notif_msg, sizeof(_notif_msg), "%s", (format));                \
    } else {                                                                   \
      snprintf(_notif_msg, sizeof(_notif_msg), (format), ##__VA_ARGS__);       \
    }                                                                          \
    NotifyNotification *notif =                                                \
        notify_notification_new(title, _notif_msg, NULL);                      \
    notify_notification_show(notif, NULL);                                     \
    g_object_unref(notif);                                                     \
  } while (0)
#define NOTIF_LOG_POINTER(title, handle, level)                                \
  do {                                                                         \
    if (!notify_init("Desktop Webview"))                                       \
      break;                                                                   \
    char _notif_msg[256];                                                      \
    snprintf(_notif_msg, sizeof(_notif_msg),                                   \
             "%s | Type: %s | Address: %p | Decimal: %lu", title,              \
             typeid(handle).name(), (void *)handle, (unsigned long)handle);    \
    NotifyNotification *notif =                                                \
        notify_notification_new(title, _notif_msg, NULL);                      \
    notify_notification_show(notif, NULL);                                     \
    g_object_unref(notif);                                                     \
  } while (0)

#else

// Shell command-line notification fallback (zero dependencies, works on typical
// Linux)
#define NOTIF_INFO(title, message)                                             \
  do {                                                                         \
    std::cout << "[NOTIF INFO] " << (title) << ": " << (message) << std::endl; \
    std::string cmd = "notify-send \"" + std::string(title) + "\" \"" +        \
                      std::string(message) + "\" 2>/dev/null &";               \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define NOTIF_WARNING(title, message)                                          \
  do {                                                                         \
    std::cout << "[NOTIF WARNING] " << (title) << ": " << (message)            \
              << std::endl;                                                    \
    std::string cmd = "notify-send --urgency=normal \"" + std::string(title) + \
                      "\" \"" + std::string(message) + "\" 2>/dev/null &";     \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define NOTIF_ERROR(title, message)                                            \
  do {                                                                         \
    std::cerr << "[NOTIF ERROR] " << (title) << ": " << (message)              \
              << std::endl;                                                    \
    std::string cmd = "notify-send --urgency=critical \"" +                    \
                      std::string(title) + "\" \"" + std::string(message) +    \
                      "\" 2>/dev/null &";                                      \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define NOTIF_SUCCESS(title, message)                                          \
  do {                                                                         \
    std::cout << "[NOTIF SUCCESS] " << (title) << ": " << (message)            \
              << std::endl;                                                    \
    std::string cmd = "notify-send \"" + std::string(title) + "\" \"" +        \
                      std::string(message) + "\" 2>/dev/null &";               \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define NOTIF_LOG(title, message)                                              \
  do {                                                                         \
    std::cout << "[NOTIF LOG] " << (title) << ": " << (message) << std::endl;  \
    std::string cmd = "notify-send \"" + std::string(title) + "\" \"" +        \
                      std::string(message) + "\" 2>/dev/null &";               \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define NOTIF_LOGF(title, format, ...)                                         \
  do {                                                                         \
    char _notif_msg[1024];                                                     \
    if (std::string(#__VA_ARGS__).empty()) {                                   \
      snprintf(_notif_msg, sizeof(_notif_msg), "%s", (format));                \
    } else {                                                                   \
      snprintf(_notif_msg, sizeof(_notif_msg), (format), ##__VA_ARGS__);       \
    }                                                                          \
    std::cout << "[NOTIF LOG] " << (title) << ": " << _notif_msg << std::endl; \
    std::string cmd = "notify-send \"" + std::string(title) + "\" \"" +        \
                      std::string(_notif_msg) + "\" 2>/dev/null &";            \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#define NOTIF_LOG_POINTER(title, handle, level)                                \
  do {                                                                         \
    char _notif_msg[256];                                                      \
    snprintf(_notif_msg, sizeof(_notif_msg),                                   \
             "%s | Type: %s | Address: %p | Decimal: %lu", title,              \
             typeid(handle).name(), (void *)handle, (unsigned long)handle);    \
    std::cout << "[NOTIF POINTER] " << _notif_msg << std::endl;                \
    std::string cmd = "notify-send \"" + std::string(title) + "\" \"" +        \
                      std::string(_notif_msg) + "\" 2>/dev/null &";            \
    int ret = std::system(cmd.c_str());                                        \
    (void)ret;                                                                 \
  } while (0)

#endif

#define DEBUG_LABEL(msg)                                                       \
  do {                                                                         \
    if (Debug::g_DebugMode) {                                                  \
      Debug::AddDebugLabel((msg), __FILE__, __LINE__);                         \
    }                                                                          \
  } while (0)