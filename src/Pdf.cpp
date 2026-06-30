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
  if (obj.kind != Kind::kDict)
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

static std::vector<TextSpan>
ParseText(const std::vector<std::uint8_t> &stream) {
  struct Token {
    enum { Operand, Operator } type;
    Object obj;
    std::string op;
  };
  std::vector<Token> toks;

  // Known PDF content-stream operators
  auto isOp = [](const std::string &s) -> bool {
    static const char *ops[] = {"BT",  "ET",  "Tf",  "Td",  "TD",
                                "Tm",  "Tj",  "TJ",  "T*",  "TL",
                                "Tr",  "Ts",  "Tw",  "Tc",  "Tz",
                                "q",   "Q",   "cm",  "w",   "J",
                                "j",   "M",   "d",   "S",   "s",
                                "f",   "F",   "B",   "b",   "n",
                                "W",   "h",   "m",   "l",   "c",
                                "v",   "y",   "re",  "n",   "W",
                                "f*",  "B*",  "b*",  "Do",  "CS",
                                "cs",  "SC",  "sc",  "SCN", "scn",
                                "G",   "g",   "RG",  "rg",  "K",
                                "k",   "ri",  "i",   "gs",  "sh",
                                "MP",  "DP",  "BMC", "BDC", "EMC",
                                "Tl",  "0",   nullptr};
    for (int i = 0; ops[i]; ++i)
      if (s == ops[i])
        return true;
    return false;
  };

  Tokeniser ts(stream.data(), stream.size());
  while (!ts.eof()) {
    Object o = ts.read();
    if (o.kind == Kind::kNull)
      break;
    if (o.kind == Kind::kName) {
      std::string name = o.s;
      if (!name.empty() && name[0] == '/')
        name = name.substr(1);
      Token t;
      if (isOp(name)) {
        t.type = Token::Operator;
        t.op = std::move(name);
      } else {
        t.type = Token::Operand;
        t.obj = std::move(o);
      }
      toks.push_back(std::move(t));
    } else {
      Token t;
      t.type = Token::Operand;
      t.obj = std::move(o);
      toks.push_back(std::move(t));
    }
  }

  std::vector<TextSpan> out;
  double tx = 0, ty = 0, fontSize = 12, leading = 14;

  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].type != Token::Operator)
      continue;
    const std::string &op = toks[i].op;

    if (op == "BT") {
      tx = 0;
      ty = 0;
    } else if (op == "Tf") {
      if (i >= 1 && toks[i - 1].type == Token::Operand)
        fontSize = GetNum(toks[i - 1].obj);
    } else if (op == "Td" || op == "TD") {
      if (i >= 2) {
        tx += GetNum(toks[i - 2].obj);
        ty += GetNum(toks[i - 1].obj);
        if (op == "TD")
          leading = -GetNum(toks[i - 1].obj);
      }
    } else if (op == "Tj") {
      if (i >= 1 && toks[i - 1].type == Token::Operand) {
        const Object &o = toks[i - 1].obj;
        if (o.kind == Kind::kStr) {
          out.push_back({o.s, tx, ty, fontSize});
        }
      }
    } else if (op == "TJ") {
      if (i >= 1 && toks[i - 1].type == Token::Operand) {
        const Object &o = toks[i - 1].obj;
        if (o.kind == Kind::kArray) {
          std::string combined;
          for (const Object &e : o.arr)
            if (e.kind == Kind::kStr)
              combined += e.s;
          if (!combined.empty())
            out.push_back({combined, tx, ty, fontSize});
        }
      }
    } else if (op == "'") {
      tx = 0;
      ty -= leading;
      if (i >= 1 && toks[i - 1].type == Token::Operand) {
        const Object &o = toks[i - 1].obj;
        if (o.kind == Kind::kStr)
          out.push_back({o.s, tx, ty, fontSize});
      }
    } else if (op == "\"") {
      tx = 0;
      ty -= leading;
      if (i >= 3 && toks[i - 1].type == Token::Operand) {
        const Object &o = toks[i - 1].obj;
        if (o.kind == Kind::kStr)
          out.push_back({o.s, tx, ty, fontSize});
      }
    } else if (op == "Tm") {
      if (i >= 6) {
        tx = GetNum(toks[i - 2].obj);
        ty = GetNum(toks[i - 1].obj);
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
                               reinterpret_cast<std::uint8_t *>(decoded) + outLen);
            free(decoded);
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
  if (pageNum < 0 || pageNum >= static_cast<int>(pdf.pageRefs.size()))
    return false;

  Ref pageRef = pdf.pageRefs[pageNum];
  Object refObj = MakeRef(pageRef);
  const Object *pageObj = Resolve(pdf.objs, &refObj);
  if (!pageObj)
    return false;

  double pageW = pdf.defaultW;
  double pageH = pdf.defaultH;
  const Object *mb = DictFind(*pageObj, "MediaBox");
  if (mb && mb->kind == Kind::kArray && mb->arr.size() >= 4) {
    pageW = GetNum(mb->arr[2]);
    pageH = GetNum(mb->arr[3]);
  }
  if (pageW < 1 || pageH < 1) {
    pageW = 612;
    pageH = 792;
  }

  // Get content stream(s)
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
            auto &s = resolved->stream;
            contentStream.insert(contentStream.end(), s.begin(), s.end());
          }
        }
      }
    }
  }

  // Extract and render text
  std::vector<TextSpan> spans;
  if (!contentStream.empty())
    spans = ParseText(contentStream);

  int renderW = static_cast<int>(pageW);
  int renderH = static_cast<int>(pageH);
  const int kMaxDim = 1200;
  double scale = 1.0;
  if (renderW > kMaxDim || renderH > kMaxDim) {
    scale = std::min(static_cast<double>(kMaxDim) / renderW,
                     static_cast<double>(kMaxDim) / renderH);
    renderW = static_cast<int>(renderW * scale);
    renderH = static_cast<int>(renderH * scale);
  }

  Paint::Canvas canvas(renderW, renderH);
  canvas.clear(Paint::Color{255, 255, 255, 255});

  for (const TextSpan &sp : spans) {
    double x = sp.tx * scale;
    double y = (pageH - sp.ty) * scale - sp.fontSize * scale;
    if (y < 0)
      y = 0;
    if (y >= renderH)
      continue;

    int fs = std::max(8, static_cast<int>(sp.fontSize * scale));
    Font::drawText(canvas, static_cast<int>(x), static_cast<int>(y), sp.text,
                   Paint::Color{0, 0, 0, 255}, fs);
  }

  out.width = renderW;
  out.height = renderH;
  out.pixels = canvas.pixels();
  return true;
}

bool renderPdfToBitmap(const std::vector<std::uint8_t> &data,
                       Image::Bitmap &out, int pageNum) {
  PageBitmap pb;
  if (!renderPdfPage(data, pageNum, pb))
    return false;
  out.width = pb.width;
  out.height = pb.height;
  out.pixels = std::move(pb.pixels);
  return out.valid();
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
