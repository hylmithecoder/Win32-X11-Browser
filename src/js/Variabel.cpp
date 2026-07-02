#include "Variabel.hpp"
#include "Debugger.hpp"
#include "Net.hpp"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace Debug;

namespace DesktopWebview {
namespace Js {

bool truthy(const JsValue &v) {
  switch (v.type) {
  case ValueType::Boolean:
    return v.boolVal;
  case ValueType::Number:
    return v.numberVal != 0 && !std::isnan(v.numberVal);
  case ValueType::String:
    return !v.stringVal.empty();
  case ValueType::Object:
    return v.objVal != nullptr || static_cast<bool>(v.callback);
  default:
    return false;
  }
}

double toNum(const JsValue &v) {
  if (v.type == ValueType::Number)
    return v.numberVal;
  if (v.type == ValueType::Boolean)
    return v.boolVal ? 1.0 : 0.0;
  if (v.type == ValueType::String) {
    if (v.stringVal.empty())
      return 0.0;
    try {
      size_t pos = 0;
      double d = std::stod(v.stringVal, &pos);
      return d;
    } catch (...) {
      return std::nan("");
    }
  }
  return std::nan("");
}

namespace {

// Convert a parsed nlohmann::json value into the equivalent JsValue (objects
// -> Object with properties, arrays -> Object with isArray/elements, scalars
// map 1:1). Used by JSON.parse and fetch()'s Response.json().
JsValue JsonToJsValue(const nlohmann::json &j) {
  if (j.is_object()) {
    JsValue obj(ValueType::Object);
    for (auto it = j.begin(); it != j.end(); ++it) {
      obj.setProperty(it.key(), JsonToJsValue(it.value()));
    }
    return obj;
  }
  if (j.is_array()) {
    JsValue arr(ValueType::Object);
    arr.objVal->isArray = true;
    for (const auto &item : j) {
      arr.objVal->elements.push_back(JsonToJsValue(item));
    }
    return arr;
  }
  if (j.is_string()) {
    return JsValue(j.get<std::string>());
  }
  if (j.is_boolean()) {
    return JsValue(j.get<bool>());
  }
  if (j.is_number()) {
    return JsValue(j.get<double>());
  }
  return JsValue(); // null -> undefined (this engine has no separate null)
}

// Convert a JsValue into an nlohmann::json value (the inverse of
// JsonToJsValue). Used by JSON.stringify and fetch()'s request body
// serialization. A callable JsValue (no serializable representation in JSON)
// becomes `null`, matching real JSON.stringify's treatment of functions
// inside arrays.
nlohmann::json JsValueToJson(const JsValue &v) {
  switch (v.type) {
  case ValueType::Number:
    return v.numberVal;
  case ValueType::String:
    return v.stringVal;
  case ValueType::Boolean:
    return v.boolVal;
  case ValueType::Object: {
    if (v.callback) {
      return nullptr;
    }
    if (v.objVal && v.objVal->isArray) {
      nlohmann::json arr = nlohmann::json::array();
      for (const JsValue &item : v.objVal->elements) {
        arr.push_back(JsValueToJson(item));
      }
      return arr;
    }
    nlohmann::json obj = nlohmann::json::object();
    if (v.objVal) {
      for (const auto &kv : v.objVal->properties) {
        // Skip callable properties (methods) -- not serializable, and
        // present on e.g. a fetch() Response object alongside real data.
        if (!kv.second.callback) {
          obj[kv.first] = JsValueToJson(kv.second);
        }
      }
    }
    return obj;
  }
  default:
    return nullptr;
  }
}

std::string ToUpperAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return s;
}

// Parse the numeric status code out of a raw HTTP response's status line
// ("HTTP/1.1 200 OK\r\n..."), or 0 if it cannot be parsed.
int HttpStatusCode(const std::string &raw) {
  size_t sp1 = raw.find(' ');
  if (sp1 == std::string::npos) {
    return 0;
  }
  size_t sp2 = raw.find_first_of(" \r\n", sp1 + 1);
  if (sp2 == std::string::npos || sp2 <= sp1 + 1) {
    return 0;
  }
  try {
    return std::stoi(raw.substr(sp1 + 1, sp2 - sp1 - 1));
  } catch (...) {
    return 0;
  }
}

// The Promise<Response> fetch() resolves with. .status/.ok are plain values;
// .text()/.json() are themselves async (return a Promise) to match the real
// fetch() API, even though the body is already fully in memory by the time
// this is built -- so `await (await fetch(url)).json()` works the same way
// it would in a real browser.
JsValue makeFetchResponse(JsEngine *eng, int status, const std::string &body) {
  JsValue res(ValueType::Object);
  res.setProperty("status", JsValue(static_cast<double>(status)));
  res.setProperty("ok", JsValue(status >= 200 && status < 300));
  res.setProperty(
      "text", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                  [body, eng](const std::vector<JsValue> &) {
                    JsValue p = eng->makePromise();
                    eng->resolvePromiseObj(p.objVal, JsValue(body));
                    return p;
                  })));
  res.setProperty(
      "json", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                  [body, eng](const std::vector<JsValue> &) {
                    JsValue p = eng->makePromise();
                    try {
                      eng->resolvePromiseObj(
                          p.objVal, JsonToJsValue(nlohmann::json::parse(body)));
                    } catch (const nlohmann::json::parse_error &e) {
                      eng->rejectPromiseObj(
                          p.objVal, JsValue(std::string(
                                        "invalid JSON in response: ") +
                                        e.what()));
                    }
                    return p;
                  })));
  return res;
}

// Build a Date instance: a plain object carrying `epochMs` baked into each of
// its bound getter/formatter methods at construction time (this engine has no
// mutable-field object model for builtins, so the value is captured by the
// closures rather than stored on the object and re-read per call).
JsValue makeDateInstance(double epochMs) {
  JsValue d(ValueType::Object);

  auto bind0 = [&](const char *name, double value) {
    d.setProperty(name,
                  JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                      [value](const std::vector<JsValue> &) {
                        return JsValue(value);
                      })));
  };
  auto bindStr = [&](const char *name, const std::string &value) {
    d.setProperty(name,
                  JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                      [value](const std::vector<JsValue> &) {
                        return JsValue(value);
                      })));
  };

  bind0("getTime", epochMs);
  bind0("valueOf", epochMs);

  std::time_t secs = static_cast<std::time_t>(epochMs / 1000.0);
  int millis =
      static_cast<int>(epochMs - static_cast<double>(secs) * 1000.0);
  if (millis < 0) {
    millis += 1000; // negative epochMs (pre-1970): keep millis in [0,1000)
  }
  std::tm local = *std::localtime(&secs);
  std::tm utc = *std::gmtime(&secs);

  bind0("getFullYear", local.tm_year + 1900);
  bind0("getMonth", local.tm_mon); // 0-indexed, matches JS
  bind0("getDate", local.tm_mday);
  bind0("getDay", local.tm_wday); // 0 = Sunday, matches JS
  bind0("getHours", local.tm_hour);
  bind0("getMinutes", local.tm_min);
  bind0("getSeconds", local.tm_sec);
  bind0("getMilliseconds", millis);

  // "3:45:12 PM" -- the common en-US default toLocaleTimeString() format.
  int h12 = local.tm_hour % 12;
  if (h12 == 0) {
    h12 = 12;
  }
  std::ostringstream ts;
  ts << h12 << ":" << std::setfill('0') << std::setw(2) << local.tm_min
     << ":" << std::setfill('0') << std::setw(2) << local.tm_sec << " "
     << (local.tm_hour < 12 ? "AM" : "PM");
  bindStr("toLocaleTimeString", ts.str());

  // "M/D/YYYY" -- the common en-US default toLocaleDateString() format.
  std::ostringstream ds;
  ds << (local.tm_mon + 1) << "/" << local.tm_mday << "/"
     << (local.tm_year + 1900);
  bindStr("toLocaleDateString", ds.str());
  bindStr("toLocaleString", ds.str() + ", " + ts.str());

  // "YYYY-MM-DDTHH:MM:SS.mmmZ" (UTC) -- toISOString() is unambiguous, so it
  // uses UTC rather than local time, matching the real Date API.
  std::ostringstream iso;
  iso << std::setfill('0') << std::setw(4) << (utc.tm_year + 1900) << "-"
      << std::setw(2) << (utc.tm_mon + 1) << "-" << std::setw(2)
      << utc.tm_mday << "T" << std::setw(2) << utc.tm_hour << ":"
      << std::setw(2) << utc.tm_min << ":" << std::setw(2) << utc.tm_sec
      << "." << std::setw(3) << millis << "Z";
  bindStr("toISOString", iso.str());
  bindStr("toString", iso.str());

  return d;
}

} // namespace

void JsEngine::initBuiltins() {
  // console.log / warn / error / info
  JsValue consoleObj(ValueType::Object);
  auto logger = [](const std::vector<JsValue> &args) {
    for (size_t i = 0; i < args.size(); ++i) {
      std::cout << args[i].toString() << (i + 1 < args.size() ? " " : "");
    }
    std::cout << std::endl;
    return JsValue();
  };
  consoleObj.setProperty(
      "log",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(logger)));
  consoleObj.setProperty(
      "warn",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(logger)));
  consoleObj.setProperty(
      "error",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(logger)));
  consoleObj.setProperty(
      "info",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(logger)));
  m_globalEnv->set("console", consoleObj);

  // Math
  JsValue mathObj(ValueType::Object);
  mathObj.setProperty("PI", JsValue(3.141592653589793));
  mathObj.setProperty(
      "floor", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                   [](const std::vector<JsValue> &a) {
                     return JsValue(std::floor(a.empty() ? 0 : a[0].numberVal));
                   })));
  mathObj.setProperty(
      "ceil", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                  [](const std::vector<JsValue> &a) {
                    return JsValue(std::ceil(a.empty() ? 0 : a[0].numberVal));
                  })));
  mathObj.setProperty(
      "round", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                   [](const std::vector<JsValue> &a) {
                     return JsValue(std::round(a.empty() ? 0 : a[0].numberVal));
                   })));
  mathObj.setProperty(
      "abs", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                 [](const std::vector<JsValue> &a) {
                   return JsValue(std::fabs(a.empty() ? 0 : a[0].numberVal));
                 })));
  mathObj.setProperty(
      "max", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                 [](const std::vector<JsValue> &a) {
                   double m = -INFINITY;
                   for (auto &v : a)
                     m = std::max(m, v.numberVal);
                   return JsValue(m);
                 })));
  mathObj.setProperty(
      "min", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                 [](const std::vector<JsValue> &a) {
                   double m = INFINITY;
                   for (auto &v : a)
                     m = std::min(m, v.numberVal);
                   return JsValue(m);
                 })));
  mathObj.setProperty(
      "random", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                    [](const std::vector<JsValue> &) {
                      return JsValue((double)rand() / RAND_MAX);
                    })));
  mathObj.setProperty(
      "sqrt", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                  [](const std::vector<JsValue> &a) {
                    return JsValue(std::sqrt(a.empty() ? 0 : a[0].numberVal));
                  })));
  m_globalEnv->set("Math", mathObj);

  // parseInt / parseFloat / String / Number / Boolean / isNaN
  m_globalEnv->set("parseInt",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [](const std::vector<JsValue> &a) {
                         if (a.empty())
                           return JsValue(std::nan(""));
                         try {
                           return JsValue((double)std::stoll(a[0].toString()));
                         } catch (...) {
                           return JsValue(std::nan(""));
                         }
                       })));
  m_globalEnv->set("parseFloat",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [](const std::vector<JsValue> &a) {
                         if (a.empty())
                           return JsValue(std::nan(""));
                         try {
                           return JsValue(std::stod(a[0].toString()));
                         } catch (...) {
                           return JsValue(std::nan(""));
                         }
                       })));
  m_globalEnv->set("String",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [](const std::vector<JsValue> &a) {
                         return JsValue(a.empty() ? std::string()
                                                  : a[0].toString());
                       })));
  m_globalEnv->set("Number",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [](const std::vector<JsValue> &a) {
                         return JsValue(a.empty() ? 0.0 : toNum(a[0]));
                       })));
  m_globalEnv->set("Boolean",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [](const std::vector<JsValue> &a) {
                         return JsValue(!a.empty() && truthy(a[0]));
                       })));
  m_globalEnv->set("isNaN",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [](const std::vector<JsValue> &a) {
                         return JsValue(a.empty() || std::isnan(toNum(a[0])));
                       })));

  // Setup document
  JsValue documentObj(ValueType::Object);
  documentObj.isDocument = true;

  documentObj.setProperty(
      "getElementById",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [this](const std::vector<JsValue> &args) {
            if (args.empty())
              return JsValue();
            return getOrElement(args[0].toString());
          })));

  auto querySelectorAllFn =
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [this, dom = m_dom](const std::vector<JsValue> &args) {
            JsValue arr(ValueType::Object);
            arr.objVal->isArray = true;
            if (!args.empty() && dom.querySelectorAll) {
              std::string sel = args[0].toString();
              std::vector<std::string> ids = dom.querySelectorAll(sel);
              for (const std::string &id : ids) {
                arr.objVal->elements.push_back(getOrElement(id));
              }
            }
            arr.setProperty("length", JsValue(static_cast<double>(
                                          arr.objVal->elements.size())));
            return arr;
          }));
  documentObj.setProperty("querySelectorAll", querySelectorAllFn);

  auto querySelectorFn =
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [this, dom = m_dom](const std::vector<JsValue> &args) {
            if (!args.empty() && dom.querySelectorAll) {
              std::string sel = args[0].toString();
              std::vector<std::string> ids = dom.querySelectorAll(sel);
              if (!ids.empty()) {
                return getOrElement(ids[0]);
              }
            }
            return JsValue();
          }));
  documentObj.setProperty("querySelector", querySelectorFn);

  auto stubElement =
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [this](const std::vector<JsValue> &) { return JsValue(); }));
  documentObj.setProperty("createElement", stubElement);
  documentObj.setProperty(
      "addEventListener",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [docObj = documentObj.objVal](const std::vector<JsValue> &args) {
            if (args.size() >= 2) {
              std::string eventType = args[0].toString();
              JsValue callback = args[1];
              if (docObj) {
                docObj->properties["_listener_" + eventType] = callback;
              }
            }
            return JsValue();
          })));
  documentObj.setProperty(
      "write", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                   [](const std::vector<JsValue> &a) {
                     for (auto &v : a)
                       std::cout << v.toString();
                     return JsValue();
                   })));
  m_globalEnv->set("document", documentObj);

  // window aliases to the global scope; a couple of no-op timers.
  JsValue windowObj(ValueType::Object);
  windowObj.setProperty(
      "addEventListener",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [](const std::vector<JsValue> &) { return JsValue(); })));
  windowObj.setProperty(
      "alert", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                   [](const std::vector<JsValue> &a) {
                     MSGBOX_INFO("alert", (a.empty() ? "" : a[0].toString()));
                     return JsValue();
                   })));
  m_globalEnv->set("window", windowObj);
  m_globalEnv->set("alert", windowObj.getProperty("alert"));

  // setTimeout/setInterval schedule a real Timer, run later from pump() (the
  // Browser calls pump() once per render tick); any arguments after the
  // delay are forwarded to the callback, matching the DOM API.
  m_globalEnv->set("setTimeout",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [this](const std::vector<JsValue> &a) -> JsValue {
                         if (a.empty() || !a[0].callback) {
                           return JsValue();
                         }
                         Timer t;
                         t.id = m_nextTimerId++;
                         t.dueMs = m_virtualNowMs +
                                   (a.size() > 1 ? toNum(a[1]) : 0.0);
                         t.callback = a[0];
                         if (a.size() > 2) {
                           t.args.assign(a.begin() + 2, a.end());
                         }
                         m_timers.push_back(t);
                         return JsValue(static_cast<double>(t.id));
                       })));
  m_globalEnv->set("setInterval",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [this](const std::vector<JsValue> &a) -> JsValue {
                         if (a.empty() || !a[0].callback) {
                           return JsValue();
                         }
                         Timer t;
                         t.id = m_nextTimerId++;
                         t.intervalMs =
                             a.size() > 1 ? std::max(0.0, toNum(a[1])) : 0.0;
                         t.dueMs = m_virtualNowMs + t.intervalMs;
                         t.repeating = true;
                         t.callback = a[0];
                         if (a.size() > 2) {
                           t.args.assign(a.begin() + 2, a.end());
                         }
                         m_timers.push_back(t);
                         return JsValue(static_cast<double>(t.id));
                       })));
  m_globalEnv->set("clearTimeout",
                   JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                       [this](const std::vector<JsValue> &a) -> JsValue {
                         if (!a.empty()) {
                           int id = static_cast<int>(toNum(a[0]));
                           for (Timer &t : m_timers) {
                             if (t.id == id) {
                               t.cancelled = true;
                             }
                           }
                         }
                         return JsValue();
                       })));
  // Intervals share the same id space/vector as timeouts, so cancellation is
  // identical either way.
  m_globalEnv->set("clearInterval", m_globalEnv->get("clearTimeout"));

  // Date: `new Date()`/`new Date(ms)` return a bound-methods instance (see
  // makeDateInstance) using the real wall clock, not m_virtualNowMs -- that
  // field is purely internal timer-scheduling bookkeeping relative to page
  // load, not a value that should ever show up in a rendered date/time.
  auto nowEpochMs = []() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  JsValue dateGlobal(std::function<JsValue(const std::vector<JsValue> &)>(
      [nowEpochMs](const std::vector<JsValue> &a) -> JsValue {
        double ms = a.empty() ? nowEpochMs() : toNum(a[0]);
        return makeDateInstance(ms);
      }));
  dateGlobal.setProperty(
      "now", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                 [nowEpochMs](const std::vector<JsValue> &) {
                   return JsValue(nowEpochMs());
                 })));
  m_globalEnv->set("Date", dateGlobal);

  // ---- JSON -------------------------------------------------------------
  JsValue jsonObj(ValueType::Object);
  jsonObj.setProperty(
      "parse", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                   [](const std::vector<JsValue> &a) -> JsValue {
                     if (a.empty()) {
                       return JsValue();
                     }
                     try {
                       return JsonToJsValue(
                           nlohmann::json::parse(a[0].toString()));
                     } catch (const nlohmann::json::parse_error &) {
                       return JsValue(); // invalid JSON -> undefined (this
                                         // engine has no throw to raise
                                         // SyntaxError with instead)
                     }
                   })));
  jsonObj.setProperty(
      "stringify",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [](const std::vector<JsValue> &a) -> JsValue {
            if (a.empty()) {
              return JsValue();
            }
            return JsValue(JsValueToJson(a[0]).dump());
          })));
  m_globalEnv->set("JSON", jsonObj);

  // ---- fetch --------------------------------------------------------------
  // A synchronous (blocking) HTTP request via Net::Get/Post/Put/Delete --
  // this engine has no async I/O -- wrapped as a Promise<Response> so it
  // still composes with async/await and .then() chains the way real fetch()
  // does; since the underlying call has already completed by the time this
  // returns, the promise it returns is always already settled.
  m_globalEnv->set(
      "fetch",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [this](const std::vector<JsValue> &a) -> JsValue {
            JsValue promise = makePromise();
            if (a.empty()) {
              rejectPromiseObj(promise.objVal,
                               JsValue(std::string("fetch: missing URL")));
              return promise;
            }
            std::string url = a[0].toString();
            std::string method = "GET";
            std::string body;
            std::string contentType = Net::kDefaultContentType;
            if (a.size() > 1 && a[1].objVal) {
              JsValue methodVal = a[1].getProperty("method");
              if (methodVal.type == ValueType::String &&
                  !methodVal.stringVal.empty()) {
                method = ToUpperAscii(methodVal.stringVal);
              }
              JsValue bodyVal = a[1].getProperty("body");
              if (bodyVal.type == ValueType::String) {
                body = bodyVal.stringVal;
              }
              JsValue headersVal = a[1].getProperty("headers");
              if (headersVal.objVal) {
                JsValue ct = headersVal.getProperty("Content-Type");
                if (ct.type == ValueType::String && !ct.stringVal.empty()) {
                  contentType = ct.stringVal;
                }
              }
            }

            std::string raw;
            if (method == "POST") {
              raw = Net::Post(url, body, contentType);
            } else if (method == "PUT") {
              raw = Net::Put(url, body, contentType);
            } else if (method == "DELETE") {
              raw = Net::Delete(url);
            } else {
              raw = Net::Get(url);
            }

            if (raw.empty()) {
              rejectPromiseObj(
                  promise.objVal,
                  JsValue(std::string("fetch: network error requesting ") + url));
              return promise;
            }

            resolvePromiseObj(promise.objVal,
                              makeFetchResponse(this, HttpStatusCode(raw),
                                                Net::ExtractBody(raw)));
            return promise;
          })));

  // ---- Promise --------------------------------------------------------
  JsValue promiseGlobal(std::function<JsValue(const std::vector<JsValue> &)>(
      [this](const std::vector<JsValue> &a) -> JsValue {
        JsValue p = makePromise();
        if (!a.empty() && a[0].callback) {
          auto obj = p.objVal;
          JsEngine *eng = this;
          JsValue resolveFn(
              std::function<JsValue(const std::vector<JsValue> &)>(
                  [obj, eng](const std::vector<JsValue> &ra) {
                    eng->resolvePromiseObj(obj, ra.empty() ? JsValue() : ra[0]);
                    return JsValue();
                  }));
          JsValue rejectFn(std::function<JsValue(const std::vector<JsValue> &)>(
              [obj, eng](const std::vector<JsValue> &ra) {
                eng->rejectPromiseObj(obj, ra.empty() ? JsValue() : ra[0]);
                return JsValue();
              }));
          try {
            a[0].callback({resolveFn, rejectFn});
          } catch (RejectSignal &rs) {
            rejectPromiseObj(obj, rs.value);
          }
        }
        return p;
      }));
  promiseGlobal.setProperty(
      "resolve", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                     [this](const std::vector<JsValue> &a) {
                       JsValue v = a.empty() ? JsValue() : a[0];
                       if (v.objVal && v.objVal->isPromise) {
                         return v;
                       }
                       JsValue p = makePromise();
                       resolvePromiseObj(p.objVal, v);
                       return p;
                     })));
  promiseGlobal.setProperty(
      "reject", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                    [this](const std::vector<JsValue> &a) {
                      JsValue p = makePromise();
                      rejectPromiseObj(p.objVal, a.empty() ? JsValue() : a[0]);
                      return p;
                    })));
  promiseGlobal.setProperty(
      "all",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [this](const std::vector<JsValue> &a) -> JsValue {
            JsValue result = makePromise();
            JsValue arr = a.empty() ? JsValue() : a[0];
            if (!arr.objVal || !arr.objVal->isArray ||
                arr.objVal->elements.empty()) {
              JsValue empty(ValueType::Object);
              empty.objVal->isArray = true;
              resolvePromiseObj(result.objVal, empty);
              return result;
            }
            size_t n = arr.objVal->elements.size();
            auto remaining = std::make_shared<int>(static_cast<int>(n));
            auto results = std::make_shared<std::vector<JsValue>>(n);
            auto resultObj = result.objVal;
            JsEngine *eng = this;
            auto settleOne = [remaining, results, resultObj, eng](size_t idx,
                                                                  JsValue v) {
              (*results)[idx] = v;
              if (--(*remaining) == 0) {
                JsValue arrOut(ValueType::Object);
                arrOut.objVal->isArray = true;
                arrOut.objVal->elements = *results;
                eng->resolvePromiseObj(resultObj, arrOut);
              }
            };
            for (size_t idx = 0; idx < n; ++idx) {
              JsValue item = arr.objVal->elements[idx];
              if (item.objVal && item.objVal->isPromise) {
                promiseThen(
                    item.objVal,
                    JsValue(
                        std::function<JsValue(const std::vector<JsValue> &)>(
                            [idx, settleOne](const std::vector<JsValue> &v) {
                              settleOne(idx, v.empty() ? JsValue() : v[0]);
                              return JsValue();
                            })),
                    JsValue(
                        std::function<JsValue(const std::vector<JsValue> &)>(
                            [resultObj, eng](const std::vector<JsValue> &v) {
                              eng->rejectPromiseObj(
                                  resultObj, v.empty() ? JsValue() : v[0]);
                              return JsValue();
                            })));
              } else {
                settleOne(idx, item);
              }
            }
            return result;
          })));
  m_globalEnv->set("Promise", promiseGlobal);
}

} // namespace Js
} // namespace DesktopWebview
