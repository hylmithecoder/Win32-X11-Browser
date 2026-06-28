#ifndef BASEWINDOW_HPP
#define BASEWINDOW_HPP

#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__linux__) || defined(__gnu_linux__)
#include <X11/Xlib.h>
#endif

namespace DesktopWebview {

class BaseWindow {
public:
  BaseWindow();
  ~BaseWindow();

  void Run();

private:
#if defined(_WIN32)
  HWND hwnd = nullptr;
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam);
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#elif defined(__linux__) || defined(__gnu_linux__)
  Display *display = nullptr;
  Window window = 0;
  void drawText(const std::string &text);
#endif
};

} // namespace DesktopWebview

#endif // BASEWINDOW_HPP