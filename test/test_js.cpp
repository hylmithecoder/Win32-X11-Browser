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
  Check("el.innerText sets element text in C++",
        elementId == "heading" && elementIdText == "Updated text!");

  // 7. Test global function declaration and call
  engine.execute("function add(a, b) { return a + b; }");

  // 8. Test function call with arguments and check return value
  engine.execute("var sum = add(7, 3);");
  Js::JsValue sum = engine.getGlobalEnv()->get("sum");
  Check("function call with arguments (add(7,3)), expected 10",
        sum.numberVal == 10.0);

  // 9. Test nested function calls. compose(f, g, x) = f(g(x)); both f and g
  // are unary, so compose(double, square, 3) = double(9) = 18.
  engine.execute("function compose(f, g, x) { return f(g(x)); }");
  engine.execute("var result = compose(function(n){return n+n;}, "
                 "function(y){return y*y;}, 3);");
  Js::JsValue result = engine.getGlobalEnv()->get("result");
  Check("nested function calls (compose(double, square, 3)), expected 18.0, got " +
            std::to_string(result.numberVal),
        result.numberVal == 18.0);

  // 10. Test if-else statement
  engine.execute("var isEven = function(n){ if (n % 2 === 0) return true; else "
                 "return false; };");
  engine.execute("var evenTest = isEven(4);");
  Js::JsValue evenTest = engine.getGlobalEnv()->get("evenTest");
  Check("if-else statement (isEven(4)), expected true",
        evenTest.boolVal == true);

  // 11. Test while loop
  engine.execute("var i = 0; while (i < 5) { i++; }");
  Js::JsValue i = engine.getGlobalEnv()->get("i");
  Check("while loop (i < 5, increment i), expected 5", i.numberVal == 5.0);

  // 12. Test for loop
  engine.execute(
      "var sumFor = 0; for (var j = 1; j <= 5; j++) { sumFor += j; }");
  Js::JsValue sumFor = engine.getGlobalEnv()->get("sumFor");
  Check("for loop (1 to 5 inclusive), expected 15", sumFor.numberVal == 15.0);

  // 13. Test array declaration
  engine.execute("var arr = [10, 20, 30, 40, 50];");

  // 14. Test array element access
  engine.execute("var secondElement = arr[1];");
  Js::JsValue secondElement = engine.getGlobalEnv()->get("secondElement");
  Check("array element access (arr[1]), expected 20",
        secondElement.numberVal == 20.0);

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
