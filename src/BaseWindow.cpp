#include "../include/BaseWindow.hpp"
#include <iostream>

#if defined(_WIN32)
// =========================================================================
// Windows Win32 API Implementation
// =========================================================================

namespace DesktopWebview {

BaseWindow::BaseWindow() {
  // Windows initialization (if any)
}

BaseWindow::~BaseWindow() {
  // Windows cleanup (if any)
}

LRESULT CALLBACK BaseWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
  BaseWindow *pWindow = nullptr;
  if (msg == WM_NCCREATE) {
    CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT *>(lParam);
    pWindow = reinterpret_cast<BaseWindow *>(pCreate->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWindow));
  } else {
    pWindow =
        reinterpret_cast<BaseWindow *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  }

  if (pWindow) {
    return pWindow->HandleMessage(hwnd, msg, wParam, lParam);
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT BaseWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    // Fill background with white
    RECT rect;
    GetClientRect(hwnd, &rect);
    FillRect(hdc, &rect, (HBRUSH)(COLOR_WINDOW + 1));

    // Set text properties
    SetTextColor(hdc, RGB(0, 0, 0));
    SetBkMode(hdc, TRANSPARENT);

    // Draw Chinese greeting text
    std::wstring chinese_text = L"你好，世界！";
    TextOutW(hdc, 10, 30, chinese_text.c_str(), chinese_text.length());

    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_LBUTTONDOWN:
    // Exit on mouse click
    DestroyWindow(hwnd);
    return 0;
  case WM_KEYDOWN:
    // Exit on key press
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

void BaseWindow::Run() {
  HINSTANCE hInst = GetModuleHandle(nullptr);

  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"BaseWindowClass";
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

  if (!RegisterClassW(&wc)) {
    std::cerr << "Failed to register window class!" << std::endl;
    return;
  }

  hwnd = CreateWindowExW(0, L"BaseWindowClass", L"DesktopWebview Windows",
                         WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, nullptr,
                         nullptr, hInst, this);

  if (!hwnd) {
    std::cerr << "Failed to create window!" << std::endl;
    return;
  }

  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);

  MSG msg = {};
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

} // namespace DesktopWebview

#elif defined(__linux__) || defined(__gnu_linux__)
// =========================================================================
// Linux X11 API Implementation
// =========================================================================
#include <X11/Xatom.h>
#include <vector>

namespace DesktopWebview {

BaseWindow::BaseWindow() : display(nullptr), window(0) {}

BaseWindow::~BaseWindow() {
  // Cleanup if run didn't finish properly
}

void BaseWindow::drawText(const std::string &text) {
  if (!display || !window)
    return;

  // Load a font that has wide character / ISO10646 support
  XFontStruct *font_info = XLoadQueryFont(
      display, "-misc-fixed-medium-r-normal--15-140-75-75-c-70-iso10646-1");

  if (!font_info) {
    font_info = XLoadQueryFont(
        display, "-misc-fixed-medium-r-normal--13-120-100-100-c-60-iso8859-1");
  }

  if (!font_info) {
    font_info = XLoadQueryFont(display, "fixed");
  }

  GC gc = XCreateGC(display, window, 0, NULL);

  Colormap colormap = DefaultColormap(display, DefaultScreen(display));
  XColor black, white;
  XAllocNamedColor(display, colormap, "black", &black, &black);
  XAllocNamedColor(display, colormap, "white", &white, &white);

  XSetForeground(display, gc, black.pixel);
  XSetBackground(display, gc, white.pixel);

  if (font_info) {
    XSetFont(display, gc, font_info->fid);
  }

  // Convert UTF-8 to UTF-16 (XChar2b) for drawing
  std::vector<XChar2b> wide_text;
  for (size_t i = 0; i < text.length();) {
    unsigned char c = text[i];
    unsigned int cp = 0;
    if (c < 0x80) {
      cp = c;
      i += 1;
    } else if ((c & 0xe0) == 0xc0) {
      if (i + 1 < text.length()) {
        cp = ((c & 0x1f) << 6) | (text[i + 1] & 0x3f);
        i += 2;
      } else {
        break;
      }
    } else if ((c & 0xf0) == 0xe0) {
      if (i + 2 < text.length()) {
        cp = ((c & 0x0f) << 12) | ((text[i + 1] & 0x3f) << 6) |
             (text[i + 2] & 0x3f);
        i += 3;
      } else {
        break;
      }
    } else if ((c & 0xf8) == 0xf0) {
      if (i + 3 < text.length()) {
        cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3f) << 12) |
             ((text[i + 2] & 0x3f) << 6) | (text[i + 3] & 0x3f);
        i += 4;
      } else {
        break;
      }
    } else {
      cp = c;
      i += 1;
    }

    XChar2b xchar;
    xchar.byte1 = (cp >> 8) & 0xff;
    xchar.byte2 = cp & 0xff;
    wide_text.push_back(xchar);
  }

  if (!wide_text.empty()) {
    XDrawString16(display, window, gc, 10, 30, wide_text.data(),
                  wide_text.size());
  }

  XFreeGC(display, gc);
  if (font_info) {
    XFreeFont(display, font_info);
  }
}

void BaseWindow::Run() {
  display = XOpenDisplay(NULL);
  if (display == NULL) {
    std::cerr << "Cannot open display" << std::endl;
    return;
  }

  int screen_num = DefaultScreen(display);
  Window root = RootWindow(display, screen_num);

  int x = 100;
  int y = 100;
  int width = 400;
  int height = 300;
  unsigned long black_pixel = BlackPixel(display, screen_num);
  unsigned long white_pixel = WhitePixel(display, screen_num);

  window = XCreateSimpleWindow(display, root, x, y, width, height, 1,
                               black_pixel, white_pixel);

  XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask);

  // Set Window Title
  XStoreName(display, window, "DesktopWebview Linux");

  XMapWindow(display, window);

  XEvent event;
  bool running = true;
  std::string chinese_text = "你好，世界！";

  while (running) {
    XNextEvent(display, &event);

    switch (event.type) {
    case Expose:
      if (event.xexpose.count == 0) {
        drawText(chinese_text);
      }
      break;

    case KeyPress:
      running = false;
      break;

    case ButtonPress:
      running = false;
      break;
    }
  }

  XDestroyWindow(display, window);
  XCloseDisplay(display);
  display = nullptr;
  window = 0;
}

} // namespace DesktopWebview

#endif