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

class JsEngine {
public:
  explicit JsEngine(DomInterface dom);
  void execute(const std::string &code);

  std::shared_ptr<JsEnvironment> getGlobalEnv() const { return m_globalEnv; }
  
  JsValue getOrElement(const std::string &id);
  void triggerEvent(const std::string &eventType, const std::string &elementId);

private:
  DomInterface m_dom;
  std::shared_ptr<JsEnvironment> m_globalEnv;
  std::map<std::string, JsValue> m_elementCache;
};

} // namespace Js
} // namespace DesktopWebview

#endif // JS_ENGINE_HPP
