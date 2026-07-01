#include "../include/Debugger.hpp"
#include "../include/Documents.hpp"
#include "../include/Font.hpp"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

using namespace Debug;

namespace DesktopWebview {
namespace Documents {

// -----------------------------------------------------------------------
// PDF types
// -----------------------------------------------------------------------

using Ref = std::pair<int, int>;

enum class Kind {
  kNull,
  kBool,
  kInt,
  kReal,
  kName,
  kStr,
  kDict,
  kArray,
  kRef,
  kStream
};

struct Object {
  Kind kind = Kind::kNull;
  bool b = false;
  int i = 0;
  double f = 0.0;
  std::string s;
  std::vector<std::pair<std::string, Object>> dict;
  std::vector<Object> arr;
  Ref ref{0, 0};
  std::vector<std::uint8_t> stream;
};

static const Object *DictFind(const Object &obj, const char *key) {
  if (obj.kind != Kind::kDict && obj.kind != Kind::kStream)
    return nullptr;
  for (const auto &kv : obj.dict) {
    // Keys from readName() include the leading '/', so match both
    // "/Key" and "Key" forms.
    if (kv.first == key || kv.first == std::string("/") + key)
      return &kv.second;
  }
  return nullptr;
}

static double GetNum(const Object &o) {
  if (o.kind == Kind::kReal)
    return o.f;
  if (o.kind == Kind::kInt)
    return static_cast<double>(o.i);
  return 0.0;
}

// -----------------------------------------------------------------------
// PDF tokeniser
// -----------------------------------------------------------------------

class Tokeniser {
  const std::uint8_t *d;
  size_t sz, pos;

public:
  Tokeniser(const std::uint8_t *data, size_t size)
      : d(data), sz(size), pos(0) {}

  size_t tell() const { return pos; }
  void seek(size_t p) { pos = p; }

  void skipSpace() {
    while (pos < sz) {
      char c = static_cast<char>(d[pos]);
      if (c == '%') {
        while (pos < sz && d[pos] != '\n' && d[pos] != '\r')
          pos++;
        continue;
      }
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f') {
        pos++;
        continue;
      }
      break;
    }
  }

  bool eof() {
    skipSpace();
    return pos >= sz;
  }

  Object read() {
    skipSpace();
    if (pos >= sz)
      return {};
    char c = static_cast<char>(d[pos]);

    if (c == '+' || c == '-' || (c >= '0' && c <= '9') || c == '.') {
      Object num = readNum();
      if (num.kind != Kind::kInt)
        return num;
      // Check for "n g R" reference pattern
      size_t save = pos;
      skipSpace();
      Object num2;
      bool gotSecond = false;
      if (pos < sz && ((d[pos] >= '0' && d[pos] <= '9') || d[pos] == '+' ||
                       d[pos] == '-')) {
        num2 = readNum();
        gotSecond = (num2.kind == Kind::kInt);
      }
      if (gotSecond) {
        size_t save2 = pos;
        skipSpace();
        if (pos < sz && d[pos] == 'R') {
          size_t afterR = pos + 1;
          bool delimOk = (afterR >= sz) ||
                         std::strchr(" \n\r\t/<>[](){}%",
                                     static_cast<char>(d[afterR])) != nullptr;
          if (delimOk) {
            Object ref;
            ref.kind = Kind::kRef;
            ref.ref = {num.i, num2.i};
            pos++;
            return ref;
          }
        }
        pos = save2;
      }
      pos = save;
      return num;
    }
    if (pos + 3 < sz && d[pos] == 't' && d[pos + 1] == 'r' &&
        d[pos + 2] == 'u' && d[pos + 3] == 'e') {
      pos += 4;
      Object o;
      o.kind = Kind::kBool;
      o.b = true;
      return o;
    }
    if (pos + 4 < sz && d[pos] == 'f' && d[pos + 1] == 'a' &&
        d[pos + 2] == 'l' && d[pos + 3] == 's' && d[pos + 4] == 'e') {
      pos += 5;
      Object o;
      o.kind = Kind::kBool;
      o.b = false;
      return o;
    }
    if (pos + 3 < sz && d[pos] == 'n' && d[pos + 1] == 'u' &&
        d[pos + 2] == 'l' && d[pos + 3] == 'l') {
      pos += 4;
      return {};
    }
    if (c == '/')
      return readName();
    if (c == '(')
      return readStr();
    if (c == '<') {
      if (pos + 1 < sz && d[pos + 1] == '<')
        return readDict();
      return readHexStr();
    }
    if (c == '[')
      return readArray();
    if (c == ']') {
      pos++;
      return {};
    }
    if (c == '>') {
      pos++;
      if (pos < sz && d[pos] == '>')
        pos++;
      return {};
    }

    // Try keyword tokens
    struct {
      const char *s;
    } keywords[] = {{"obj"},  {"endobj"},  {"stream"},    {"endstream"}, {"R"},
                    {"xref"}, {"trailer"}, {"startxref"}, {nullptr}};
    for (int k = 0; keywords[k].s; ++k) {
      size_t len = std::strlen(keywords[k].s);
      if (pos + len <= sz && std::memcmp(d + pos, keywords[k].s, len) == 0) {
        if (pos + len >= sz ||
            std::strchr(" \n\r\t\f/<>[](){}%", d[pos + len])) {
          pos += len;
          Object o;
          o.kind = Kind::kName;
          o.s = "/";
          o.s += keywords[k].s;
          return o;
        }
      }
    }

    // Read bare word/keyword/operator
    size_t start = pos;
    while (pos < sz) {
      char c2 = static_cast<char>(d[pos]);
      if (std::strchr(" \n\r\t\f/<>[](){}%", c2)) {
        break;
      }
      pos++;
    }
    if (pos > start) {
      Object o;
      o.kind = Kind::kName;
      o.s = std::string(reinterpret_cast<const char *>(d + start), pos - start);
      return o;
    }

    pos++;
    return {};
  }

private:
  Object readNum() {
    const char *start = reinterpret_cast<const char *>(d + pos);
    char *end = nullptr;
    double v = std::strtod(start, &end);
    if (end == start)
      return {};
    size_t n = static_cast<size_t>(end - start);
    pos += n;
    bool hasDot = false;
    for (size_t j = 0; j < n; ++j) {
      if (start[j] == '.') {
        hasDot = true;
        break;
      }
    }
    Object o;
    if (hasDot) {
      o.kind = Kind::kReal;
      o.f = v;
    } else {
      o.kind = Kind::kInt;
      o.i = static_cast<int>(std::llround(v));
    }
    return o;
  }

  Object readName() {
    Object o;
    o.kind = Kind::kName;
    o.s = "/";
    pos++;
    while (pos < sz) {
      char c = static_cast<char>(d[pos]);
      if (std::strchr(" \n\r\t\f/[]<>(){}%", c))
        break;
      if (c == '#') {
        pos++;
        auto hd = [&]() -> char {
          auto val = [](char x) -> int {
            if (x >= '0' && x <= '9')
              return x - '0';
            if (x >= 'a' && x <= 'f')
              return x - 'a' + 10;
            if (x >= 'A' && x <= 'F')
              return x - 'A' + 10;
            return 0;
          };
          char hi = static_cast<char>(d[pos]);
          pos++;
          char lo = static_cast<char>(d[pos]);
          return static_cast<char>((val(hi) << 4) | val(lo));
        };
        o.s += hd();
      } else {
        o.s += c;
        pos++;
      }
    }
    return o;
  }

  Object readStr() {
    Object o;
    o.kind = Kind::kStr;
    pos++;
    int depth = 0;
    while (pos < sz) {
      char c = static_cast<char>(d[pos]);
      if (c == '\\') {
        pos++;
        if (pos >= sz)
          break;
        char esc = static_cast<char>(d[pos]);
        switch (esc) {
        case 'n':
          o.s += '\n';
          break;
        case 'r':
          o.s += '\r';
          break;
        case 't':
          o.s += '\t';
          break;
        case '(':
          o.s += '(';
          break;
        case ')':
          o.s += ')';
          break;
        case '\\':
          o.s += '\\';
          break;
        default:
          if (esc >= '0' && esc <= '7') {
            int oct = esc - '0';
            if (pos + 1 < sz && d[pos + 1] >= '0' && d[pos + 1] <= '7') {
              pos++;
              oct = oct * 8 + (d[pos] - '0');
            }
            if (pos + 1 < sz && d[pos + 1] >= '0' && d[pos + 1] <= '7') {
              pos++;
              oct = oct * 8 + (d[pos] - '0');
            }
            o.s += static_cast<char>(oct);
          } else
            o.s += esc;
        }
        pos++;
        continue;
      }
      if (c == '(') {
        depth++;
        o.s += c;
        pos++;
        continue;
      }
      if (c == ')') {
        if (depth == 0) {
          pos++;
          break;
        }
        depth--;
        o.s += c;
        pos++;
        continue;
      }
      if (c == '\r') {
        o.s += '\n';
        pos++;
        if (pos < sz && d[pos] == '\n')
          pos++;
        continue;
      }
      if (c == '\n') {
        o.s += '\n';
        pos++;
        continue;
      }
      o.s += c;
      pos++;
    }
    return o;
  }

  Object readHexStr() {
    Object o;
    o.kind = Kind::kStr;
    pos++;
    std::string hex;
    while (pos < sz) {
      char c = static_cast<char>(d[pos]);
      if (c == '>') {
        pos++;
        break;
      }
      if (std::strchr(" \n\r\t", c)) {
        pos++;
        continue;
      }
      hex += c;
      pos++;
    }
    auto val = [](char x) -> int {
      if (x >= '0' && x <= '9')
        return x - '0';
      if (x >= 'a' && x <= 'f')
        return x - 'a' + 10;
      if (x >= 'A' && x <= 'F')
        return x - 'A' + 10;
      return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
      o.s += static_cast<char>((val(hex[i]) << 4) | val(hex[i + 1]));
    if (hex.size() % 2 == 1)
      o.s += static_cast<char>((val(hex.back()) << 4));
    return o;
  }

  Object readDict() {
    Object o;
    o.kind = Kind::kDict;
    pos += 2;
    while (pos < sz) {
      skipSpace();
      if (pos + 1 < sz && d[pos] == '>' && d[pos + 1] == '>') {
        pos += 2;
        break;
      }
      if (eof())
        break;
      Object key = read();
      if (key.kind != Kind::kName)
        break;
      Object val = read();
      o.dict.emplace_back(key.s, std::move(val));
    }
    return o;
  }

  Object readArray() {
    Object o;
    o.kind = Kind::kArray;
    pos++;
    while (pos < sz) {
      skipSpace();
      if (pos < sz && d[pos] == ']') {
        pos++;
        break;
      }
      if (eof())
        break;
      o.arr.push_back(read());
    }
    return o;
  }
};

// -----------------------------------------------------------------------
// Resolve refs
// -----------------------------------------------------------------------

static const Object *Resolve(const std::map<Ref, Object> &objs,
                             const Object *o) {
  if (!o)
    return nullptr;
  if (o->kind != Kind::kRef)
    return o;
  auto it = objs.find(o->ref);
  if (it == objs.end())
    return nullptr;
  return Resolve(objs, &it->second);
}

static Object MakeRef(Ref r) {
  Object o;
  o.kind = Kind::kRef;
  o.ref = r;
  return o;
}

// -----------------------------------------------------------------------
// Walk page tree
// -----------------------------------------------------------------------

static void CollectPageRefs(const std::map<Ref, Object> &objs,
                            const Object &pagesDict, std::vector<Ref> &out) {
  const Object *type = DictFind(pagesDict, "Type");
  if (!type || type->kind != Kind::kName)
    return;
  if (type->s == "/Page")
    return;
  if (type->s != "/Pages")
    return;
  const Object *kids = DictFind(pagesDict, "Kids");
  if (!kids || kids->kind != Kind::kArray)
    return;
  for (const Object &kid : kids->arr) {
    if (kid.kind == Kind::kRef) {
      const Object *node = Resolve(objs, &kid);
      if (!node)
        continue;
      const Object *kt = DictFind(*node, "Type");
      if (!kt || kt->kind != Kind::kName)
        continue;
      if (kt->s == "/Page") {
        out.push_back(kid.ref);
      } else if (kt->s == "/Pages") {
        CollectPageRefs(objs, *node, out);
      }
    }
  }
}

// -----------------------------------------------------------------------
// Content stream text extraction
// -----------------------------------------------------------------------

struct TextSpan {
  std::string text;
  double tx, ty;
  double fontSize;
};

// PDF content stream token — operator or operand.
struct ContentToken {
  enum { Operand, Operator } type;
  Object obj;
  std::string op;
};

static std::vector<TextSpan>
ParseText(const std::vector<std::uint8_t> &stream) {

  // Known multi-char PDF content-stream operator names
  auto isOp = [](const std::string &s) -> bool {
    static const char *ops[] = {
        "BT", "ET",  "Tf",  "Td",  "TD", "Tm", "TJ", "Tj",  "T*",   "TL",
        "Tr", "Ts",  "Tw",  "Tc",  "Tz", "q",  "Q",  "cm",  "w",    "J",
        "j",  "M",   "d",   "S",   "s",  "f",  "F",  "B",   "b",    "n",
        "W",  "h",   "m",   "l",   "c",  "v",  "y",  "re",  "f*",   "B*",
        "b*", "W*",  "Do",  "CS",  "cs", "SC", "sc", "SCN", "scn",  "G",
        "g",  "RG",  "rg",  "K",   "k",  "ri", "i",  "gs",  "sh",   "MP",
        "DP", "BMC", "BDC", "EMC", "BX", "EX", "'",  "\"",  nullptr};
    for (int i = 0; ops[i]; ++i)
      if (s == ops[i])
        return true;
    return false;
  };

  std::vector<ContentToken> toks;
  Tokeniser ts(stream.data(), stream.size());

  while (!ts.eof()) {
    Object o = ts.read();
    if (o.kind == Kind::kNull)
      break;

    if (o.kind == Kind::kName) {
      std::string name = o.s;
      // Remove leading '/' so "/BT" → "BT"
      if (!name.empty() && name[0] == '/')
        name = name.substr(1);
      ContentToken t;
      if (isOp(name)) {
        t.type = ContentToken::Operator;
        t.op = std::move(name);
      } else {
        // Not a recognised operator: treat as name operand (e.g. /F1 in Tf)
        t.type = ContentToken::Operand;
        t.obj = std::move(o);
      }
      toks.push_back(std::move(t));
    } else {
      ContentToken t;
      t.type = ContentToken::Operand;
      t.obj = std::move(o);
      toks.push_back(std::move(t));
    }
  }

  // ---- Stack Machine State -------------------------------------------------
  std::vector<TextSpan> out;
  std::vector<const Object *> stack;

  double tm_tx = 0, tm_ty = 0; // current text position (from Tm)
  double lm_tx = 0, lm_ty = 0; // line matrix position
  double fontSize = 12;
  double leading = 0;

  // Pop helper to safely extract count elements in forward order
  auto pop = [&](int count) -> std::vector<const Object *> {
    std::vector<const Object *> popped;
    popped.reserve(count);
    int available = std::min(count, static_cast<int>(stack.size()));
    int nulls = count - available;
    for (int k = 0; k < nulls; ++k) {
      popped.push_back(nullptr);
    }
    if (available > 0) {
      popped.insert(popped.end(), stack.end() - available, stack.end());
      stack.erase(stack.end() - available, stack.end());
    }
    return popped;
  };

  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].type == ContentToken::Operand) {
      stack.push_back(&toks[i].obj);
      continue;
    }

    const std::string &op = toks[i].op;

    if (op == "BT") {
      tm_tx = 0;
      tm_ty = 0;
      lm_tx = 0;
      lm_ty = 0;
      stack.clear();
    } else if (op == "ET") {
      stack.clear();
    } else if (op == "TL") {
      auto args = pop(1);
      if (args[0])
        leading = GetNum(*args[0]);
    } else if (op == "Tf") {
      auto args = pop(2); // fontName, fontSize
      if (args[1])
        fontSize = GetNum(*args[1]);
      if (fontSize <= 0)
        fontSize = 12;
    } else if (op == "Td") {
      auto args = pop(2); // tx, ty
      if (args[0] && args[1]) {
        lm_tx += GetNum(*args[0]);
        lm_ty += GetNum(*args[1]);
        tm_tx = lm_tx;
        tm_ty = lm_ty;
      }
    } else if (op == "TD") {
      auto args = pop(2); // tx, ty
      if (args[0] && args[1]) {
        double dx = GetNum(*args[0]);
        double dy = GetNum(*args[1]);
        leading = -dy;
        lm_tx += dx;
        lm_ty += dy;
        tm_tx = lm_tx;
        tm_ty = lm_ty;
      }
    } else if (op == "Tm") {
      auto args = pop(6); // a, b, c, d, e, f
      if (args[4] && args[5]) {
        tm_tx = GetNum(*args[4]);
        tm_ty = GetNum(*args[5]);
        lm_tx = tm_tx;
        lm_ty = tm_ty;
      }
    } else if (op == "T*") {
      lm_ty -= leading;
      lm_tx = 0;
      tm_tx = lm_tx;
      tm_ty = lm_ty;
    } else if (op == "Tj") {
      auto args = pop(1);
      if (args[0] && args[0]->kind == Kind::kStr && !args[0]->s.empty()) {
        out.push_back({args[0]->s, tm_tx, tm_ty, fontSize});
      }
    } else if (op == "TJ") {
      auto args = pop(1);
      if (args[0] && args[0]->kind == Kind::kArray) {
        std::string combined;
        double xAdj = 0.0;
        for (const Object &elem : args[0]->arr) {
          if (elem.kind == Kind::kStr) {
            combined += elem.s;
          } else if (elem.kind == Kind::kInt || elem.kind == Kind::kReal) {
            xAdj += GetNum(elem);
          }
        }
        if (!combined.empty()) {
          out.push_back({combined, tm_tx, tm_ty, fontSize});
        }
        tm_tx -= xAdj * fontSize / 1000.0;
      }
    } else if (op == "'") {
      lm_ty -= leading;
      lm_tx = 0;
      tm_tx = lm_tx;
      tm_ty = lm_ty;
      auto args = pop(1);
      if (args[0] && args[0]->kind == Kind::kStr && !args[0]->s.empty()) {
        out.push_back({args[0]->s, tm_tx, tm_ty, fontSize});
      }
    } else if (op == "\"") {
      auto args = pop(3); // aw, ac, string
      lm_ty -= leading;
      lm_tx = 0;
      tm_tx = lm_tx;
      tm_ty = lm_ty;
      if (args[2] && args[2]->kind == Kind::kStr && !args[2]->s.empty()) {
        out.push_back({args[2]->s, tm_tx, tm_ty, fontSize});
      }
    } else {
      // Ignored non-text operators: pop their operands if we know the count
      // to keep stack reasonably clean, though Tj/TJ are robust anyway.
      if (op == "g" || op == "G" || op == "gs" || op == "w" || op == "cs" ||
          op == "CS") {
        pop(1);
      } else if (op == "rg" || op == "RG") {
        pop(3);
      } else if (op == "cm" || op == "re") {
        pop(6);
      }
    }
  }
  return out;
}

// -----------------------------------------------------------------------
// XMP metadata extraction (via libxml2)
// -----------------------------------------------------------------------

struct XmpMetadata {
  std::string title;
  std::string author;
  std::string subject;
  std::string creator;
  std::string producer;
  std::string creationDate;
  std::string modDate;
};

static XmpMetadata ExtractXmp(const std::vector<std::uint8_t> &streamData) {
  XmpMetadata meta;
  if (streamData.empty())
    return meta;

  // libxml2 expects null-terminated input; the stream data may contain
  // null bytes (unlikely for XMP, but guard against it).
  xmlDocPtr doc =
      xmlReadMemory(reinterpret_cast<const char *>(streamData.data()),
                    static_cast<int>(streamData.size()), nullptr, nullptr,
                    XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!doc)
    return meta;

  // XMP uses a flat structure with elements like:
  //   <dc:title><rdf:Alt><rdf:li>My Title</rdf:li></rdf:Alt></dc:title>
  // We search for common metadata elements by local name.
  xmlNodePtr root = xmlDocGetRootElement(doc);
  if (!root) {
    xmlFreeDoc(doc);
    return meta;
  }

  auto findChildText = [](xmlNodePtr parent,
                          const char *localName) -> std::string {
    if (!parent)
      return {};
    for (xmlNodePtr n = parent->children; n; n = n->next) {
      if (n->type != XML_ELEMENT_NODE)
        continue;
      const char *name = reinterpret_cast<const char *>(n->name);
      // Handle namespace-prefixed names (e.g. "dc:title")
      const char *colon = std::strrchr(name, ':');
      const char *local = colon ? colon + 1 : name;
      if (std::strcmp(local, localName) != 0)
        continue;
      // The text may be nested inside <rdf:Alt><rdf:li>...</rdf:li></rdf:Alt>
      for (xmlNodePtr c = n->children; c; c = c->next) {
        if (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE) {
          const char *content = reinterpret_cast<const char *>(c->content);
          if (content)
            return std::string(content);
        }
        // Drill into rdf:Alt / rdf:Bag / rdf:Seq wrappers
        for (xmlNodePtr li = c->children; li; li = li->next) {
          if (li->type == XML_TEXT_NODE || li->type == XML_CDATA_SECTION_NODE) {
            const char *content = reinterpret_cast<const char *>(li->content);
            if (content)
              return std::string(content);
          }
        }
      }
    }
    return {};
  };

  // Also search in x:xmpmeta wrapper
  xmlNodePtr xmp = root;
  if (root->ns && root->ns->prefix &&
      std::strcmp(reinterpret_cast<const char *>(root->ns->prefix),
                  "xmpmeta") == 0) {
    // root is <x:xmpmeta>, look for <rdf:RDF> inside
    for (xmlNodePtr c = root->children; c; c = c->next) {
      if (c->type == XML_ELEMENT_NODE) {
        xmp = c;
        break;
      }
    }
  }

  meta.title = findChildText(xmp, "title");
  meta.author = findChildText(xmp, "creator");
  meta.subject = findChildText(xmp, "description");
  meta.creator = findChildText(xmp, "CreatorTool");
  meta.producer = findChildText(xmp, "Producer");
  meta.creationDate = findChildText(xmp, "CreateDate");
  meta.modDate = findChildText(xmp, "ModifyDate");

  xmlFreeDoc(doc);
  return meta;
}

// -----------------------------------------------------------------------
// Build PDF object map and page list
// -----------------------------------------------------------------------

struct PdfData {
  std::map<Ref, Object> objs;
  std::vector<Ref> pageRefs;
  double defaultW = 612;
  double defaultH = 792;
};

static bool BuildPdf(const std::vector<std::uint8_t> &data, PdfData &pdf) {
  if (data.empty())
    return false;

  std::string text(reinterpret_cast<const char *>(data.data()), data.size());

  // Find startxref (informational; we don't use the offset directly)
  size_t sxPos = text.rfind("startxref");
  if (sxPos == std::string::npos) {
    DEBUG_LOG("[PDF] no startxref – trying without it");
  }

  // First pass: find all "n g obj" positions
  struct Entry {
    int num, gen;
    size_t pos;
  };
  std::vector<Entry> entries;

  // Scan for "obj" keywords and parse the preceding "N G" numbers backwards.
  // This avoids false matches where numbers in the version header or stream
  // data get consumed as part of a wrong "N G obj" triplet.
  auto isWS = [](unsigned char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
  };
  auto isDigit = [](unsigned char c) { return c >= '0' && c <= '9'; };

  for (size_t pos = 3; pos + 3 <= data.size(); ++pos) {
    if (std::memcmp(data.data() + pos, "obj", 3) != 0)
      continue;
    // Must be preceded by whitespace
    if (pos == 0 || !isWS(data[pos - 1]))
      continue;
    // Must be followed by whitespace or EOF
    if (pos + 3 < data.size() && !isWS(data[pos + 3]))
      continue;

    // Walk backwards from 'obj' to find "G" then "N"
    size_t p = pos;
    // Skip whitespace before 'obj'
    while (p > 0 && isWS(data[p - 1]))
      p--;
    if (p == 0)
      continue;

    // Parse generation number G (digits)
    size_t gEnd = p;
    while (p > 0 && isDigit(data[p - 1]))
      p--;
    if (p == gEnd || p == 0)
      continue;
    int gen = static_cast<int>(std::strtol(
        reinterpret_cast<const char *>(data.data() + p), nullptr, 10));

    // Skip whitespace between N and G
    while (p > 0 && isWS(data[p - 1]))
      p--;
    if (p == 0)
      continue;

    // Parse object number N (digits)
    size_t nEnd = p;
    while (p > 0 && isDigit(data[p - 1]))
      p--;
    if (p == nEnd)
      continue;

    // Validate: char before N must not be a digit (avoid matching part of a
    // longer number)
    if (p > 0 && isDigit(data[p - 1]))
      continue;

    int objNum = static_cast<int>(std::strtol(
        reinterpret_cast<const char *>(data.data() + p), nullptr, 10));
    if (objNum < 0)
      continue;

    entries.push_back({objNum, gen, pos + 3});
  }

  // Sort by object number for deterministic order
  std::sort(entries.begin(), entries.end(),
            [](const Entry &a, const Entry &b) { return a.num < b.num; });

  DEBUG_LOG("[PDF] found %zu objects", entries.size());
  for (auto &e : entries)
    DEBUG_LOG("[PDF]   obj %d %d at %zu", e.num, e.gen, e.pos);

  // Second pass: parse each object
  for (const Entry &e : entries) {
    Tokeniser tok(data.data(), data.size());
    tok.seek(e.pos);
    Object body = tok.read();
    size_t after = tok.tell();

    // Check for stream
    Tokeniser stok(data.data(), data.size());
    stok.seek(after);
    stok.skipSpace();
    if (stok.tell() + 6 <= data.size() &&
        std::memcmp(data.data() + stok.tell(), "stream", 6) == 0) {
      size_t streamEnd = stok.tell() + 6;
      bool streamDelim =
          (streamEnd >= data.size()) ||
          std::strchr(" \n\r\t", static_cast<char>(data[streamEnd])) != nullptr;
      if (streamDelim) {
        stok.seek(streamEnd);
        size_t sp = stok.tell();
        if (sp < data.size() && data[sp] == '\r')
          sp++;
        if (sp < data.size() && data[sp] == '\n')
          sp++;
        size_t sStart = sp;
        size_t sEnd = data.size();
        for (size_t s = sStart; s + 9 <= data.size(); ++s) {
          if (std::memcmp(data.data() + s, "endstream", 9) == 0) {
            sEnd = s;
            break;
          }
        }
        body.kind = Kind::kStream;
        body.stream.assign(data.data() + sStart, data.data() + sEnd);

        // Decompress FlateDecode streams (zlib/deflate)
        bool hasFlate = false;
        const Object *filter = DictFind(body, "Filter");
        if (filter) {
          if (filter->kind == Kind::kName && filter->s == "/FlateDecode")
            hasFlate = true;
          else if (filter->kind == Kind::kArray) {
            for (const Object &f : filter->arr)
              if (f.kind == Kind::kName && f.s == "/FlateDecode") {
                hasFlate = true;
                break;
              }
          }
        }
        if (hasFlate && !body.stream.empty()) {
          int outLen = 0;
          char *decoded = stbi_zlib_decode_malloc(
              reinterpret_cast<const char *>(body.stream.data()),
              static_cast<int>(body.stream.size()), &outLen);
          if (decoded && outLen > 0) {
            body.stream.assign(reinterpret_cast<std::uint8_t *>(decoded),
                               reinterpret_cast<std::uint8_t *>(decoded) +
                                   outLen);
            free(decoded);
            DEBUG_LOG("[PDF] obj %d %d: decompressed FlateDecode stream "
                      "successfully to %d bytes",
                      e.num, e.gen, outLen);
          } else {
            DEBUG_LOG("[PDF] obj %d %d: failed to decompress FlateDecode "
                      "stream of size %zu",
                      e.num, e.gen, body.stream.size());
          }
        }
      }
    }

    pdf.objs[{e.num, e.gen}] = std::move(body);
  }

  // Helper lambda: resolve Root→Pages→PageRefs from a trailer dict.
  auto resolveFromDict = [&](const Object &trailDict) -> bool {
    const Object *rootRef = DictFind(trailDict, "Root");
    if (!rootRef || rootRef->kind != Kind::kRef) {
      DEBUG_LOG("[PDF] no Root ref in trailer");
      return false;
    }
    DEBUG_LOG("[PDF] Root -> %d %d R", rootRef->ref.first, rootRef->ref.second);
    Object refObj = MakeRef(rootRef->ref);
    const Object *catalog = Resolve(pdf.objs, &refObj);
    if (!catalog) {
      DEBUG_LOG("[PDF] catalog not found for Root");
      return false;
    }
    DEBUG_LOG("[PDF] catalog has %zu entries", catalog->dict.size());

    const Object *pagesRef = DictFind(*catalog, "Pages");
    if (!pagesRef || pagesRef->kind != Kind::kRef) {
      DEBUG_LOG("[PDF] no Pages ref in catalog");
      return false;
    }
    refObj = MakeRef(pagesRef->ref);
    const Object *pagesNode = Resolve(pdf.objs, &refObj);
    if (!pagesNode) {
      DEBUG_LOG("[PDF] pages node not found");
      return false;
    }

    CollectPageRefs(pdf.objs, *pagesNode, pdf.pageRefs);
    DEBUG_LOG("[PDF] found %zu pages", pdf.pageRefs.size());

    const Object *mb = DictFind(*pagesNode, "MediaBox");
    if (mb && mb->kind == Kind::kArray && mb->arr.size() >= 4) {
      pdf.defaultW = GetNum(mb->arr[2]);
      pdf.defaultH = GetNum(mb->arr[3]);
    }
    return true;
  };

  // --- Strategy 1: traditional trailer keyword ---
  size_t trailerPos = text.rfind("trailer");
  if (trailerPos != std::string::npos) {
    DEBUG_LOG("[PDF] trailer at %zu", trailerPos);
    Tokeniser tTok(data.data(), data.size());
    tTok.seek(trailerPos + 7);
    while (!tTok.eof()) {
      Object o = tTok.read();
      if (o.kind == Kind::kDict) {
        DEBUG_LOG("[PDF] trailer dict has %zu entries", o.dict.size());
        if (resolveFromDict(o))
          return true;
        break;
      }
    }
  }

  // --- Strategy 2: cross-reference stream (modern PDFs) ---
  // The startxref offset may point to a cross-reference stream object whose
  // decoded stream contains the trailer dictionary as a series of key-value
  // pairs.  Some generators embed the trailer dict directly in the stream
  // object body.  Scan all objects for one with /Type /Catalog (the root
  // catalog) as a last resort.
  DEBUG_LOG("[PDF] trailer not found, scanning objects for catalog");
  for (auto &[ref, obj] : pdf.objs) {
    if (obj.kind != Kind::kDict)
      continue;
    const Object *type = DictFind(obj, "Type");
    if (!type || type->kind != Kind::kName || type->s != "/Catalog")
      continue;
    DEBUG_LOG("[PDF] found catalog at %d %d R", ref.first, ref.second);
    if (resolveFromDict(obj))
      return true;
  }

  DEBUG_LOG("[PDF] failed to resolve page tree");
  return false;
}

// =========================================================================
// Internal helper: render one page into a canvas given a pre-built PdfData
// =========================================================================

static bool RenderPageFromData(const PdfData &pdf, int pageNum, double scale,
                               PageBitmap &out) {
  if (pageNum < 0 || pageNum >= static_cast<int>(pdf.pageRefs.size()))
    return false;

  Ref pageRef = pdf.pageRefs[pageNum];
  Object refObj = MakeRef(pageRef);
  const Object *pageObj = Resolve(pdf.objs, &refObj);
  if (!pageObj)
    return false;

  // MediaBox: [llx lly urx ury]  — width = urx-llx, height = ury-lly
  double llx = 0, lly = 0, urx = pdf.defaultW, ury = pdf.defaultH;
  const Object *mb = DictFind(*pageObj, "MediaBox");
  if (mb && mb->kind == Kind::kArray && mb->arr.size() >= 4) {
    llx = GetNum(mb->arr[0]);
    lly = GetNum(mb->arr[1]);
    urx = GetNum(mb->arr[2]);
    ury = GetNum(mb->arr[3]);
  }
  double pageW = urx - llx;
  double pageH = ury - lly;
  if (pageW < 1)
    pageW = pdf.defaultW;
  if (pageH < 1)
    pageH = pdf.defaultH;

  // Auto-scale so no dimension exceeds kMaxDim at scale=1, then apply caller
  // scale on top.  We clamp renderW/H to avoid enormous allocations.
  const int kMaxDim = 1400;
  double autoScale = 1.0;
  if (pageW * scale > kMaxDim || pageH * scale > kMaxDim) {
    autoScale = std::min(static_cast<double>(kMaxDim) / (pageW * scale),
                         static_cast<double>(kMaxDim) / (pageH * scale));
  }
  double finalScale = scale * autoScale;

  int renderW = std::max(1, static_cast<int>(std::ceil(pageW * finalScale)));
  int renderH = std::max(1, static_cast<int>(std::ceil(pageH * finalScale)));

  // Collect content stream(s)
  std::vector<std::uint8_t> contentStream;
  const Object *contents = DictFind(*pageObj, "Contents");
  if (contents) {
    if (contents->kind == Kind::kRef) {
      Object r = MakeRef(contents->ref);
      const Object *resolved = Resolve(pdf.objs, &r);
      if (resolved && resolved->kind == Kind::kStream)
        contentStream = resolved->stream;
    } else if (contents->kind == Kind::kStream) {
      contentStream = contents->stream;
    } else if (contents->kind == Kind::kArray) {
      for (const Object &item : contents->arr) {
        if (item.kind == Kind::kRef) {
          Object r = MakeRef(item.ref);
          const Object *resolved = Resolve(pdf.objs, &r);
          if (resolved && resolved->kind == Kind::kStream) {
            const auto &s = resolved->stream;
            contentStream.insert(contentStream.end(), s.begin(), s.end());
            // Add a whitespace separator between concatenated streams
            contentStream.push_back(' ');
          }
        }
      }
    }
  }

  // Parse text spans from content stream
  std::vector<TextSpan> spans;
  DEBUG_LOG("[PDF] page %d: contentStream=%zu bytes, "
            "mediaBox=[%.1f,%.1f,%.1f,%.1f], canvas=%dx%d",
            pageNum, contentStream.size(), llx, lly, urx, ury, renderW,
            renderH);
  if (!contentStream.empty())
    spans = ParseText(contentStream);
  DEBUG_LOG("[PDF] page %d: parsed %zu text spans", pageNum, spans.size());
  if (!spans.empty()) {
    DEBUG_LOG("[PDF]   first span: '%s' tx=%.1f ty=%.1f fs=%.1f",
              spans[0].text.substr(0, 20).c_str(), spans[0].tx, spans[0].ty,
              spans[0].fontSize);
  }

  Paint::Canvas canvas(renderW, renderH);
  canvas.clear(Paint::Color{255, 255, 255, 255});

  for (const TextSpan &sp : spans) {
    if (sp.text.empty())
      continue;
    if (sp.fontSize <= 0)
      continue;

    // PDF coordinate system: origin bottom-left, Y increases upward.
    // Canvas coordinate system: origin top-left, Y increases downward.
    // Transform: canvas_y = (pageH - (sp.ty - lly)) * finalScale -
    // fontSize*finalScale
    double canvasX = (sp.tx - llx) * finalScale;
    double canvasY = (ury - sp.ty) * finalScale - sp.fontSize * finalScale;

    // Clamp to canvas bounds (with a small margin to not clip ascenders)
    if (canvasX < 0)
      canvasX = 0;
    if (canvasY < -sp.fontSize * finalScale)
      continue; // fully above top
    if (canvasY >= renderH)
      continue; // below bottom
    if (canvasX >= renderW)
      continue; // past right edge

    int fs =
        std::max(6, static_cast<int>(std::round(sp.fontSize * finalScale)));
    Font::drawText(canvas, static_cast<int>(std::round(canvasX)),
                   static_cast<int>(std::round(canvasY)), sp.text,
                   Paint::Color{0, 0, 0, 255}, fs);
  }

  out.width = renderW;
  out.height = renderH;
  out.pixels = canvas.pixels();
  return out.valid() || (renderW > 0 && renderH > 0);
}

// =========================================================================
// Public API
// =========================================================================

int pdfPageCount(const std::vector<std::uint8_t> &data) {
  PdfData pdf;
  if (!BuildPdf(data, pdf))
    return 0;
  return static_cast<int>(pdf.pageRefs.size());
}

bool renderPdfPage(const std::vector<std::uint8_t> &data, int pageNum,
                   PageBitmap &out) {
  PdfData pdf;
  if (!BuildPdf(data, pdf))
    return false;
  return RenderPageFromData(pdf, pageNum, 1.0, out);
}

bool renderPdfToBitmap(const std::vector<std::uint8_t> &data,
                       Image::Bitmap &out, int pageNum) {
  PageBitmap pb;
  if (!renderPdfPage(data, pageNum, pb))
    return false;
  out.width = pb.width;
  out.height = pb.height;
  out.pixels = std::move(pb.pixels);
  // Valid if pixel buffer has the right size; white blank pages are also valid.
  return out.width > 0 && out.height > 0 &&
         out.pixels.size() == static_cast<size_t>(out.width) * out.height;
}

bool renderAllPdfPages(const std::vector<std::uint8_t> &data,
                       Image::Bitmap &out, double scale) {
  PdfData pdf;
  if (!BuildPdf(data, pdf))
    return false;
  int nPages = static_cast<int>(pdf.pageRefs.size());
  if (nPages == 0)
    return false;

  // Render all pages and stack them vertically
  std::vector<PageBitmap> pages;
  pages.reserve(nPages);
  int totalH = 0;
  int maxW = 0;
  const int kPageGap = 8; // pixels between pages

  for (int p = 0; p < nPages; ++p) {
    PageBitmap pb;
    if (RenderPageFromData(pdf, p, scale, pb) && pb.width > 0 &&
        pb.height > 0) {
      totalH += pb.height;
      if (p + 1 < nPages)
        totalH += kPageGap;
      maxW = std::max(maxW, pb.width);
      pages.push_back(std::move(pb));
    }
  }

  if (pages.empty() || maxW <= 0 || totalH <= 0)
    return false;

  // Composite all pages into one tall bitmap, centred horizontally
  out.width = maxW;
  out.height = totalH;
  out.pixels.assign(static_cast<size_t>(maxW) * totalH,
                    Paint::Color{0xcc, 0xcc, 0xcc, 255}); // grey gap

  int yOff = 0;
  for (const PageBitmap &pb : pages) {
    int xOff = (maxW - pb.width) / 2; // centre narrower pages
    for (int py = 0; py < pb.height; ++py) {
      for (int px = 0; px < pb.width; ++px) {
        size_t srcIdx = static_cast<size_t>(py) * pb.width + px;
        size_t dstIdx = static_cast<size_t>(yOff + py) * maxW + xOff + px;
        out.pixels[dstIdx] = pb.pixels[srcIdx];
      }
    }
    yOff += pb.height + kPageGap;
  }

  return true;
}

bool pdfPageSizes(const std::vector<std::uint8_t> &data,
                  std::vector<std::pair<double, double>> &sizes) {
  PdfData pdf;
  if (!BuildPdf(data, pdf))
    return false;
  sizes.reserve(pdf.pageRefs.size());
  for (size_t i = 0; i < pdf.pageRefs.size(); ++i) {
    Ref pageRef = pdf.pageRefs[i];
    Object refObj = MakeRef(pageRef);
    const Object *pageObj = Resolve(pdf.objs, &refObj);
    double llx = 0, lly = 0, urx = pdf.defaultW, ury = pdf.defaultH;
    if (pageObj) {
      const Object *mb = DictFind(*pageObj, "MediaBox");
      if (mb && mb->kind == Kind::kArray && mb->arr.size() >= 4) {
        llx = GetNum(mb->arr[0]);
        lly = GetNum(mb->arr[1]);
        urx = GetNum(mb->arr[2]);
        ury = GetNum(mb->arr[3]);
      }
    }
    double w = urx - llx;
    double h = ury - lly;
    if (w < 1)
      w = pdf.defaultW;
    if (h < 1)
      h = pdf.defaultH;
    sizes.push_back({w, h});
  }
  return true;
}

PdfMetadata pdfMetadata(const std::vector<std::uint8_t> &data) {
  PdfMetadata meta;
  PdfData pdf;
  if (!BuildPdf(data, pdf))
    return meta;

  // Look for an XMP metadata stream object (typically has
  // /Type /Metadata /Subtype /XML).
  for (auto &[ref, obj] : pdf.objs) {
    if (obj.kind != Kind::kStream)
      continue;
    const Object *type = DictFind(obj, "Type");
    if (!type || type->kind != Kind::kName || type->s != "/Metadata")
      continue;
    const Object *subtype = DictFind(obj, "Subtype");
    if (!subtype || subtype->kind != Kind::kName || subtype->s != "/XML")
      continue;
    XmpMetadata xmp = ExtractXmp(obj.stream);
    meta.title = std::move(xmp.title);
    meta.author = std::move(xmp.author);
    meta.subject = std::move(xmp.subject);
    meta.creator = std::move(xmp.creator);
    meta.producer = std::move(xmp.producer);
    meta.creationDate = std::move(xmp.creationDate);
    meta.modDate = std::move(xmp.modDate);
    return meta;
  }

  return meta;
}

} // namespace Documents
} // namespace DesktopWebview
