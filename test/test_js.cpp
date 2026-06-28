#include "../include/JsEngine.hpp"
#include <cassert>
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

int main() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "JavaScript Engine unit tests" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  std::string testTitle = "";
  std::string elementIdText = "";
  std::string elementId = "";

  Js::DomInterface dom;
  dom.setTitle = [&](const std::string &title) { testTitle = title; };
  dom.setElementText = [&](const std::string &id, const std::string &text) {
    elementId = id;
    elementIdText = text;
  };
  dom.getElementText = [&](const std::string &id) -> std::string {
    if (id == "heading") {
      return "Hello, World!";
    }
    return "";
  };

  Js::JsEngine engine(dom);

  // 1. Check basic arithmetic and variable variables
  engine.execute("var x = 10 + 5 * 2;");
  Js::JsValue x = engine.getGlobalEnv()->get("x");
  Check("var declaration and expression evaluation (precedence)",
        x.numberVal == 20.0);

  // 2. Check variable update
  engine.execute("x = x + 5;");
  x = engine.getGlobalEnv()->get("x");
  Check("variable update assignment", x.numberVal == 25.0);

  // 3. String concatenation
  engine.execute("var greeting = 'Hello ' + 'Hylmi';");
  Js::JsValue greeting = engine.getGlobalEnv()->get("greeting");
  Check("string concatenation works", greeting.stringVal == "Hello Hylmi");

  // 4. Equality check
  engine.execute("var eq1 = (10 == 10); var eq2 = (10 == 20);");
  Js::JsValue eq1 = engine.getGlobalEnv()->get("eq1");
  Js::JsValue eq2 = engine.getGlobalEnv()->get("eq2");
  Check("equality (true)", eq1.boolVal == true);
  Check("equality (false)", eq2.boolVal == false);

  // 5. Document title DOM integration
  engine.execute("document.title = 'Test Title From JS';");
  Check("document.title sets page title in C++",
        testTitle == "Test Title From JS");

  // 6. document.getElementById and element innerText setter integration
  engine.execute("var el = document.getElementById('heading');");
  Js::JsValue el = engine.getGlobalEnv()->get("el");
  Check("getElementById returns element object",
        el.isElement && el.elementId == "heading");

  engine.execute("el.innerText = 'Updated text!';");
  Check("el.innerText sets element text in DOM/C++",
        elementId == "heading" && elementIdText == "Updated text!");

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All JavaScript tests passed." << std::endl;
  } else {
    std::cout << g_failures << " JavaScript test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;

  return g_failures == 0 ? 0 : 1;
}
