#ifndef JS_ENGINE_HPP
#define JS_ENGINE_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Js {

enum class ValueType { Undefined, Number, String, Object, Boolean };

struct JsObject;

struct JsValue {
  ValueType type = ValueType::Undefined;
  double numberVal = 0.0;
  std::string stringVal;
  bool boolVal = false;
  std::shared_ptr<JsObject> objVal;
  std::function<JsValue(const std::vector<JsValue> &)> callback;

  // DOM flag bindings
  bool isDocument = false;
  bool isElement = false;
  std::string elementId;

  JsValue();
  explicit JsValue(ValueType t);
  explicit JsValue(double n);
  explicit JsValue(const std::string &s);
  explicit JsValue(const char *s);
  explicit JsValue(bool b);
  JsValue(std::function<JsValue(const std::vector<JsValue> &)> cb);

  std::string toString() const;
  JsValue getProperty(const std::string &name) const;
  void setProperty(const std::string &name, const JsValue &val);
};

struct JsObject {
  std::map<std::string, JsValue> properties;
  // Array storage: when isArray is true, `elements` holds the ordered values.
  std::vector<JsValue> elements;
  bool isArray = false;

  // Promise state (meaningful only when isPromise is true). `onSettled` holds
  // reaction closures registered by .then()/.catch()/.finally() while still
  // pending; each one already captures everything it needs to drive its own
  // derived promise, so no separate reaction-record type is needed here.
  bool isPromise = false;
  int promiseState = 0; // 0 = pending, 1 = fulfilled, 2 = rejected
  JsValue promiseResult;
  std::vector<std::function<void()>> onSettled;
};

struct JsEnvironment {
  std::map<std::string, JsValue> vars;
  std::shared_ptr<JsEnvironment> parent;

  explicit JsEnvironment(std::shared_ptr<JsEnvironment> p = nullptr);
  JsValue get(const std::string &name);
  void set(const std::string &name, const JsValue &val);
};

struct DomInterface {
  std::function<void(const std::string &)> setTitle;
  std::function<void(const std::string &, const std::string &)> setElementText;
  std::function<std::string(const std::string &)> getElementText;
  std::function<std::string(const std::string &, const std::string &)> getElementAttribute;
  std::function<void(const std::string &, const std::string &, const std::string &)> setElementAttribute;
  std::function<std::vector<std::string>(const std::string &)> querySelectorAll;
};

// Control-flow signals (propagated as exceptions through the tree walker).
struct ReturnSignal {
  JsValue value;
};
struct BreakSignal {};
struct ContinueSignal {};
// Thrown by `await` when the awaited promise rejects. Not a user-facing JS
// exception (this engine has no try/catch/throw statement) -- it exists
// purely to unwind an async function's body up to the point in makeFunction
// that turns it into a rejection of that function's own Promise, mirroring
// how an uncaught rejection aborts an async function in real JS. Deliberately
// not derived from std::exception so it is never swallowed by a generic
// `catch (const std::exception &)`.
struct RejectSignal {
  JsValue value;
};

class JsEngine {
public:
  explicit JsEngine(DomInterface dom);
  void execute(const std::string &code);

  std::shared_ptr<JsEnvironment> getGlobalEnv() const { return m_globalEnv; }

  JsValue getOrElement(const std::string &id);
  void triggerEvent(const std::string &eventType, const std::string &elementId);

  // ---- Async support --------------------------------------------------
  // Advance the engine's virtual clock to `nowMs` (milliseconds since the
  // page started loading), running every timer/interval whose due time has
  // passed -- each followed by a full microtask drain, matching the JS
  // event loop's "one macrotask, then drain microtasks" ordering -- plus any
  // microtasks already queued. The Browser calls this once per render tick
  // so setTimeout/setInterval/Promise callbacks actually fire over time
  // instead of being permanently deferred.
  void pump(double nowMs);

  // Promise primitives. Public (rather than tucked behind a friend
  // declaration) because the interpreter's Parser is a free-standing class
  // defined entirely in JsEngine.cpp, not a nested member, and needs to
  // drive Promise/async-function/await behaviour through these.
  JsValue makePromise();
  void resolvePromiseObj(std::shared_ptr<JsObject> obj, JsValue value);
  void rejectPromiseObj(std::shared_ptr<JsObject> obj, JsValue reason);
  JsValue promiseThen(std::shared_ptr<JsObject> obj, JsValue onFulfilled,
                      JsValue onRejected);
  void enqueueMicrotask(std::function<void()> task);

  // Run exactly one pending unit of async work: a queued microtask if any,
  // else the earliest due timer (jumping the virtual clock forward to meet
  // it). This lets `await` fast-forward straight to whatever it's waiting on
  // without a real wall-clock wait. Returns false when nothing remains that
  // could possibly settle anything further, so `await` can give up rather
  // than loop forever on a promise nothing will ever resolve.
  bool advanceOneStep();

private:
  void initBuiltins();
  void drainMicrotasks();

  // One scheduled setTimeout/setInterval callback.
  struct Timer {
    int id = 0;
    double dueMs = 0;
    double intervalMs = 0; // > 0 for setInterval; ignored for one-shot timers
    bool repeating = false;
    bool cancelled = false;
    JsValue callback;
    std::vector<JsValue> args;
  };

  DomInterface m_dom;
  std::shared_ptr<JsEnvironment> m_globalEnv;
  std::map<std::string, JsValue> m_elementCache;

  std::vector<Timer> m_timers;
  int m_nextTimerId = 1;
  double m_virtualNowMs = 0.0;
  std::vector<std::function<void()>> m_microtasks;
};

} // namespace Js
} // namespace DesktopWebview

#endif // JS_ENGINE_HPP
