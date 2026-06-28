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
#include <winsock2.h>
#include <windows.h>
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