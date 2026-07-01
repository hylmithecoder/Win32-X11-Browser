#include "BaseWindow.hpp"
#include "Browser.hpp"
#include "Debugger.hpp"
#include "Net.hpp"
#include "Optimizer.hpp"

#include <string>

#define TITLENAME "Hylmi"

using namespace Debug;
using namespace DesktopWebview;

int main(int argc, char **argv) {
  Net::Init();

  // Warm up and time the GPU/OpenCL paths up front. Prints per-pass timings so
  // the optimization cost (and the GPU/CPU choice) is visible at startup.
  Optimizer::optimizeAll();

  // Start URL: first CLI argument, else a local server. Type a new URL into the
  // address bar and press Enter to navigate.
  std::string startUrl = (argc > 1) ? argv[1] : "http://localhost/";

  Browser::Browser browser;

  BaseWindow window;

  // Wire up paste request (async clipboard read via X11)
  browser.m_requestPaste = [&window]() {
    window.RequestPaste();
  };
  browser.m_copyText = [&window](const std::string &text) {
    window.SetSelectionText(text);
  };

  browser.navigate(startUrl);

  window.SetRenderCallback([&browser](int width, int height) {
    return browser.render(width, height);
  });

  window.SetKeyCallback([&browser](const BaseWindow::Key &k) {
    Browser::KeyInput in;
    in.ctrl = k.ctrl;
    in.shift = k.shift;
    in.alt = k.alt;
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
    case BaseWindow::Key::Left:
      in.kind = Browser::KeyInput::Left;
      break;
    case BaseWindow::Key::Right:
      in.kind = Browser::KeyInput::Right;
      break;
    case BaseWindow::Key::Up:
      in.kind = Browser::KeyInput::Up;
      break;
    case BaseWindow::Key::Down:
      in.kind = Browser::KeyInput::Down;
      break;
    case BaseWindow::Key::Tab:
      in.kind = Browser::KeyInput::Tab;
      break;
    case BaseWindow::Key::Delete:
      in.kind = Browser::KeyInput::Delete;
      break;
    case BaseWindow::Key::Home:
      in.kind = Browser::KeyInput::Home;
      break;
    case BaseWindow::Key::End:
      in.kind = Browser::KeyInput::End;
      break;
    }
    // Ctrl+C / Ctrl+V are handled as Char events with ctrl flag
    return browser.handleKey(in);
  });

  window.SetPasteCallback([&browser](const std::string &text) {
    browser.handlePaste(text);
  });

  window.SetMouseCallback([&browser, &window](const BaseWindow::MouseEvent &m) {
    switch (m.kind) {
    case BaseWindow::MouseEvent::ButtonDown:
      return browser.handleMouseDown(m.x, m.y);
    case BaseWindow::MouseEvent::Move:
      return browser.handleMouseMove(m.x, m.y);
    case BaseWindow::MouseEvent::ButtonUp: {
      bool repaint = browser.handleMouseUp(m.x, m.y);
      // Publish a completed selection to the system clipboard.
      std::string sel = browser.selectedText();
      if (!sel.empty()) {
        window.SetSelectionText(sel);
      }
      return repaint;
    }
    case BaseWindow::MouseEvent::ScrollUp:
      return browser.handleScroll(-40);
    case BaseWindow::MouseEvent::ScrollDown:
      return browser.handleScroll(40);
    }
    return false;
  });

  window.Run();

  Net::Cleanup();
  return 0;
}
