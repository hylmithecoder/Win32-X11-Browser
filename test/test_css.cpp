#include "../include/Css.hpp"
#include "../include/Wrapper.hpp"
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

// Convenience: computed value for a property, or "<none>" when unset.
static std::string Val(const std::map<std::string, std::string> &style,
                       const std::string &prop) {
  auto it = style.find(prop);
  return it == style.end() ? "<none>" : it->second;
}

static void ParsingTests() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "CSS parsing" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  const std::string css = "/* a comment */\n"
                          "h1, .title { color: red; font-size: 20px; }\n"
                          "div p { margin: 0 !important; }\n"
                          "@media screen { .ignored { color: blue; } }\n"
                          "#main { color: green }\n";

  Css::Stylesheet sheet = Css::parse(css);

  // 3 rules: the grouped h1/.title, div p, and #main. The @media block is
  // skipped entirely.
  Check("parsed 3 rules (at-rule skipped)", sheet.rules.size() == 3);

  if (sheet.rules.size() >= 1) {
    const Css::Rule &r0 = sheet.rules[0];
    Check("rule 0 has 2 grouped selectors", r0.selectors.size() == 2);
    Check("rule 0 has 2 declarations", r0.declarations.size() == 2);
    Check("rule 0 decl 0 is color: red",
          r0.declarations[0].property == "color" &&
              r0.declarations[0].value == "red");
  }

  if (sheet.rules.size() >= 2) {
    const Css::Rule &r1 = sheet.rules[1];
    Check("'div p' parsed as 2 components",
          r1.selectors.size() == 1 && r1.selectors[0].components.size() == 2);
    Check("margin marked !important", r1.declarations.size() == 1 &&
                                          r1.declarations[0].important &&
                                          r1.declarations[0].value == "0");
  }
}

static void SpecificityTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Specificity" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  auto spec = [](const std::string &sel) {
    return Css::parse(sel + " { x: y }").rules[0].selectors[0].specificity();
  };

  Check("#id (1,0,0) > .class (0,1,0)", spec(".c") < spec("#i"));
  Check(".class (0,1,0) > type (0,0,1)", spec("div") < spec(".c"));
  Check("div.c.d#i counts correctly",
        (spec("div.c.d#i") == Css::Specificity{1, 2, 1}));
  Check("universal contributes nothing",
        (spec("*") == Css::Specificity{0, 0, 0}));
}

static void MatchingTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Selector matching against the DOM" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  const std::string html = "<html><body>"
                           "<div class=\"box wide\" id=\"main\">"
                           "  <p class=\"lead\">hello</p>"
                           "  <span>world</span>"
                           "</div>"
                           "<p>outside</p>"
                           "</body></html>";

  Wrapper::HtmlDocument doc;
  doc.parse(html);

  std::vector<Wrapper::Node> ps = doc.getElementsByTagName("p");
  // ps[0] = .lead inside the div, ps[1] = the outside <p>.
  Check("found 2 <p> elements", ps.size() == 2);
  if (ps.size() != 2) {
    return;
  }
  Wrapper::Node lead = ps[0];
  Wrapper::Node outside = ps[1];
  Wrapper::Node div = doc.getElementById("main");

  auto sel = [](const std::string &s) {
    return Css::parse(s + " { x: y }").rules[0].selectors[0];
  };

  Check("type 'p' matches lead", Css::matches(sel("p"), lead));
  Check("class '.lead' matches lead", Css::matches(sel(".lead"), lead));
  Check("'.lead' does NOT match outside <p>",
        !Css::matches(sel(".lead"), outside));
  Check("id '#main' matches div", Css::matches(sel("#main"), div));
  Check("compound 'div.box#main' matches div",
        Css::matches(sel("div.box#main"), div));
  Check("compound 'div.box#main' does NOT match lead",
        !Css::matches(sel("div.box#main"), lead));
  Check("descendant 'div p' matches lead", Css::matches(sel("div p"), lead));
  Check("descendant 'div p' does NOT match outside <p>",
        !Css::matches(sel("div p"), outside));
  Check("descendant '#main .lead' matches lead",
        Css::matches(sel("#main .lead"), lead));
  Check("universal '*' matches div", Css::matches(sel("*"), div));

  // Grouping: "span, p" parses to two selectors; the outside <p> matches the
  // second but not the first.
  Css::Rule grouped = Css::parse("span, p { x: y }").rules[0];
  Check("'span, p' parses to 2 selectors", grouped.selectors.size() == 2);
  if (grouped.selectors.size() == 2) {
    Check("outside <p> matches 'p' but not 'span' in the group",
          !Css::matches(grouped.selectors[0], outside) &&
              Css::matches(grouped.selectors[1], outside));
  }
}

static void CascadeTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Cascade (computeStyle)" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  const std::string html = "<html><body>"
                           "<p class=\"lead\" id=\"intro\" style=\"color: "
                           "purple; margin: 5px\">hi</p>"
                           "</body></html>";

  Wrapper::HtmlDocument doc;
  doc.parse(html);
  Wrapper::Node p = doc.getElementById("intro");
  Check("test node found", p.valid());

  // color: source order then specificity then !important then inline.
  const std::string css =
      "p { color: red; font-size: 10px; }" // (0,0,1)
      ".lead { color: green; }"            // (0,1,0) > p
      "#intro { font-size: 16px; }"        // (1,0,0)
      "p { color: orange; }"               // (0,0,1) but later
      "p { font-weight: bold !important; }"
      ".lead { font-weight: normal; }"; // loses to !important

  Css::Stylesheet sheet = Css::parse(css);
  std::map<std::string, std::string> style = Css::computeStyle(sheet, p);

  Check("color resolves to inline 'purple' (inline > author)",
        Val(style, "color") == "purple");
  Check("font-size resolves to '16px' (#intro beats p)",
        Val(style, "font-size") == "16px");
  Check("font-weight resolves to 'bold' (!important beats normal)",
        Val(style, "font-weight") == "bold");
  Check("inline 'margin' applied: 5px", Val(style, "margin") == "5px");

  // Without the inline style, .lead's green should win for color.
  const std::string html2 =
      "<html><body><p class=\"lead\" id=\"intro\">hi</p></body></html>";
  Wrapper::HtmlDocument doc2;
  doc2.parse(html2);
  Wrapper::Node p2 = doc2.getElementById("intro");
  std::map<std::string, std::string> style2 = Css::computeStyle(sheet, p2);
  Check("no inline: color == green (.lead 0,1,0 beats p 0,0,1)",
        Val(style2, "color") == "green");
}

int main() {
  ParsingTests();
  SpecificityTests();
  MatchingTests();
  CascadeTests();

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All CSS tests passed." << std::endl;
  } else {
    std::cout << g_failures << " CSS test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
