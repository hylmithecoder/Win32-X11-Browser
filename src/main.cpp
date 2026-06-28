#include "../include/BaseWindow.hpp"
#include "../include/Browser.hpp"
#include "../include/Net.hpp"

#include <string>

using namespace DesktopWebview;

int main(int argc, char **argv) {
  Net::Init();

  // Start URL: first CLI argument, else a local server. Type a new URL into the
  // address bar and press Enter to navigate.
  std::string startUrl = (argc > 1) ? argv[1] : "http://localhost/";

  Browser::Browser browser;
  browser.navigate(startUrl);

  BaseWindow window;
  window.SetRenderCallback([&browser](int width, int height) {
    return browser.render(width, height);
  });

  window.SetKeyCallback([&browser](const BaseWindow::Key &k) {
    Browser::KeyInput in;
    switch (k.kind) {
    case BaseWindow::Key::Char:
      in.kind = Browser::KeyInput::Char;
      in.ch = k.ch;
      break;
    case BaseWindow::Key::Backspace:
      in.kind = Browser::KeyInput::Backspace;
      break;
    case BaseWindow::Key::Enter:
      in.kind = Browser::KeyInput::Enter;
      break;
    }
    return browser.handleKey(in);
  });

  window.SetMouseCallback([&browser](const BaseWindow::MouseEvent &m) {
    if (m.kind == BaseWindow::MouseEvent::ButtonDown) {
      return browser.handleClick(m.x, m.y);
    }
    return false;
  });

  window.Run();

  Net::Cleanup();
  return 0;
}
