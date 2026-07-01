#include "Input.hpp"
#include "Wrapper.hpp"

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

static Wrapper::Node El(Wrapper::HtmlDocument &doc, const std::string &id) {
  return doc.getElementById(id);
}

int main() {
  const std::string html =
      "<html><body><form>"
      "<input type=\"text\" id=\"txt\" value=\"hi\">"
      "<input type=\"password\" id=\"pw\" value=\"abc\">"
      "<input type=\"checkbox\" id=\"cb\">"
      "<input type=\"radio\" name=\"g\" id=\"r1\">"
      "<input type=\"radio\" name=\"g\" id=\"r2\" checked>"
      "<input type=\"hidden\" id=\"hid\">"
      "<input type=\"submit\" id=\"sub\" value=\"Go\">"
      "<button id=\"btn\">Click Me</button>"
      "<textarea id=\"ta\">body text</textarea>"
      "<select id=\"sel\"><option value=\"1\">One</option>"
      "<option value=\"2\" selected>Two</option></select>"
      "<div id=\"div\">x</div>"
      "</form></body></html>";
  Wrapper::HtmlDocument doc;
  doc.parse(html);

  using K = Elements::ControlKind;

  std::cout << "=== classify ===" << std::endl;
  Check("text classified", Elements::classify(El(doc, "txt")) == K::Text);
  Check("password classified", Elements::classify(El(doc, "pw")) == K::Password);
  Check("checkbox classified", Elements::classify(El(doc, "cb")) == K::Checkbox);
  Check("radio classified", Elements::classify(El(doc, "r1")) == K::Radio);
  Check("hidden classified", Elements::classify(El(doc, "hid")) == K::Hidden);
  Check("submit is button", Elements::classify(El(doc, "sub")) == K::Button);
  Check("button classified", Elements::classify(El(doc, "btn")) == K::Button);
  Check("textarea classified", Elements::classify(El(doc, "ta")) == K::Textarea);
  Check("select classified", Elements::classify(El(doc, "sel")) == K::Select);
  Check("div is not a control",
        Elements::classify(El(doc, "div")) == K::NoControl);
  Check("isFormControl(select)", Elements::isFormControl(El(doc, "sel")));
  Check("!isFormControl(div)", !Elements::isFormControl(El(doc, "div")));
  Check("isTextEntry(text)", Elements::isTextEntry(El(doc, "txt")));
  Check("isTextEntry(textarea)", Elements::isTextEntry(El(doc, "ta")));
  Check("!isTextEntry(checkbox)", !Elements::isTextEntry(El(doc, "cb")));

  std::cout << "=== displayText ===" << std::endl;
  Check("text value shown", Elements::displayText(El(doc, "txt")).text == "hi");
  Check("password masked", Elements::displayText(El(doc, "pw")).text == "***");
  Check("button label from text",
        Elements::displayText(El(doc, "btn")).text == "Click Me");
  Check("submit label from value",
        Elements::displayText(El(doc, "sub")).text == "Go");
  Check("textarea shows content",
        Elements::displayText(El(doc, "ta")).text == "body text");
  Check("select shows selected option",
        Elements::displayText(El(doc, "sel")).text == "Two");
  {
    Wrapper::Node txt = El(doc, "txt");
    txt.removeAttribute("value");
    Check("empty text -> placeholder flag",
          Elements::displayText(txt).placeholder);
    txt.setAttribute("value", "hi"); // restore
  }

  std::cout << "=== intrinsicSize ===" << std::endl;
  Elements::Size cb = Elements::intrinsicSize(El(doc, "cb"), 16);
  Check("checkbox square, >0", cb.width > 0 && cb.width == cb.height);
  Check("hidden -> zero size",
        Elements::intrinsicSize(El(doc, "hid"), 16).width == 0);
  Check("button width grows with label length",
        Elements::intrinsicSize(El(doc, "btn"), 16).width >
            Elements::intrinsicSize(El(doc, "sub"), 16).width);

  std::cout << "=== interaction ===" << std::endl;
  {
    Wrapper::Node cbx = El(doc, "cb");
    Check("checkbox starts unchecked", !cbx.hasAttribute("checked"));
    bool now = Elements::toggleCheckbox(cbx);
    Check("toggle -> checked", now && cbx.hasAttribute("checked"));
    Elements::toggleCheckbox(cbx);
    Check("toggle -> unchecked", !cbx.hasAttribute("checked"));
  }
  {
    Wrapper::Node r1 = El(doc, "r1");
    Wrapper::Node r2 = El(doc, "r2");
    Elements::selectRadio(r1, doc);
    Check("selectRadio checks r1", r1.hasAttribute("checked"));
    Check("selectRadio clears same-group r2", !r2.hasAttribute("checked"));
  }
  {
    Wrapper::Node sel = El(doc, "sel");
    Elements::cycleSelect(sel); // "Two" -> wraps to "One"
    Check("cycleSelect advances selection",
          Elements::displayText(sel).text == "One");
  }
  {
    Wrapper::Node txt = El(doc, "txt");
    std::string before = txt.attribute("value");
    Elements::insertChar(txt, 'Z');
    Check("insertChar appends", txt.attribute("value") == before + "Z");
    Check("backspace removes", Elements::backspace(txt) &&
                                   txt.attribute("value") == before);
  }

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All Input (form-control) tests passed." << std::endl;
  } else {
    std::cout << g_failures << " Input test(s) failed." << std::endl;
  }
  return g_failures == 0 ? 0 : 1;
}
