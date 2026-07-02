#ifndef HANDLER_CSS_VARIABLE_HPP
#define HANDLER_CSS_VARIABLE_HPP

#include "Css.hpp"
#include "Wrapper.hpp"

#include <map>
#include <string>

namespace DesktopWebview {
namespace Css {

// CSS custom properties ("CSS variables"). Css::computeStyle already captures
// a "--foo: bar" declaration as an ordinary Declaration{property="--foo"} --
// no parser changes are needed for that. What is missing is the other half:
// custom properties *inherit* down the tree (unlike most CSS properties,
// which this engine's cascade does not model inheritance for at all), and
// any property value can *reference* one via var(--foo). This header is the
// "getter" (read a custom property's inherited value straight off the CSS
// cascade) and "setter" (substitute that value into an element's already-
// cascaded style map) side of that -- the two operations Bootstrap/Tailwind's
// theming relies on (e.g. ":root{--bs-primary:#0d6efd}" +
// ".btn-primary{background-color:var(--bs-primary)}").

// Look up custom property `--name` (pass without the leading "--") for
// `node`, walking up its ancestors -- including `node` itself -- until a
// declaration for it is found (custom properties inherit; the nearest
// declared value wins, exactly like Browser's other ResolveInherited*
// helpers do for ordinary properties). Returns "" if never declared on any
// ancestor.
std::string getCssVariable(const Wrapper::Node &node, const Stylesheet &sheet,
                           const std::string &name);

// Resolve every var(--name) / var(--name, fallback) call inside `style` (the
// already-cascaded property -> value map for `node`, as returned by
// computeStyle) to a concrete value, in place. A custom property's own value
// may itself reference another var(), so resolution repeats per-value until
// no var() calls remain (bounded to tolerate a cyclic reference without
// looping forever). A var() whose variable is undefined resolves to its
// fallback text (itself resolved), or "" with no fallback.
void resolveCssVariables(const Wrapper::Node &node, const Stylesheet &sheet,
                         std::map<std::string, std::string> &style);

} // namespace Css
} // namespace DesktopWebview

#endif // HANDLER_CSS_VARIABLE_HPP
