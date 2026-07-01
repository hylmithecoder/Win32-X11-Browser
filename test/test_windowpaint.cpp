#include "BaseWindow.hpp"
#include "Css.hpp"
#include "Layout.hpp"
#include "Paint.hpp"
#include "Wrapper.hpp"

#include <string>

using namespace DesktopWebview;

int main() {
  // A small demo page exercising the full pipeline: HTML -> DOM -> CSS cascade
  // -> box-model layout -> paint -> on-screen presentation.
  const std::string html = "<html><body>"
                           "<div id='header'></div>"
                           "<div id='content'>"
                           "<div id='sidebar'></div>"
                           "<div id='main'></div>"
                           "</div>"
                           "<div id='footer'></div>"
                           "</body></html>";

  const std::string css =
      "body { background-color: #f0f0f0; }"
      "#header { height: 80px; background-color: #3366cc; }"
      "#content { background-color: #ffffff; padding: 20px; }"
      "#sidebar { height: 200px; width: 220px; background-color: #dde8ff;"
      "           border: 3px solid #3366cc; margin: 0 0 20px 0; }"
      "#main { height: 160px; background-color: #fbe8d0;"
      "        border: 1px solid #cccccc; }"
      "#footer { height: 60px; background-color: #333333; }";

  Wrapper::HtmlDocument doc;
  doc.parse(html);
  Css::Stylesheet sheet = Css::parse(css);
  Layout::StyledNode style = Layout::styleTree(doc.root(), sheet);

  BaseWindow window;
  window.SetRenderCallback([&style](int width, int height) {
    Layout::LayoutBox box = Layout::layout(style, static_cast<float>(width),
                                           static_cast<float>(height));
    Paint::DisplayList list = Paint::buildDisplayList(box);

    Paint::Canvas canvas(width, height);
    canvas.clear(Paint::Color{255, 255, 255, 255});
    canvas.paint(list);
    return canvas;
  });

  window.Run();
  return 0;
}
