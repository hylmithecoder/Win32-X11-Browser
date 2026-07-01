#ifndef VARIABEL_HPP
#define VARIABEL_HPP

#include "JsEngine.hpp"

namespace DesktopWebview {
namespace Js {

bool truthy(const JsValue &v);
double toNum(const JsValue &v);

} // namespace Js
} // namespace DesktopWebview

#endif
