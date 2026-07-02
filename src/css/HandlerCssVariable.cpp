#include "HandlerCssVariable.hpp"

#include <functional>

namespace DesktopWebview {
namespace Css {

namespace {

std::string Trim(const std::string &s) {
  const std::string ws = " \t\r\n\f";
  size_t begin = s.find_first_not_of(ws);
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

// Replace the first var(...) call in `value` with its resolved value (looked
// up via `lookup`, which returns "" for an undeclared variable), consuming a
// declared fallback ("var(--x, fallback)") when the variable itself is
// undeclared. Handles nested parens in the fallback (e.g. a fallback that is
// itself another var() or a color function like rgb(...)) by matching
// balanced parens rather than stopping at the first ')'. Returns false (and
// leaves `value` untouched) when there is no var(...) call, or its parens
// never balance.
bool ReplaceOneVarCall(std::string &value,
                       const std::function<std::string(const std::string &)> &lookup) {
  size_t callStart = value.find("var(");
  if (callStart == std::string::npos) {
    return false;
  }
  size_t argsStart = callStart + 4;
  size_t i = argsStart;
  int depth = 1;
  while (i < value.size() && depth > 0) {
    if (value[i] == '(') {
      ++depth;
    } else if (value[i] == ')') {
      --depth;
    }
    if (depth > 0) {
      ++i;
    }
  }
  if (depth != 0) {
    return false; // unbalanced "var(" with no matching ')': leave as-is
  }
  size_t argsEnd = i; // index of the matching ')'
  std::string args = value.substr(argsStart, argsEnd - argsStart);

  // Split into the variable name and an optional fallback at the first
  // top-level comma (a comma nested inside the fallback's own parens does
  // not count, e.g. "var(--x, rgb(0,0,0))").
  std::string name = args, fallback;
  bool hasFallback = false;
  int nestDepth = 0;
  for (size_t k = 0; k < args.size(); ++k) {
    if (args[k] == '(') {
      ++nestDepth;
    } else if (args[k] == ')') {
      --nestDepth;
    } else if (args[k] == ',' && nestDepth == 0) {
      name = args.substr(0, k);
      fallback = Trim(args.substr(k + 1));
      hasFallback = true;
      break;
    }
  }
  name = Trim(name);
  if (name.rfind("--", 0) == 0) {
    name = name.substr(2);
  }

  std::string resolved = lookup(name);
  std::string replacement =
      !resolved.empty() ? resolved : (hasFallback ? fallback : "");
  value = value.substr(0, callStart) + replacement +
         value.substr(argsEnd + 1);
  return true;
}

} // namespace

std::string getCssVariable(const Wrapper::Node &node, const Stylesheet &sheet,
                           const std::string &name) {
  const std::string key = "--" + name;
  Wrapper::Node current = node;
  int guard = 0;
  // Bounded walk: a DOM this deep would indicate a cycle/corruption, not a
  // real page, and the loop must not hang on one.
  while (current.valid() && guard++ < 256) {
    if (current.isElement()) {
      std::map<std::string, std::string> style = computeStyle(sheet, current);
      auto it = style.find(key);
      if (it != style.end() && !it->second.empty()) {
        return Trim(it->second);
      }
    }
    current = current.parent();
  }
  return "";
}

void resolveCssVariables(const Wrapper::Node &node, const Stylesheet &sheet,
                         std::map<std::string, std::string> &style) {
  auto lookup = [&](const std::string &name) {
    return getCssVariable(node, sheet, name);
  };
  for (auto &kv : style) {
    // A variable's own value may reference another var() ("--a: var(--b)"),
    // so this loop is not limited to non-"--" properties. Bounded per-value
    // so a cyclic reference (var(--a) inside --a's own definition) cannot
    // hang rendering -- it just stops substituting and leaves the leftover
    // var(...) text in place.
    int guard = 0;
    while (kv.second.find("var(") != std::string::npos && guard++ < 32) {
      if (!ReplaceOneVarCall(kv.second, lookup)) {
        break;
      }
    }
  }
}

} // namespace Css
} // namespace DesktopWebview
