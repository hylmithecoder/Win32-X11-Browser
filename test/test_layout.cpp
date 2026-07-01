#include "Css.hpp"
#include "Layout.hpp"
#include "Wrapper.hpp"
#include <cmath>
#include <iostream>
#include <string>

using namespace DesktopWebview;

static int g_failures = 0;

static void Check(const std::string &label, bool condition) {
  if (condition) {
    std::cout << "  [PASS] " << label << std::endl;
  } else {
    std::cout << "  [FAIL] " << label << std::endl;
    ++g_failures;
  }
}

// Float comparison tolerant of rounding.
static bool Near(float a, float b) { return std::fabs(a - b) < 0.5f; }

static void AdvancedLayoutTests();

static void CheckNear(const std::string &label, float actual, float expected) {
  bool ok = Near(actual, expected);
  if (!ok) {
    std::cout << "  [FAIL] " << label << " (got " << actual << ", expected "
              << expected << ")" << std::endl;
    ++g_failures;
  } else {
    std::cout << "  [PASS] " << label << std::endl;
  }
}

// Depth-first search for the box whose element carries id="<id>".
static const Layout::LayoutBox *FindById(const Layout::LayoutBox &box,
                                         const std::string &id) {
  if (box.node && box.node->node.isElement() &&
      box.node->node.attribute("id") == id) {
    return &box;
  }
  for (const Layout::LayoutBox &child : box.children) {
    if (const Layout::LayoutBox *found = FindById(child, id)) {
      return found;
    }
  }
  return nullptr;
}

int main() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "Layout engine (box model, block flow)" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  const std::string html = "<html><head><title>t</title></head><body>"
                           "<div id=\"a\"></div>"
                           "<div id=\"b\"></div>"
                           "<div id=\"c\"></div>"
                           "<div id=\"d\"></div>"
                           "<div id=\"hidden\"></div>"
                           "</body></html>";

  const std::string css = "div { height: 50px; }"
                          "#c { width: 400px; margin: 0 auto; }"
                          "#d { width: 100px; padding: 10px; border: 5px solid "
                          "black; height: 30px; }"
                          "#hidden { display: none; }";

  Wrapper::HtmlDocument doc;
  doc.parse(html);
  Css::Stylesheet sheet = Css::parse(css);

  Layout::StyledNode style = Layout::styleTree(doc.root(), sheet);
  Layout::LayoutBox root = Layout::layout(style, 800.0f);

  std::cout << "\n--- LAYOUT TREE ---" << std::endl;
  Layout::printLayoutTree(root);
  std::cout << std::endl;

  // Root <html> fills the viewport width.
  CheckNear("root width fills viewport (800)", root.dimensions.content.width,
            800.0f);
  Check("root is <html>", root.node && root.node->node.name() == "html");
  Check("display:none head excluded -> html has 1 child box (body)",
        root.children.size() == 1);

  const Layout::LayoutBox *body = FindById(root, "");
  // body has no id; fetch via the single child instead.
  body = root.children.empty() ? nullptr : &root.children.front();
  Check("body present",
        body != nullptr && body->node && body->node->node.name() == "body");
  if (body) {
    CheckNear("body width fills viewport (800)", body->dimensions.content.width,
              800.0f);
  }

  const Layout::LayoutBox *a = FindById(root, "a");
  const Layout::LayoutBox *b = FindById(root, "b");
  const Layout::LayoutBox *c = FindById(root, "c");
  const Layout::LayoutBox *d = FindById(root, "d");
  const Layout::LayoutBox *hidden = FindById(root, "hidden");

  Check("blocks a-d all present", a && b && c && d);
  Check("display:none box is absent from the tree", hidden == nullptr);

  if (a && b) {
    CheckNear("a stacks at y=0", a->dimensions.content.y, 0.0f);
    CheckNear("a height from 'div{height:50px}'", a->dimensions.content.height,
              50.0f);
    CheckNear("a width fills container (800)", a->dimensions.content.width,
              800.0f);
    CheckNear("b stacks below a at y=50", b->dimensions.content.y, 50.0f);
  }

  if (c) {
    // width 400 centred in 800 -> 200px margins on each side.
    CheckNear("c centred: content.x=200", c->dimensions.content.x, 200.0f);
    CheckNear("c width honoured (400)", c->dimensions.content.width, 400.0f);
    CheckNear("c stacks below a+b at y=100", c->dimensions.content.y, 100.0f);
  }

  if (d) {
    // content.x offset by border(5) + padding(10) = 15.
    CheckNear("d content.x offset by border+padding (15)",
              d->dimensions.content.x, 15.0f);
    CheckNear("d width honoured (100)", d->dimensions.content.width, 100.0f);
    CheckNear("d explicit height overrides div rule (30)",
              d->dimensions.content.height, 30.0f);
    // border box width = 100 + 2*10 padding + 2*5 border = 130.
    CheckNear("d border-box width = 130", d->dimensions.borderBox().width,
              130.0f);
    // y = a(50) + b(50) + c(50) = 150, plus border(5) + padding(10) = 165.
    CheckNear("d content.y accounts for siblings + border + padding (165)",
              d->dimensions.content.y, 165.0f);
  }

  if (body) {
    // a+b+c each margin-box 50 = 150, d margin-box = 30+20+10 = 60 -> 210.
    CheckNear("body height encloses children (210)",
              body->dimensions.content.height, 210.0f);
  }

  AdvancedLayoutTests();

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All layout tests passed." << std::endl;
  } else {
    std::cout << g_failures << " layout test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}

static void AdvancedLayoutTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Advanced Layout (max-width, flex wrapping, grid areas/spans)"
            << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // 1. Test max-width
  {
    const std::string html = "<html><body><div id=\"mw\"></div></body></html>";
    const std::string css = "#mw { width: 500px; max-width: 300px; }";
    Wrapper::HtmlDocument doc;
    doc.parse(html);
    Css::Stylesheet sheet = Css::parse(css);
    Layout::StyledNode style = Layout::styleTree(doc.root(), sheet);
    Layout::LayoutBox root = Layout::layout(style, 800.0f);
    const Layout::LayoutBox *mw = FindById(root, "mw");
    Check("max-width overrides width (300)",
          mw && Near(mw->dimensions.content.width, 300.0f));
  }

  // 2. Test flex-wrap
  {
    const std::string html = "<html><body><div id=\"flex\">"
                             "  <div id=\"f1\"></div><div id=\"f2\"></div>"
                             "</div></body></html>";
    const std::string css =
        "#flex { display: flex; flex-wrap: wrap; width: 200px; }"
        "#f1 { width: 150px; height: 50px; }"
        "#f2 { width: 100px; height: 50px; }";
    Wrapper::HtmlDocument doc;
    doc.parse(html);
    Css::Stylesheet sheet = Css::parse(css);
    Layout::StyledNode style = Layout::styleTree(doc.root(), sheet);
    Layout::LayoutBox root = Layout::layout(style, 400.0f);
    const Layout::LayoutBox *f1 = FindById(root, "f1");
    const Layout::LayoutBox *f2 = FindById(root, "f2");
    Check("flex item 1 placed on line 0",
          f1 && Near(f1->dimensions.content.y, 0.0f));
    Check("flex item 2 wrapped to line 1",
          f2 && Near(f2->dimensions.content.y, 50.0f));
  }

  // 3. Test grid template areas & spans
  {
    const std::string html = "<html><body><div id=\"grid\">"
                             "  <div id=\"g1\"></div><div id=\"g2\"></div>"
                             "</div></body></html>";
    const std::string css = "#grid { display: grid; grid-template-columns: "
                            "100px 100px; grid-template-areas: 'a b'; }"
                            "#g1 { grid-area: b; }"
                            "#g2 { grid-column: span 2; }";
    Wrapper::HtmlDocument doc;
    doc.parse(html);
    Css::Stylesheet sheet = Css::parse(css);
    Layout::StyledNode style = Layout::styleTree(doc.root(), sheet);
    Layout::LayoutBox root = Layout::layout(style, 300.0f);
    const Layout::LayoutBox *g1 = FindById(root, "g1");
    const Layout::LayoutBox *g2 = FindById(root, "g2");
    Check("grid item g1 placed in area b (col 1)",
          g1 && Near(g1->dimensions.content.x, 100.0f));
    Check("grid item g2 spans 2 columns (width 200)",
          g2 && Near(g2->dimensions.content.width, 200.0f));
  }
}
