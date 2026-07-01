#include "Css.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace DesktopWebview {
namespace Css {

namespace {

const std::string kWhitespace = " \t\r\n\f";

std::string Trim(const std::string &s) {
  size_t begin = s.find_first_not_of(kWhitespace);
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = s.find_last_not_of(kWhitespace);
  return s.substr(begin, end - begin + 1);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// CSS identifier characters (sufficient for tag/class/id names in MVP scope).
bool IsIdentChar(char c) {
  unsigned char uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) != 0 || c == '-' || c == '_';
}

std::vector<std::string> Split(const std::string &s, char delim) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : s) {
    if (c == delim) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  return parts;
}

// Whitespace-separated tokens (used for class lists and selector chains).
std::vector<std::string> SplitWhitespace(const std::string &s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) {
    out.push_back(tok);
  }
  return out;
}

// Remove /* ... */ comments. Unterminated comments swallow the rest of input,
// matching browser behaviour.
std::string StripComments(const std::string &css) {
  std::string out;
  out.reserve(css.size());
  size_t i = 0;
  const size_t n = css.size();
  while (i < n) {
    if (i + 1 < n && css[i] == '/' && css[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(css[i] == '*' && css[i + 1] == '/')) {
        ++i;
      }
      i += 2; // skip the closing */ (harmless if it runs past the end)
    } else {
      out.push_back(css[i++]);
    }
  }
  return out;
}

// Parse one compound selector like "div.foo#bar" or "*".
SimpleSelector ParseSimpleSelector(const std::string &token) {
  SimpleSelector sel;
  size_t i = 0;
  const size_t n = token.size();
  while (i < n) {
    char c = token[i];
    if (c == '*') {
      sel.universal = true;
      ++i;
    } else if (c == '.') {
      ++i;
      std::string name;
      while (i < n && IsIdentChar(token[i])) {
        name.push_back(token[i++]);
      }
      if (!name.empty()) {
        sel.classes.push_back(name);
      }
    } else if (c == '#') {
      ++i;
      std::string name;
      while (i < n && IsIdentChar(token[i])) {
        name.push_back(token[i++]);
      }
      if (!name.empty()) {
        sel.id = name;
      }
    } else if (c == ':') {
      // Pseudo-class (:x) or pseudo-element (::x). Pseudo-elements always target
      // a box we do not generate, so the rule can never match a real element.
      ++i;
      bool pseudoElement = false;
      if (i < n && token[i] == ':') {
        pseudoElement = true;
        ++i;
      }
      std::string name;
      while (i < n && IsIdentChar(token[i])) {
        name.push_back(token[i++]);
      }
      // Consume any functional argument, e.g. :nth-child(2n+1) or :not(.x).
      bool functional = false;
      if (i < n && token[i] == '(') {
        functional = true;
        int depth = 1;
        ++i;
        while (i < n && depth > 0) {
          if (token[i] == '(') {
            ++depth;
          } else if (token[i] == ')') {
            --depth;
          }
          ++i;
        }
      }
      name = ToLower(name);
      // Pseudos we can evaluate statically against the DOM. Everything else
      // (dynamic state like :hover/:focus, structural functions, pseudo-
      // elements) is marked non-matching rather than silently dropped.
      static const char *kSupported[] = {
          "checked",   "disabled",    "enabled",     "required",
          "optional",  "read-only",   "read-write",  "first-child",
          "last-child", "only-child", "root",        "link",
          "any-link"};
      bool supported = false;
      if (!pseudoElement && !functional) {
        for (const char *s : kSupported) {
          if (name == s) {
            supported = true;
            break;
          }
        }
      }
      if (supported) {
        sel.pseudoClasses.push_back(name);
      } else {
        sel.neverMatches = true;
      }
    } else if (c == '[') {
      // Attribute selector: [attr], [attr=val], [attr~=val], [attr|=val],
      // [attr^=val], [attr$=val], [attr*=val], with optional quotes on val.
      ++i;
      AttributeSelector attr;
      std::string name;
      while (i < n && (IsIdentChar(token[i]) || token[i] == ':')) {
        name.push_back(token[i++]);
      }
      attr.name = ToLower(name);
      if (i < n && token[i] != '=' && token[i] != ']' &&
          std::string("~|^$*").find(token[i]) != std::string::npos) {
        attr.op = token[i++];
      }
      if (i < n && token[i] == '=') {
        if (attr.op == '\0') {
          attr.op = '=';
        }
        ++i;
      }
      // Value, possibly quoted.
      if (i < n && (token[i] == '"' || token[i] == '\'')) {
        char quote = token[i++];
        while (i < n && token[i] != quote) {
          attr.value.push_back(token[i++]);
        }
        if (i < n) {
          ++i; // closing quote
        }
      } else {
        while (i < n && token[i] != ']') {
          attr.value.push_back(token[i++]);
        }
        attr.value = Trim(attr.value);
      }
      while (i < n && token[i] != ']') {
        ++i;
      }
      if (i < n) {
        ++i; // consume ']'
      }
      if (!attr.name.empty()) {
        sel.attributes.push_back(std::move(attr));
      }
    } else if (IsIdentChar(c)) {
      std::string name;
      while (i < n && IsIdentChar(token[i])) {
        name.push_back(token[i++]);
      }
      sel.tag = ToLower(name);
    } else {
      ++i; // skip anything we do not model (combinators, etc.)
    }
  }
  return sel;
}

// Parse a single selector chain ("div .foo a") into its compound components.
// Bare combinator tokens (>, +, ~) are ignored, collapsing them to descendant
// combinators for MVP purposes.
Selector ParseSelector(const std::string &text) {
  Selector sel;
  for (const std::string &token : SplitWhitespace(text)) {
    if (token == ">" || token == "+" || token == "~") {
      continue;
    }
    sel.components.push_back(ParseSimpleSelector(token));
  }
  return sel;
}

// Parse a comma-separated selector list.
std::vector<Selector> ParseSelectorList(const std::string &text) {
  std::vector<Selector> selectors;
  for (const std::string &part : Split(text, ',')) {
    std::string trimmed = Trim(part);
    if (trimmed.empty()) {
      continue;
    }
    Selector sel = ParseSelector(trimmed);
    if (!sel.components.empty()) {
      selectors.push_back(std::move(sel));
    }
  }
  return selectors;
}

// True when `element` satisfies a single compound selector (no combinators).
// True if `element` is the first / last element among its element siblings.
bool IsFirstElementChild(const Wrapper::Node &element) {
  Wrapper::Node parent = element.parent();
  if (!parent.valid()) {
    return false;
  }
  std::vector<Wrapper::Node> sibs = parent.children();
  return !sibs.empty() && sibs.front().raw() == element.raw();
}
bool IsLastElementChild(const Wrapper::Node &element) {
  Wrapper::Node parent = element.parent();
  if (!parent.valid()) {
    return false;
  }
  std::vector<Wrapper::Node> sibs = parent.children();
  return !sibs.empty() && sibs.back().raw() == element.raw();
}

// Evaluate one attribute selector against `element`.
bool MatchAttribute(const AttributeSelector &attr,
                    const Wrapper::Node &element) {
  if (!element.hasAttribute(attr.name)) {
    return false;
  }
  if (attr.op == '\0') {
    return true; // bare presence test
  }
  std::string have = element.attribute(attr.name);
  const std::string &want = attr.value;
  switch (attr.op) {
  case '=':
    return have == want;
  case '~': { // whitespace-separated list contains `want`
    std::vector<std::string> parts = SplitWhitespace(have);
    return std::find(parts.begin(), parts.end(), want) != parts.end();
  }
  case '|': // exactly `want`, or `want` followed by '-'
    return have == want ||
           (have.size() > want.size() && have.compare(0, want.size(), want) == 0 &&
            have[want.size()] == '-');
  case '^': // prefix
    return !want.empty() && have.size() >= want.size() &&
           have.compare(0, want.size(), want) == 0;
  case '$': // suffix
    return !want.empty() && have.size() >= want.size() &&
           have.compare(have.size() - want.size(), want.size(), want) == 0;
  case '*': // substring
    return !want.empty() && have.find(want) != std::string::npos;
  default:
    return false;
  }
}

// Evaluate a statically-decidable pseudo-class against `element`.
bool MatchPseudoClass(const std::string &pseudo, const Wrapper::Node &element) {
  std::string tag = ToLower(element.name());
  if (pseudo == "checked") {
    return element.hasAttribute("checked");
  }
  if (pseudo == "disabled") {
    return element.hasAttribute("disabled");
  }
  if (pseudo == "enabled") {
    return !element.hasAttribute("disabled");
  }
  if (pseudo == "required") {
    return element.hasAttribute("required");
  }
  if (pseudo == "optional") {
    return !element.hasAttribute("required");
  }
  if (pseudo == "read-only") {
    return element.hasAttribute("readonly");
  }
  if (pseudo == "read-write") {
    return !element.hasAttribute("readonly");
  }
  if (pseudo == "first-child") {
    return IsFirstElementChild(element);
  }
  if (pseudo == "last-child") {
    return IsLastElementChild(element);
  }
  if (pseudo == "only-child") {
    return IsFirstElementChild(element) && IsLastElementChild(element);
  }
  if (pseudo == "root") {
    return !element.parent().isElement();
  }
  if (pseudo == "link" || pseudo == "any-link") {
    return (tag == "a" || tag == "area") && element.hasAttribute("href");
  }
  return false;
}

bool MatchSimple(const SimpleSelector &sel, const Wrapper::Node &element) {
  if (!element.isElement()) {
    return false;
  }
  if (sel.neverMatches) {
    return false;
  }
  if (sel.isUnconstrained() && !sel.universal) {
    return false;
  }
  if (!sel.tag.empty() && sel.tag != ToLower(element.name())) {
    return false;
  }
  if (!sel.id.empty() && element.attribute("id") != sel.id) {
    return false;
  }
  if (!sel.classes.empty()) {
    std::vector<std::string> have = SplitWhitespace(element.attribute("class"));
    for (const std::string &want : sel.classes) {
      if (std::find(have.begin(), have.end(), want) == have.end()) {
        return false;
      }
    }
  }
  for (const AttributeSelector &attr : sel.attributes) {
    if (!MatchAttribute(attr, element)) {
      return false;
    }
  }
  for (const std::string &pseudo : sel.pseudoClasses) {
    if (!MatchPseudoClass(pseudo, element)) {
      return false;
    }
  }
  return true;
}

bool EvaluateMediaQuery(const std::string &prelude, float viewportWidth) {
  std::string cond = prelude;
  size_t mediaPos = cond.find("@media");
  if (mediaPos != std::string::npos) {
    cond = cond.substr(mediaPos + 6);
  }
  cond = ToLower(Trim(cond));
  if (cond.empty()) {
    return true;
  }

  // If the query contains "print", we want to return false because this is a
  // screen display.
  if (cond.find("print") != std::string::npos) {
    return false;
  }

  bool result = true;
  size_t pos = 0;
  while (true) {
    size_t minPos = cond.find("min-width", pos);
    size_t maxPos = cond.find("max-width", pos);

    if (minPos == std::string::npos && maxPos == std::string::npos) {
      break;
    }

    if (minPos != std::string::npos &&
        (maxPos == std::string::npos || minPos < maxPos)) {
      size_t colon = cond.find(':', minPos);
      if (colon != std::string::npos) {
        size_t numStart = cond.find_first_of("0123456789.", colon);
        if (numStart != std::string::npos) {
          char *endptr = nullptr;
          double val = std::strtod(cond.c_str() + numStart, &endptr);
          if (endptr != cond.c_str() + numStart) {
            if (viewportWidth < val) {
              result = false;
            }
          }
        }
      }
      pos = minPos + 9;
    } else {
      size_t colon = cond.find(':', maxPos);
      if (colon != std::string::npos) {
        size_t numStart = cond.find_first_of("0123456789.", colon);
        if (numStart != std::string::npos) {
          char *endptr = nullptr;
          double val = std::strtod(cond.c_str() + numStart, &endptr);
          if (endptr != cond.c_str() + numStart) {
            if (viewportWidth > val) {
              result = false;
            }
          }
        }
      }
      pos = maxPos + 9;
    }
  }

  return result;
}

} // namespace

Specificity Selector::specificity() const {
  Specificity s;
  for (const SimpleSelector &comp : components) {
    if (!comp.id.empty()) {
      ++s.ids;
    }
    // Classes, attribute selectors and pseudo-classes all contribute to the
    // class column of specificity.
    s.classes += static_cast<int>(comp.classes.size());
    s.classes += static_cast<int>(comp.attributes.size());
    s.classes += static_cast<int>(comp.pseudoClasses.size());
    if (!comp.tag.empty()) {
      ++s.types;
    }
  }
  return s;
}

std::vector<Declaration> parseDeclarations(const std::string &block) {
  std::vector<Declaration> decls;
  for (const std::string &raw : Split(block, ';')) {
    std::string text = Trim(raw);
    if (text.empty()) {
      continue;
    }
    size_t colon = text.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string property = Trim(text.substr(0, colon));
    std::string value = Trim(text.substr(colon + 1));
    if (property.empty() || value.empty()) {
      continue;
    }

    Declaration decl;
    decl.property = ToLower(property);

    // Detect a trailing "!important" (case-insensitive) and strip it.
    std::string lowered = ToLower(value);
    size_t bang = lowered.rfind("!important");
    if (bang != std::string::npos &&
        Trim(lowered.substr(bang + std::string("!important").size())).empty()) {
      decl.important = true;
      value = Trim(value.substr(0, bang));
    }
    if (value.empty()) {
      continue;
    }
    decl.value = value;
    decls.push_back(std::move(decl));
  }
  return decls;
}

Stylesheet parse(const std::string &cssRaw, float viewportWidth) {
  Stylesheet sheet;
  std::string css = StripComments(cssRaw);
  size_t i = 0;
  const size_t n = css.size();

  while (i < n) {
    // Skip leading whitespace.
    while (i < n && std::isspace(static_cast<unsigned char>(css[i]))) {
      ++i;
    }
    if (i >= n) {
      break;
    }

    // Skip single-line at-rules (e.g. @charset "UTF-8";)
    if (css[i] == '@') {
      size_t j = i;
      while (j < n && css[j] != ';' && css[j] != '{' && css[j] != '}') {
        ++j;
      }
      if (j < n && css[j] == ';') {
        i = j + 1;
        continue;
      }
    }

    // Read the selector/prelude text up to the block opener.
    size_t start = i;
    while (i < n && css[i] != '{' && css[i] != '}') {
      ++i;
    }
    if (i >= n) {
      break; // no block; trailing junk
    }
    if (css[i] == '}') {
      ++i; // stray closing brace
      continue;
    }

    std::string prelude = Trim(css.substr(start, i - start));
    ++i; // consume '{'

    // Handle block at-rules (e.g. @media, @keyframes, @font-face)
    if (!prelude.empty() && prelude[0] == '@') {
      size_t blockStart = i;
      int depth = 1;
      while (i < n && depth > 0) {
        if (css[i] == '{') {
          ++depth;
        } else if (css[i] == '}') {
          --depth;
        }
        ++i;
      }
      size_t blockEnd = i - 1; // index of matching '}'

      // If it is a media query, conditionally parse its contents
      if (prelude.rfind("@media", 0) == 0) {
        if (EvaluateMediaQuery(prelude, viewportWidth)) {
          std::string blockContent =
              css.substr(blockStart, blockEnd - blockStart);
          Stylesheet innerSheet = parse(blockContent, viewportWidth);
          sheet.rules.insert(sheet.rules.end(),
                             std::make_move_iterator(innerSheet.rules.begin()),
                             std::make_move_iterator(innerSheet.rules.end()));
        }
      }
      continue;
    }

    // Read the declaration block up to the closing brace.
    size_t declStart = i;
    while (i < n && css[i] != '}') {
      ++i;
    }
    std::string declText = css.substr(declStart, i - declStart);
    if (i < n) {
      ++i; // consume '}'
    }

    Rule rule;
    rule.selectors = ParseSelectorList(prelude);
    rule.declarations = parseDeclarations(declText);
    if (!rule.selectors.empty() && !rule.declarations.empty()) {
      sheet.rules.push_back(std::move(rule));
    }
  }
  return sheet;
}

bool matches(const Selector &selector, const Wrapper::Node &node) {
  const std::vector<SimpleSelector> &comps = selector.components;
  if (comps.empty()) {
    return false;
  }
  // The right-most component must match the node itself.
  if (!MatchSimple(comps.back(), node)) {
    return false;
  }
  // Each remaining component (right to left) must match some ancestor, in
  // order. Greedy walk up the tree is correct for pure descendant combinators.
  int idx = static_cast<int>(comps.size()) - 2;
  Wrapper::Node current = node.parent();
  while (idx >= 0 && current.valid()) {
    if (MatchSimple(comps[idx], current)) {
      --idx;
    }
    current = current.parent();
  }
  return idx < 0;
}

std::map<std::string, std::string> computeStyle(const Stylesheet &sheet,
                                                const Wrapper::Node &node) {
  // A candidate declaration plus the keys used to order the cascade. Higher
  // tuple (tier, specificity, order) wins and is therefore applied last.
  struct Candidate {
    Declaration decl;
    int tier; // 0 author, 1 inline, 2 author !important, 3 inline !important
    Specificity spec;
    int order;
  };

  std::vector<Candidate> candidates;
  int order = 0;

  for (const Rule &rule : sheet.rules) {
    // A rule may list several selectors; use the highest-specificity one that
    // matches this node.
    bool matched = false;
    Specificity best;
    for (const Selector &sel : rule.selectors) {
      if (matches(sel, node)) {
        Specificity s = sel.specificity();
        if (!matched || best < s) {
          best = s;
        }
        matched = true;
      }
    }
    if (!matched) {
      continue;
    }
    for (const Declaration &decl : rule.declarations) {
      candidates.push_back({decl, decl.important ? 2 : 0, best, order++});
    }
  }

  // Inline style="" declarations sit in their own (higher) tier.
  for (const Declaration &decl : parseDeclarations(node.attribute("style"))) {
    candidates.push_back(
        {decl, decl.important ? 3 : 1, Specificity{}, order++});
  }

  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate &a, const Candidate &b) {
                     if (a.tier != b.tier) {
                       return a.tier < b.tier;
                     }
                     if (!(a.spec == b.spec)) {
                       return a.spec < b.spec;
                     }
                     return a.order < b.order;
                   });

  std::map<std::string, std::string> computed;
  for (const Candidate &c : candidates) {
    computed[c.decl.property] =
        c.decl.value; // later (winning) entries override
  }
  return computed;
}

} // namespace Css
} // namespace DesktopWebview
