#include "JsEngine.hpp"
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
  std::cout << "Async: setTimeout / setInterval" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // 15. setTimeout does not fire until the engine is pumped past its delay.
  engine.execute("var fired = false; setTimeout(function(){ fired = true; }, "
                 "100);");
  Check("setTimeout does not fire immediately",
        !engine.getGlobalEnv()->get("fired").boolVal);
  engine.pump(50);
  Check("setTimeout does not fire before its delay elapses",
        !engine.getGlobalEnv()->get("fired").boolVal);
  engine.pump(150);
  Check("setTimeout fires once its delay has elapsed",
        engine.getGlobalEnv()->get("fired").boolVal);

  // 16. setInterval fires repeatedly; clearInterval stops it. The engine's
  // virtual clock is already at 150ms from test 15, so pump far enough past
  // that for several 10ms ticks to have elapsed.
  engine.execute("var ticks = 0; var iv = setInterval(function(){ ticks++; "
                 "}, 10);");
  engine.pump(500);
  Js::JsValue ticks1 = engine.getGlobalEnv()->get("ticks");
  Check("setInterval fires multiple times (>= 3 well within 350ms/10ms)",
        ticks1.numberVal >= 3.0);
  engine.execute("clearInterval(iv);");
  Js::JsValue ticks2 = engine.getGlobalEnv()->get("ticks");
  engine.pump(2000);
  Check("clearInterval stops further ticks",
        engine.getGlobalEnv()->get("ticks").numberVal == ticks2.numberVal);

  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Async: Promise" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // 17. Promise executor + .then() chain (resolved synchronously; .then()'s
  // reaction runs as a microtask drained at the end of execute()).
  engine.execute(
      "var pResult = null;"
      "new Promise(function(resolve, reject){ resolve(21); })"
      "  .then(function(v){ return v * 2; })"
      "  .then(function(v){ pResult = v; });");
  Check("Promise chain resolves and both .then() reactions run (21*2=42)",
        engine.getGlobalEnv()->get("pResult").numberVal == 42.0);

  // 18. Promise rejection routed to .catch().
  engine.execute("var caught = null;"
                 "new Promise(function(resolve, reject){ reject('nope'); })"
                 "  .then(function(v){ return v; })"
                 "  .catch(function(e){ caught = e; });");
  Check("rejection skips .then() and is handled by .catch()",
        engine.getGlobalEnv()->get("caught").stringVal == "nope");

  // 19. Promise.resolve / Promise.all.
  engine.execute(
      "var allResult = null;"
      "Promise.all([Promise.resolve(1), 2, Promise.resolve(3)])"
      "  .then(function(vs){ allResult = vs[0] + vs[1] + vs[2]; });");
  Check("Promise.all resolves with all values (1+2+3=6)",
        engine.getGlobalEnv()->get("allResult").numberVal == 6.0);

  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Async: async function / await" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // 20. `await` on an already-resolved promise, inside an async function.
  engine.execute("async function getVal(){ var v = await "
                 "Promise.resolve(5); return v + 1; }"
                 "var awaitResult = null;"
                 "getVal().then(function(v){ awaitResult = v; });");
  Check("await unwraps a resolved promise (5+1=6)",
        engine.getGlobalEnv()->get("awaitResult").numberVal == 6.0);

  // 21. `await` on a promise that only resolves once a timer fires: doAwait
  // must itself pump the engine forward rather than returning too early.
  engine.execute(
      "function delay(ms, v){ return new Promise(function(resolve){ "
      "setTimeout(function(){ resolve(v); }, ms); }); }"
      "async function run(){ var a = await delay(30, 10); var b = await "
      "delay(30, 20); return a + b; }"
      "var delayedResult = null;"
      "run().then(function(v){ delayedResult = v; });");
  Check("await on a setTimeout-backed promise resolves via internal pumping "
        "(10+20=30)",
        engine.getGlobalEnv()->get("delayedResult").numberVal == 30.0);

  // 22. An awaited rejection aborts the async function and rejects its
  // promise; the caller observes it via .catch(), not a thrown C++ exception.
  engine.execute(
      "async function willReject(){ await Promise.reject('boom'); return "
      "'unreached'; }"
      "var rejectResult = null;"
      "willReject().catch(function(e){ rejectResult = e; });");
  Check("await on a rejected promise rejects the async function's promise",
        engine.getGlobalEnv()->get("rejectResult").stringVal == "boom");

  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "JSON.parse / JSON.stringify" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // 23. JSON.parse of an object with nested array/number/bool/null.
  engine.execute(
      "var obj = JSON.parse('{\"nama\":\"Hylmi\",\"semester\":5,"
      "\"aktif\":true,\"nilai\":[90,80],\"catatan\":null}');");
  Js::JsValue obj = engine.getGlobalEnv()->get("obj");
  Check("JSON.parse: string field", obj.getProperty("nama").stringVal == "Hylmi");
  Check("JSON.parse: number field", obj.getProperty("semester").numberVal == 5.0);
  Check("JSON.parse: boolean field", obj.getProperty("aktif").boolVal == true);
  Check("JSON.parse: array field length",
        obj.getProperty("nilai").objVal &&
            obj.getProperty("nilai").objVal->elements.size() == 2);
  Check("JSON.parse: array element value",
        obj.getProperty("nilai").objVal->elements[1].numberVal == 80.0);

  // 24. JSON.stringify round-trips a plain object.
  engine.execute("var s = JSON.stringify({a: 1, b: 'x', c: [1,2]});");
  Js::JsValue s = engine.getGlobalEnv()->get("s");
  Check("JSON.stringify produces parseable JSON",
        s.type == Js::ValueType::String && !s.stringVal.empty());
  engine.execute("var roundTrip = JSON.parse(s);");
  Js::JsValue roundTrip = engine.getGlobalEnv()->get("roundTrip");
  Check("JSON.stringify -> JSON.parse round-trip preserves values",
        roundTrip.getProperty("a").numberVal == 1.0 &&
            roundTrip.getProperty("b").stringVal == "x" &&
            roundTrip.getProperty("c").objVal->elements.size() == 2);

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
