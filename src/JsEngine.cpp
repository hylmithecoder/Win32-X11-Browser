#include "../include/JsEngine.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace DesktopWebview {
namespace Js {

// ---------------------------------------------------------------------------
// JsValue Implementation
// ---------------------------------------------------------------------------
JsValue::JsValue() = default;
JsValue::JsValue(ValueType t) : type(t) {
  if (t == ValueType::Object) {
    objVal = std::make_shared<JsObject>();
  }
}
JsValue::JsValue(double n) : type(ValueType::Number), numberVal(n) {}
JsValue::JsValue(const std::string &s)
    : type(ValueType::String), stringVal(s) {}
JsValue::JsValue(const char *s) : type(ValueType::String), stringVal(s) {}
JsValue::JsValue(bool b) : type(ValueType::Boolean), boolVal(b) {}
JsValue::JsValue(std::function<JsValue(const std::vector<JsValue> &)> cb)
    : type(ValueType::Object), callback(cb),
      objVal(std::make_shared<JsObject>()) {}

std::string JsValue::toString() const {
  if (type == ValueType::Number) {
    if (std::isnan(numberVal)) {
      return "NaN";
    }
    if (std::isinf(numberVal)) {
      return numberVal < 0 ? "-Infinity" : "Infinity";
    }
    std::string s = std::to_string(numberVal);
    if (s.find('.') != std::string::npos) {
      s.erase(s.find_last_not_of('0') + 1, std::string::npos);
      if (!s.empty() && s.back() == '.') {
        s.pop_back();
      }
    }
    return s;
  } else if (type == ValueType::String) {
    return stringVal;
  } else if (type == ValueType::Boolean) {
    return boolVal ? "true" : "false";
  } else if (type == ValueType::Object) {
    if (objVal && objVal->isArray) {
      std::string out;
      for (size_t i = 0; i < objVal->elements.size(); ++i) {
        if (i) {
          out += ",";
        }
        out += objVal->elements[i].toString();
      }
      return out;
    }
    if (callback) {
      return "function";
    }
    return "[object Object]";
  }
  return "undefined";
}

JsValue JsValue::getProperty(const std::string &name) const {
  if (type == ValueType::String && name == "length") {
    return JsValue(static_cast<double>(stringVal.size()));
  }
  if (objVal) {
    if (objVal->isArray && name == "length") {
      return JsValue(static_cast<double>(objVal->elements.size()));
    }
    auto it = objVal->properties.find(name);
    if (it != objVal->properties.end()) {
      return it->second;
    }
  }
  return JsValue();
}

void JsValue::setProperty(const std::string &name, const JsValue &val) {
  if (!objVal) {
    objVal = std::make_shared<JsObject>();
    type = ValueType::Object;
  }
  objVal->properties[name] = val;
}

// ---------------------------------------------------------------------------
// JsEnvironment Implementation
// ---------------------------------------------------------------------------
JsEnvironment::JsEnvironment(std::shared_ptr<JsEnvironment> p) : parent(p) {}

JsValue JsEnvironment::get(const std::string &name) {
  auto it = vars.find(name);
  if (it != vars.end()) {
    return it->second;
  }
  if (parent) {
    return parent->get(name);
  }
  return JsValue();
}

void JsEnvironment::set(const std::string &name, const JsValue &val) {
  if (vars.find(name) == vars.end() && parent) {
    auto p = parent;
    while (p) {
      auto pit = p->vars.find(name);
      if (pit != p->vars.end()) {
        pit->second = val;
        return;
      }
      p = p->parent;
    }
  }
  vars[name] = val;
}

// ---------------------------------------------------------------------------
// Lexer / Tokenizer
// ---------------------------------------------------------------------------
struct Token {
  enum class Type {
    Identifier,
    Number,
    String,
    KeywordVar,
    KeywordFunction,
    KeywordReturn,
    KeywordIf,
    KeywordElse,
    KeywordFor,
    KeywordWhile,
    KeywordTrue,
    KeywordFalse,
    KeywordNull,
    KeywordNew,
    KeywordTypeof,
    KeywordBreak,
    KeywordContinue,
    Assign,         // =
    PlusAssign,     // +=
    MinusAssign,    // -=
    MulAssign,      // *=
    DivAssign,      // /=
    Plus,           // +
    Minus,          // -
    Mul,            // *
    Div,            // /
    Mod,            // %
    Inc,            // ++
    Dec,            // --
    Dot,            // .
    LParen,         // (
    RParen,         // )
    LBrace,         // {
    RBrace,         // }
    LBracket,       // [
    RBracket,       // ]
    Semi,           // ;
    Comma,          // ,
    Colon,          // :
    Question,       // ?
    Equal,          // ==
    StrictEqual,    // ===
    NotEqual,       // !=
    StrictNotEqual, // !==
    Not,            // !
    Lt,             // <
    Gt,             // >
    Le,             // <=
    Ge,             // >=
    And,            // &&
    Or,             // ||
    Arrow,          // =>
    Eof,
    Unknown
  };
  Type type;
  std::string value;
};

static std::vector<Token> tokenize(const std::string &code) {
  std::vector<Token> tokens;
  size_t i = 0;
  auto two = [&](char a, char b) {
    return i + 1 < code.size() && code[i] == a && code[i + 1] == b;
  };
  auto three = [&](char a, char b, char c) {
    return i + 2 < code.size() && code[i] == a && code[i + 1] == b &&
           code[i + 2] == c;
  };
  while (i < code.size()) {
    char c = code[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
      continue;
    }
    // Single line comments
    if (two('/', '/')) {
      i += 2;
      while (i < code.size() && code[i] != '\n') {
        i++;
      }
      continue;
    }
    // Multi line comments
    if (two('/', '*')) {
      i += 2;
      while (i + 1 < code.size() && !(code[i] == '*' && code[i + 1] == '/')) {
        i++;
      }
      i += 2;
      continue;
    }
    // String literals (single, double, or template backtick)
    if (c == '"' || c == '\'' || c == '`') {
      char quote = c;
      std::string s;
      i++;
      while (i < code.size() && code[i] != quote) {
        if (code[i] == '\\' && i + 1 < code.size()) {
          i++;
          if (code[i] == 'n')
            s += '\n';
          else if (code[i] == 't')
            s += '\t';
          else if (code[i] == 'r')
            s += '\r';
          else
            s += code[i];
        } else {
          s += code[i];
        }
        i++;
      }
      i++; // eat closing quote
      tokens.push_back({Token::Type::String, s});
      continue;
    }
    // Numeric literals (incl. hex 0x.. and decimals)
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < code.size() &&
         std::isdigit(static_cast<unsigned char>(code[i + 1])))) {
      std::string n;
      if (two('0', 'x') || two('0', 'X')) {
        n += code[i++];
        n += code[i++];
        while (i < code.size() &&
               std::isxdigit(static_cast<unsigned char>(code[i]))) {
          n += code[i++];
        }
      } else {
        while (i < code.size() &&
               (std::isdigit(static_cast<unsigned char>(code[i])) ||
                code[i] == '.' || code[i] == 'e' || code[i] == 'E')) {
          n += code[i++];
        }
      }
      tokens.push_back({Token::Type::Number, n});
      continue;
    }
    // Identifiers and Keywords
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
      std::string id;
      while (i < code.size() &&
             (std::isalnum(static_cast<unsigned char>(code[i])) ||
              code[i] == '_' || code[i] == '$')) {
        id += code[i++];
      }
      Token::Type kw = Token::Type::Identifier;
      if (id == "var" || id == "let" || id == "const")
        kw = Token::Type::KeywordVar;
      else if (id == "function")
        kw = Token::Type::KeywordFunction;
      else if (id == "return")
        kw = Token::Type::KeywordReturn;
      else if (id == "if")
        kw = Token::Type::KeywordIf;
      else if (id == "else")
        kw = Token::Type::KeywordElse;
      else if (id == "for")
        kw = Token::Type::KeywordFor;
      else if (id == "while")
        kw = Token::Type::KeywordWhile;
      else if (id == "true")
        kw = Token::Type::KeywordTrue;
      else if (id == "false")
        kw = Token::Type::KeywordFalse;
      else if (id == "null" || id == "undefined")
        kw = Token::Type::KeywordNull;
      else if (id == "new")
        kw = Token::Type::KeywordNew;
      else if (id == "typeof")
        kw = Token::Type::KeywordTypeof;
      else if (id == "break")
        kw = Token::Type::KeywordBreak;
      else if (id == "continue")
        kw = Token::Type::KeywordContinue;
      tokens.push_back({kw, id});
      continue;
    }
    // Multi-character operators (longest match first)
    if (three('=', '=', '=')) {
      tokens.push_back({Token::Type::StrictEqual, "==="});
      i += 3;
      continue;
    }
    if (three('!', '=', '=')) {
      tokens.push_back({Token::Type::StrictNotEqual, "!=="});
      i += 3;
      continue;
    }
    if (two('=', '=')) {
      tokens.push_back({Token::Type::Equal, "=="});
      i += 2;
      continue;
    }
    if (two('=', '>')) {
      tokens.push_back({Token::Type::Arrow, "=>"});
      i += 2;
      continue;
    }
    if (two('!', '=')) {
      tokens.push_back({Token::Type::NotEqual, "!="});
      i += 2;
      continue;
    }
    if (two('<', '=')) {
      tokens.push_back({Token::Type::Le, "<="});
      i += 2;
      continue;
    }
    if (two('>', '=')) {
      tokens.push_back({Token::Type::Ge, ">="});
      i += 2;
      continue;
    }
    if (two('&', '&')) {
      tokens.push_back({Token::Type::And, "&&"});
      i += 2;
      continue;
    }
    if (two('|', '|')) {
      tokens.push_back({Token::Type::Or, "||"});
      i += 2;
      continue;
    }
    if (two('+', '+')) {
      tokens.push_back({Token::Type::Inc, "++"});
      i += 2;
      continue;
    }
    if (two('-', '-')) {
      tokens.push_back({Token::Type::Dec, "--"});
      i += 2;
      continue;
    }
    if (two('+', '=')) {
      tokens.push_back({Token::Type::PlusAssign, "+="});
      i += 2;
      continue;
    }
    if (two('-', '=')) {
      tokens.push_back({Token::Type::MinusAssign, "-="});
      i += 2;
      continue;
    }
    if (two('*', '=')) {
      tokens.push_back({Token::Type::MulAssign, "*="});
      i += 2;
      continue;
    }
    if (two('/', '=')) {
      tokens.push_back({Token::Type::DivAssign, "/="});
      i += 2;
      continue;
    }
    // Single-character tokens
    Token::Type single = Token::Type::Unknown;
    switch (c) {
    case '=':
      single = Token::Type::Assign;
      break;
    case '+':
      single = Token::Type::Plus;
      break;
    case '-':
      single = Token::Type::Minus;
      break;
    case '*':
      single = Token::Type::Mul;
      break;
    case '/':
      single = Token::Type::Div;
      break;
    case '%':
      single = Token::Type::Mod;
      break;
    case '.':
      single = Token::Type::Dot;
      break;
    case '(':
      single = Token::Type::LParen;
      break;
    case ')':
      single = Token::Type::RParen;
      break;
    case '{':
      single = Token::Type::LBrace;
      break;
    case '}':
      single = Token::Type::RBrace;
      break;
    case '[':
      single = Token::Type::LBracket;
      break;
    case ']':
      single = Token::Type::RBracket;
      break;
    case ';':
      single = Token::Type::Semi;
      break;
    case ',':
      single = Token::Type::Comma;
      break;
    case ':':
      single = Token::Type::Colon;
      break;
    case '?':
      single = Token::Type::Question;
      break;
    case '!':
      single = Token::Type::Not;
      break;
    case '<':
      single = Token::Type::Lt;
      break;
    case '>':
      single = Token::Type::Gt;
      break;
    default:
      single = Token::Type::Unknown;
      break;
    }
    tokens.push_back({single, std::string(1, c)});
    i++;
  }
  tokens.push_back({Token::Type::Eof, ""});
  return tokens;
}

// ---------------------------------------------------------------------------
// Control-flow signals (propagated as exceptions through the tree walker)
// ---------------------------------------------------------------------------
struct ReturnSignal {
  JsValue value;
};
struct BreakSignal {};
struct ContinueSignal {};

static bool truthy(const JsValue &v) {
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

static double toNum(const JsValue &v) {
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

static bool looseEquals(const JsValue &a, const JsValue &b) {
  if (a.type == ValueType::String && b.type == ValueType::String)
    return a.stringVal == b.stringVal;
  if (a.type == ValueType::Number || b.type == ValueType::Number)
    return toNum(a) == toNum(b);
  return a.toString() == b.toString();
}

static bool strictEquals(const JsValue &a, const JsValue &b) {
  if (a.type != b.type)
    return false;
  switch (a.type) {
  case ValueType::Number:
    return a.numberVal == b.numberVal;
  case ValueType::String:
    return a.stringVal == b.stringVal;
  case ValueType::Boolean:
    return a.boolVal == b.boolVal;
  case ValueType::Object:
    return a.objVal == b.objVal;
  default:
    return true; // both undefined
  }
}

// ---------------------------------------------------------------------------
// Parser and Interpreter Logic
// ---------------------------------------------------------------------------
class Parser {
  std::vector<Token> tokens;
  size_t index = 0;
  std::shared_ptr<JsEnvironment> env;
  DomInterface dom;

public:
  Parser(const std::vector<Token> &t, std::shared_ptr<JsEnvironment> e,
         DomInterface d)
      : tokens(t), env(e), dom(d) {}

  Token peek(size_t ahead = 0) {
    size_t k = index + ahead;
    if (k >= tokens.size()) {
      return {Token::Type::Eof, ""};
    }
    return tokens[k];
  }
  Token advance() {
    if (index >= tokens.size()) {
      return {Token::Type::Eof, ""};
    }
    return tokens[index++];
  }
  bool check(Token::Type t) { return peek().type == t; }
  bool match(Token::Type t) {
    if (peek().type == t) {
      advance();
      return true;
    }
    return false;
  }
  void consume(Token::Type t, const std::string &err) {
    if (!match(t)) {
      throw std::runtime_error(err);
    }
  }

  // Collect the tokens of a balanced { ... } block (current token must be the
  // opening brace). Returns the inner tokens with a trailing Eof. Consumes the
  // closing brace.
  std::vector<Token> collectBlock() {
    std::vector<Token> body;
    consume(Token::Type::LBrace, "Expected '{'");
    int depth = 1;
    while (depth > 0 && peek().type != Token::Type::Eof) {
      Token t = advance();
      if (t.type == Token::Type::LBrace)
        depth++;
      else if (t.type == Token::Type::RBrace) {
        depth--;
        if (depth == 0)
          break;
      }
      body.push_back(t);
    }
    body.push_back({Token::Type::Eof, ""});
    return body;
  }

  // Collect tokens up to a balanced closing of the current paren/bracket group
  // already opened by the caller. Stops before the matching closer; the closer
  // is left unconsumed.
  std::vector<Token> collectUntil(Token::Type closer) {
    std::vector<Token> out;
    int paren = 0, brace = 0, bracket = 0;
    while (peek().type != Token::Type::Eof) {
      Token::Type tt = peek().type;
      if (tt == closer && paren == 0 && brace == 0 && bracket == 0)
        break;
      if (tt == Token::Type::LParen)
        paren++;
      else if (tt == Token::Type::RParen) {
        if (paren == 0 && closer == Token::Type::RParen)
          break;
        paren--;
      } else if (tt == Token::Type::LBrace)
        brace++;
      else if (tt == Token::Type::RBrace)
        brace--;
      else if (tt == Token::Type::LBracket)
        bracket++;
      else if (tt == Token::Type::RBracket)
        bracket--;
      out.push_back(advance());
    }
    out.push_back({Token::Type::Eof, ""});
    return out;
  }

  // Build a callable JsValue from parameter names and a captured body. The
  // closure runs the body in a child environment when invoked.
  JsValue makeFunction(std::vector<std::string> params,
                       std::shared_ptr<std::vector<Token>> body,
                       std::shared_ptr<JsEnvironment> defEnv, DomInterface d) {
    return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
        [params, body, defEnv, d](const std::vector<JsValue> &args) -> JsValue {
          auto local = std::make_shared<JsEnvironment>(defEnv);
          for (size_t k = 0; k < params.size(); ++k) {
            local->set(params[k], k < args.size() ? args[k] : JsValue());
          }
          JsValue argsArr(ValueType::Object);
          argsArr.objVal->isArray = true;
          argsArr.objVal->elements = args;
          local->set("arguments", argsArr);
          Parser p(*body, local, d);
          try {
            p.parseProgram();
          } catch (ReturnSignal &r) {
            return r.value;
          }
          return JsValue();
        }));
  }

  std::vector<std::string> parseParamList() {
    std::vector<std::string> params;
    consume(Token::Type::LParen, "Expected '(' in parameter list");
    while (!check(Token::Type::RParen) && !check(Token::Type::Eof)) {
      if (check(Token::Type::Identifier)) {
        params.push_back(advance().value);
      } else {
        advance();
      }
      if (!match(Token::Type::Comma))
        break;
    }
    consume(Token::Type::RParen, "Expected ')' in parameter list");
    return params;
  }

  void parseProgram() {
    while (peek().type != Token::Type::Eof) {
      size_t before = index;
      try {
        parseStatement();
      } catch (ReturnSignal &) {
        throw;
      } catch (BreakSignal &) {
        throw;
      } catch (ContinueSignal &) {
        throw;
      } catch (const std::exception &) {
        // Error recovery: skip to the next statement boundary so a single bad
        // statement does not abort the whole script.
        recoverToBoundary();
      }
      if (index == before) {
        advance(); // guarantee forward progress
      }
    }
  }

  void recoverToBoundary() {
    int depth = 0;
    while (peek().type != Token::Type::Eof) {
      Token::Type t = peek().type;
      if (t == Token::Type::LBrace)
        depth++;
      else if (t == Token::Type::RBrace) {
        advance();
        if (depth == 0)
          return;
        depth--;
        continue;
      } else if (t == Token::Type::Semi && depth == 0) {
        advance();
        return;
      }
      advance();
    }
  }

  // Execute a captured token range as a (sub) program sharing `e` as scope.
  void runTokens(const std::vector<Token> &t,
                 std::shared_ptr<JsEnvironment> e) {
    Parser p(t, e, dom);
    p.parseProgram();
  }

  void parseStatement() {
    Token::Type t = peek().type;

    if (t == Token::Type::Semi) {
      advance();
      return;
    }
    if (t == Token::Type::LBrace) {
      std::vector<Token> block = collectBlock();
      runTokens(block, env);
      return;
    }
    if (t == Token::Type::KeywordBreak) {
      advance();
      match(Token::Type::Semi);
      throw BreakSignal{};
    }
    if (t == Token::Type::KeywordContinue) {
      advance();
      match(Token::Type::Semi);
      throw ContinueSignal{};
    }
    if (t == Token::Type::KeywordReturn) {
      advance();
      JsValue v;
      if (!check(Token::Type::Semi) && !check(Token::Type::RBrace) &&
          !check(Token::Type::Eof)) {
        v = parseExpression();
      }
      match(Token::Type::Semi);
      throw ReturnSignal{v};
    }
    if (t == Token::Type::KeywordFunction) {
      parseFunctionDeclaration();
      return;
    }
    if (t == Token::Type::KeywordIf) {
      parseIf();
      return;
    }
    if (t == Token::Type::KeywordWhile) {
      parseWhile();
      return;
    }
    if (t == Token::Type::KeywordFor) {
      parseFor();
      return;
    }
    if (t == Token::Type::KeywordVar) {
      parseVarDeclaration();
      return;
    }

    // Expression / assignment statement.
    parseExpressionStatement();
    match(Token::Type::Semi);
  }

  void parseVarDeclaration() {
    advance(); // var/let/const
    while (true) {
      Token name = advance();
      JsValue val;
      if (match(Token::Type::Assign)) {
        val = parseExpression();
      }
      env->set(name.value, val);
      if (!match(Token::Type::Comma))
        break;
    }
    match(Token::Type::Semi);
  }

  void parseFunctionDeclaration() {
    advance(); // function
    std::string name;
    if (check(Token::Type::Identifier)) {
      name = advance().value;
    }
    std::vector<std::string> params = parseParamList();
    auto body = std::make_shared<std::vector<Token>>(collectBlock());
    JsValue fn = makeFunction(params, body, env, dom);
    if (!name.empty()) {
      env->set(name, fn);
    }
  }

  void parseIf() {
    advance(); // if
    consume(Token::Type::LParen, "Expected '(' after if");
    JsValue cond = parseExpression();
    consume(Token::Type::RParen, "Expected ')' after condition");
    // then-branch
    std::vector<Token> thenTokens = captureStatementOrBlock();
    std::vector<Token> elseTokens;
    bool hasElse = false;
    if (match(Token::Type::KeywordElse)) {
      hasElse = true;
      elseTokens = captureStatementOrBlock();
    }
    if (truthy(cond)) {
      runTokens(thenTokens, env);
    } else if (hasElse) {
      runTokens(elseTokens, env);
    }
  }

  // Capture a single statement or a braced block as a token range (with Eof).
  std::vector<Token> captureStatementOrBlock() {
    if (check(Token::Type::LBrace)) {
      return collectBlock();
    }
    // single statement up to and including its terminating ';'
    std::vector<Token> out;
    int paren = 0, brace = 0, bracket = 0;
    while (peek().type != Token::Type::Eof) {
      Token::Type tt = peek().type;
      if (tt == Token::Type::Semi && paren == 0 && brace == 0 && bracket == 0) {
        out.push_back(advance());
        break;
      }
      if (tt == Token::Type::LParen)
        paren++;
      else if (tt == Token::Type::RParen)
        paren--;
      else if (tt == Token::Type::LBrace)
        brace++;
      else if (tt == Token::Type::RBrace)
        brace--;
      else if (tt == Token::Type::LBracket)
        bracket++;
      else if (tt == Token::Type::RBracket)
        bracket--;
      out.push_back(advance());
    }
    out.push_back({Token::Type::Eof, ""});
    return out;
  }

  void parseWhile() {
    advance(); // while
    consume(Token::Type::LParen, "Expected '(' after while");
    std::vector<Token> condTokens = collectUntil(Token::Type::RParen);
    consume(Token::Type::RParen, "Expected ')' after condition");
    std::vector<Token> body = captureStatementOrBlock();
    int guard = 0;
    while (guard++ < 1000000) {
      Parser cp(condTokens, env, dom);
      if (!truthy(cp.parseExpression()))
        break;
      try {
        runTokens(body, env);
      } catch (BreakSignal &) {
        break;
      } catch (ContinueSignal &) {
        continue;
      }
    }
  }

  void parseFor() {
    advance(); // for
    consume(Token::Type::LParen, "Expected '(' after for");
    // init
    std::vector<Token> initTokens = collectUntil(Token::Type::Semi);
    consume(Token::Type::Semi, "Expected ';' in for");
    std::vector<Token> condTokens = collectUntil(Token::Type::Semi);
    consume(Token::Type::Semi, "Expected ';' in for");
    std::vector<Token> updateTokens = collectUntil(Token::Type::RParen);
    consume(Token::Type::RParen, "Expected ')' after for clauses");
    std::vector<Token> body = captureStatementOrBlock();

    auto loopEnv = std::make_shared<JsEnvironment>(env);
    runTokens(initTokens, loopEnv);
    int guard = 0;
    while (guard++ < 1000000) {
      bool hasCond = condTokens.size() > 1; // more than just Eof
      if (hasCond) {
        Parser cp(condTokens, loopEnv, dom);
        if (!truthy(cp.parseExpression()))
          break;
      }
      try {
        runTokens(body, loopEnv);
      } catch (BreakSignal &) {
        break;
      } catch (ContinueSignal &) {
        // fall through to update
      }
      runTokens(updateTokens, loopEnv);
    }
  }

  // --- Expression statement: handle assignment vs bare expression ----------
  void parseExpressionStatement() {
    // Lookahead for an assignment operator at depth 0 before a ';'.
    size_t scan = index;
    int paren = 0, brace = 0, bracket = 0;
    Token::Type assignOp = Token::Type::Eof;
    size_t assignPos = 0;
    while (scan < tokens.size() && tokens[scan].type != Token::Type::Semi &&
           tokens[scan].type != Token::Type::Eof) {
      Token::Type tt = tokens[scan].type;
      if (tt == Token::Type::LParen)
        paren++;
      else if (tt == Token::Type::RParen)
        paren--;
      else if (tt == Token::Type::LBrace)
        brace++;
      else if (tt == Token::Type::RBrace)
        brace--;
      else if (tt == Token::Type::LBracket)
        bracket++;
      else if (tt == Token::Type::RBracket)
        bracket--;
      else if (paren == 0 && brace == 0 && bracket == 0 &&
               (tt == Token::Type::Assign || tt == Token::Type::PlusAssign ||
                tt == Token::Type::MinusAssign ||
                tt == Token::Type::MulAssign || tt == Token::Type::DivAssign)) {
        assignOp = tt;
        assignPos = scan;
        break;
      }
      scan++;
    }

    if (assignOp != Token::Type::Eof) {
      std::vector<Token> lhs(tokens.begin() + index,
                             tokens.begin() + assignPos);
      index = assignPos + 1; // skip the assignment operator
      JsValue rhs = parseExpression();
      executeAssignment(lhs, assignOp, rhs);
    } else {
      parseExpression();
    }
  }

  void executeAssignment(const std::vector<Token> &lhs, Token::Type op,
                         const JsValue &rhs) {
    if (lhs.empty())
      return;

    // Locate the last top-level '.' or '[' to split target from member.
    int lastDot = -1, lastBracket = -1;
    int paren = 0, bracket = 0;
    for (int i = 0; i < (int)lhs.size(); ++i) {
      Token::Type tt = lhs[i].type;
      if (tt == Token::Type::LParen)
        paren++;
      else if (tt == Token::Type::RParen)
        paren--;
      else if (tt == Token::Type::LBracket) {
        if (paren == 0 && bracket == 0)
          lastBracket = i;
        bracket++;
      } else if (tt == Token::Type::RBracket)
        bracket--;
      else if (tt == Token::Type::Dot && paren == 0 && bracket == 0)
        lastDot = i;
    }

    auto applyOp = [&](JsValue cur) -> JsValue {
      switch (op) {
      case Token::Type::PlusAssign:
        if (cur.type == ValueType::String || rhs.type == ValueType::String)
          return JsValue(cur.toString() + rhs.toString());
        return JsValue(toNum(cur) + toNum(rhs));
      case Token::Type::MinusAssign:
        return JsValue(toNum(cur) - toNum(rhs));
      case Token::Type::MulAssign:
        return JsValue(toNum(cur) * toNum(rhs));
      case Token::Type::DivAssign:
        return JsValue(toNum(cur) / toNum(rhs));
      default:
        return rhs;
      }
    };

    // Simple identifier target.
    if (lastDot == -1 && lastBracket == -1) {
      if (lhs.size() == 1 && lhs[0].type == Token::Type::Identifier) {
        JsValue cur =
            (op == Token::Type::Assign) ? JsValue() : env->get(lhs[0].value);
        env->set(lhs[0].value, applyOp(cur));
      }
      return;
    }

    if (lastBracket > lastDot) {
      // obj[expr] = rhs
      std::vector<Token> objTokens(lhs.begin(), lhs.begin() + lastBracket);
      // index tokens between the bracket and the matching close at end
      std::vector<Token> idxTokens(lhs.begin() + lastBracket + 1, lhs.end());
      if (!idxTokens.empty() && idxTokens.back().type == Token::Type::RBracket)
        idxTokens.pop_back();
      idxTokens.push_back({Token::Type::Eof, ""});
      objTokens.push_back({Token::Type::Eof, ""});

      Parser op1(objTokens, env, dom);
      JsValue obj = op1.parseExpression();
      Parser ip(idxTokens, env, dom);
      JsValue idx = ip.parseExpression();
      if (obj.objVal) {
        if (obj.objVal->isArray && idx.type == ValueType::Number) {
          size_t k = (size_t)idx.numberVal;
          if (k >= obj.objVal->elements.size())
            obj.objVal->elements.resize(k + 1);
          obj.objVal->elements[k] = applyOp(obj.objVal->elements[k]);
        } else {
          std::string key = idx.toString();
          obj.objVal->properties[key] = applyOp(obj.getProperty(key));
        }
      }
      return;
    }

    // obj.prop = rhs
    std::vector<Token> parentTokens(lhs.begin(), lhs.begin() + lastDot);
    parentTokens.push_back({Token::Type::Eof, ""});
    std::string propName =
        (lastDot + 1 < (int)lhs.size()) ? lhs[lastDot + 1].value : "";

    Parser parentParser(parentTokens, env, dom);
    JsValue parentObj = parentParser.parseExpression();

    JsValue newVal = applyOp(parentObj.getProperty(propName));

    // DOM interface integration
    if (parentObj.isDocument) {
      if (propName == "title" && dom.setTitle) {
        dom.setTitle(newVal.toString());
      }
    } else if (parentObj.isElement) {
      if ((propName == "innerText" || propName == "innerHTML" ||
           propName == "textContent") &&
          dom.setElementText) {
        dom.setElementText(parentObj.elementId, newVal.toString());
      }
    }
    parentObj.setProperty(propName, newVal);
  }

  // --- Expression precedence ----------------------------------------------
  JsValue parseExpression() { return parseTernary(); }

  JsValue parseTernary() {
    JsValue cond = parseLogicalOr();
    if (match(Token::Type::Question)) {
      JsValue a = parseExpression();
      consume(Token::Type::Colon, "Expected ':' in ternary");
      JsValue b = parseExpression();
      return truthy(cond) ? a : b;
    }
    return cond;
  }

  JsValue parseLogicalOr() {
    JsValue left = parseLogicalAnd();
    while (check(Token::Type::Or)) {
      advance();
      if (truthy(left)) {
        // short-circuit: still parse rhs to consume tokens
        parseLogicalAnd();
      } else {
        left = parseLogicalAnd();
      }
    }
    return left;
  }

  JsValue parseLogicalAnd() {
    JsValue left = parseEquality();
    while (check(Token::Type::And)) {
      advance();
      if (!truthy(left)) {
        parseEquality();
      } else {
        left = parseEquality();
      }
    }
    return left;
  }

  JsValue parseEquality() {
    JsValue left = parseComparison();
    while (check(Token::Type::Equal) || check(Token::Type::NotEqual) ||
           check(Token::Type::StrictEqual) ||
           check(Token::Type::StrictNotEqual)) {
      Token op = advance();
      JsValue right = parseComparison();
      switch (op.type) {
      case Token::Type::Equal:
        left = JsValue(looseEquals(left, right));
        break;
      case Token::Type::NotEqual:
        left = JsValue(!looseEquals(left, right));
        break;
      case Token::Type::StrictEqual:
        left = JsValue(strictEquals(left, right));
        break;
      default:
        left = JsValue(!strictEquals(left, right));
        break;
      }
    }
    return left;
  }

  JsValue parseComparison() {
    JsValue left = parseTerm();
    while (check(Token::Type::Lt) || check(Token::Type::Gt) ||
           check(Token::Type::Le) || check(Token::Type::Ge)) {
      Token op = advance();
      JsValue right = parseTerm();
      bool bothStr =
          left.type == ValueType::String && right.type == ValueType::String;
      bool res = false;
      if (bothStr) {
        switch (op.type) {
        case Token::Type::Lt:
          res = left.stringVal < right.stringVal;
          break;
        case Token::Type::Gt:
          res = left.stringVal > right.stringVal;
          break;
        case Token::Type::Le:
          res = left.stringVal <= right.stringVal;
          break;
        default:
          res = left.stringVal >= right.stringVal;
          break;
        }
      } else {
        double a = toNum(left), b = toNum(right);
        switch (op.type) {
        case Token::Type::Lt:
          res = a < b;
          break;
        case Token::Type::Gt:
          res = a > b;
          break;
        case Token::Type::Le:
          res = a <= b;
          break;
        default:
          res = a >= b;
          break;
        }
      }
      left = JsValue(res);
    }
    return left;
  }

  JsValue parseTerm() {
    JsValue left = parseFactor();
    while (check(Token::Type::Plus) || check(Token::Type::Minus)) {
      Token op = advance();
      JsValue right = parseFactor();
      if (op.type == Token::Type::Plus) {
        if (left.type == ValueType::String || right.type == ValueType::String) {
          left = JsValue(left.toString() + right.toString());
        } else {
          left = JsValue(toNum(left) + toNum(right));
        }
      } else {
        left = JsValue(toNum(left) - toNum(right));
      }
    }
    return left;
  }

  JsValue parseFactor() {
    JsValue left = parseUnary();
    while (check(Token::Type::Mul) || check(Token::Type::Div) ||
           check(Token::Type::Mod)) {
      Token op = advance();
      JsValue right = parseUnary();
      double a = toNum(left), b = toNum(right);
      if (op.type == Token::Type::Mul)
        left = JsValue(a * b);
      else if (op.type == Token::Type::Div)
        left = JsValue(a / b);
      else
        left = JsValue(std::fmod(a, b));
    }
    return left;
  }

  JsValue parseUnary() {
    if (check(Token::Type::Not)) {
      advance();
      return JsValue(!truthy(parseUnary()));
    }
    if (check(Token::Type::Minus)) {
      advance();
      return JsValue(-toNum(parseUnary()));
    }
    if (check(Token::Type::Plus)) {
      advance();
      return JsValue(toNum(parseUnary()));
    }
    if (check(Token::Type::KeywordTypeof)) {
      advance();
      JsValue v = parseUnary();
      switch (v.type) {
      case ValueType::Number:
        return JsValue(std::string("number"));
      case ValueType::String:
        return JsValue(std::string("string"));
      case ValueType::Boolean:
        return JsValue(std::string("boolean"));
      case ValueType::Object:
        return JsValue(std::string(v.callback ? "function" : "object"));
      default:
        return JsValue(std::string("undefined"));
      }
    }
    // prefix ++/-- on a bare identifier
    if (check(Token::Type::Inc) || check(Token::Type::Dec)) {
      Token op = advance();
      if (check(Token::Type::Identifier)) {
        std::string name = peek().value;
        double cur = toNum(env->get(name));
        cur += (op.type == Token::Type::Inc) ? 1 : -1;
        env->set(name, JsValue(cur));
        advance();
        return JsValue(cur);
      }
      return parseUnary();
    }
    return parsePostfix();
  }

  JsValue parsePostfix() {
    // postfix ++/-- on a bare identifier
    if (check(Token::Type::Identifier) && (peek(1).type == Token::Type::Inc ||
                                           peek(1).type == Token::Type::Dec)) {
      std::string name = advance().value;
      Token op = advance();
      double cur = toNum(env->get(name));
      double old = cur;
      cur += (op.type == Token::Type::Inc) ? 1 : -1;
      env->set(name, JsValue(cur));
      return JsValue(old);
    }
    return parseCallMember();
  }

  JsValue parseCallMember() {
    JsValue val = parsePrimary();
    JsValue thisObj; // receiver for method calls
    while (true) {
      if (match(Token::Type::Dot)) {
        Token member = advance();
        thisObj = val;
        val = resolveMember(val, member.value);
      } else if (match(Token::Type::LBracket)) {
        JsValue idx = parseExpression();
        consume(Token::Type::RBracket, "Expected ']'");
        thisObj = val;
        val = getIndex(val, idx);
      } else if (match(Token::Type::LParen)) {
        std::vector<JsValue> args;
        if (!check(Token::Type::RParen)) {
          do {
            args.push_back(parseExpression());
          } while (match(Token::Type::Comma));
        }
        consume(Token::Type::RParen, "Expected ')' after arguments");
        if (val.callback) {
          val = val.callback(args);
        } else {
          val = JsValue();
        }
        thisObj = JsValue();
      } else {
        break;
      }
    }
    return val;
  }

  // Member access with a few built-in methods on strings/arrays.
  JsValue resolveMember(const JsValue &obj, const std::string &name) {
    // String methods
    if (obj.type == ValueType::String) {
      std::string s = obj.stringVal;
      if (name == "length")
        return JsValue((double)s.size());
      if (name == "toUpperCase")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [s](const std::vector<JsValue> &) {
              std::string r = s;
              for (char &ch : r)
                ch = std::toupper((unsigned char)ch);
              return JsValue(r);
            }));
      if (name == "toLowerCase")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [s](const std::vector<JsValue> &) {
              std::string r = s;
              for (char &ch : r)
                ch = std::tolower((unsigned char)ch);
              return JsValue(r);
            }));
      if (name == "charAt")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [s](const std::vector<JsValue> &a) {
              size_t k = a.empty() ? 0 : (size_t)toNum(a[0]);
              return JsValue(k < s.size() ? std::string(1, s[k])
                                          : std::string());
            }));
      if (name == "indexOf")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [s](const std::vector<JsValue> &a) {
              if (a.empty())
                return JsValue(-1.0);
              auto p = s.find(a[0].toString());
              return JsValue(p == std::string::npos ? -1.0 : (double)p);
            }));
      if (name == "substring" || name == "substr" || name == "slice")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [s](const std::vector<JsValue> &a) {
              size_t start = a.size() > 0 ? (size_t)toNum(a[0]) : 0;
              if (start > s.size())
                start = s.size();
              size_t len = a.size() > 1 ? (size_t)toNum(a[1]) - start
                                        : std::string::npos;
              return JsValue(s.substr(start, len));
            }));
    }
    // Array methods
    if (obj.objVal && obj.objVal->isArray) {
      auto arr = obj.objVal;
      if (name == "length")
        return JsValue((double)arr->elements.size());
      if (name == "push")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [arr](const std::vector<JsValue> &a) {
              for (const auto &v : a)
                arr->elements.push_back(v);
              return JsValue((double)arr->elements.size());
            }));
      if (name == "join")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [arr](const std::vector<JsValue> &a) {
              std::string sep = a.empty() ? "," : a[0].toString();
              std::string out;
              for (size_t k = 0; k < arr->elements.size(); ++k) {
                if (k)
                  out += sep;
                out += arr->elements[k].toString();
              }
              return JsValue(out);
            }));
      if (name == "pop")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [arr](const std::vector<JsValue> &) {
              if (arr->elements.empty())
                return JsValue();
              JsValue v = arr->elements.back();
              arr->elements.pop_back();
              return v;
            }));
    }
    return obj.getProperty(name);
  }

  JsValue getIndex(const JsValue &obj, const JsValue &idx) {
    if (obj.objVal && obj.objVal->isArray && idx.type == ValueType::Number) {
      size_t k = (size_t)idx.numberVal;
      if (k < obj.objVal->elements.size())
        return obj.objVal->elements[k];
      return JsValue();
    }
    if (obj.type == ValueType::String && idx.type == ValueType::Number) {
      size_t k = (size_t)idx.numberVal;
      if (k < obj.stringVal.size())
        return JsValue(std::string(1, obj.stringVal[k]));
      return JsValue();
    }
    return obj.getProperty(idx.toString());
  }

  JsValue parsePrimary() {
    Token t = peek();
    if (match(Token::Type::Number)) {
      try {
        if (t.value.size() > 2 && (t.value[1] == 'x' || t.value[1] == 'X'))
          return JsValue((double)std::stoll(t.value, nullptr, 16));
        return JsValue(std::stod(t.value));
      } catch (...) {
        return JsValue(0.0);
      }
    }
    if (match(Token::Type::String)) {
      return JsValue(t.value);
    }
    if (match(Token::Type::KeywordTrue)) {
      return JsValue(true);
    }
    if (match(Token::Type::KeywordFalse)) {
      return JsValue(false);
    }
    if (match(Token::Type::KeywordNull)) {
      return JsValue();
    }
    if (match(Token::Type::KeywordNew)) {
      // `new X(...)` — evaluate as a call, return an object.
      JsValue v = parseCallMember();
      if (v.type == ValueType::Object)
        return v;
      return JsValue(ValueType::Object);
    }
    // Array literal
    if (match(Token::Type::LBracket)) {
      JsValue arr(ValueType::Object);
      arr.objVal->isArray = true;
      if (!check(Token::Type::RBracket)) {
        do {
          arr.objVal->elements.push_back(parseExpression());
        } while (match(Token::Type::Comma));
      }
      consume(Token::Type::RBracket, "Expected ']' in array");
      return arr;
    }
    // Object literal
    if (match(Token::Type::LBrace)) {
      JsValue obj(ValueType::Object);
      if (!check(Token::Type::RBrace)) {
        do {
          if (check(Token::Type::RBrace))
            break;
          Token key = advance();
          std::string keyName = key.value;
          consume(Token::Type::Colon, "Expected ':' in object literal");
          JsValue v = parseExpression();
          obj.setProperty(keyName, v);
        } while (match(Token::Type::Comma));
      }
      consume(Token::Type::RBrace, "Expected '}' in object literal");
      return obj;
    }
    // function expression
    if (match(Token::Type::KeywordFunction)) {
      if (check(Token::Type::Identifier))
        advance(); // optional name
      std::vector<std::string> params = parseParamList();
      auto body = std::make_shared<std::vector<Token>>(collectBlock());
      return makeFunction(params, body, env, dom);
    }
    // Arrow function: ident => ...   OR   ( ... ) => ...
    if (check(Token::Type::Identifier) && peek(1).type == Token::Type::Arrow) {
      std::vector<std::string> params{advance().value};
      advance(); // =>
      return parseArrowBody(params);
    }
    if (check(Token::Type::LParen) && isArrowAhead()) {
      std::vector<std::string> params = parseParamList();
      consume(Token::Type::Arrow, "Expected '=>'");
      return parseArrowBody(params);
    }
    if (match(Token::Type::Identifier)) {
      return env->get(t.value);
    }
    if (match(Token::Type::LParen)) {
      JsValue val = parseExpression();
      consume(Token::Type::RParen, "Expected ')'");
      return val;
    }
    // Unknown / unsupported token: consume and yield undefined.
    advance();
    return JsValue();
  }

  // Determine whether the '(' at the cursor begins an arrow-function parameter
  // list by scanning to its matching ')' and checking for a following '=>'.
  bool isArrowAhead() {
    int depth = 0;
    size_t k = index;
    while (k < tokens.size()) {
      Token::Type tt = tokens[k].type;
      if (tt == Token::Type::LParen)
        depth++;
      else if (tt == Token::Type::RParen) {
        depth--;
        if (depth == 0) {
          return k + 1 < tokens.size() &&
                 tokens[k + 1].type == Token::Type::Arrow;
        }
      } else if (tt == Token::Type::Eof) {
        return false;
      }
      k++;
    }
    return false;
  }

  JsValue parseArrowBody(std::vector<std::string> params) {
    if (check(Token::Type::LBrace)) {
      auto body = std::make_shared<std::vector<Token>>(collectBlock());
      return makeFunction(params, body, env, dom);
    }
    // Expression body: wrap as `return <expr>;`
    std::vector<Token> exprTokens = collectUntilExprEnd();
    auto body = std::make_shared<std::vector<Token>>();
    body->push_back({Token::Type::KeywordReturn, "return"});
    for (auto &tk : exprTokens)
      if (tk.type != Token::Type::Eof)
        body->push_back(tk);
    body->push_back({Token::Type::Semi, ";"});
    body->push_back({Token::Type::Eof, ""});
    return makeFunction(params, body, env, dom);
  }

  // Collect tokens for a single expression (used by arrow expression bodies),
  // stopping at a top-level comma, ')', ']', '}', ';' or Eof.
  std::vector<Token> collectUntilExprEnd() {
    std::vector<Token> out;
    int paren = 0, brace = 0, bracket = 0;
    while (peek().type != Token::Type::Eof) {
      Token::Type tt = peek().type;
      if ((tt == Token::Type::Comma || tt == Token::Type::RParen ||
           tt == Token::Type::RBracket || tt == Token::Type::RBrace ||
           tt == Token::Type::Semi) &&
          paren == 0 && brace == 0 && bracket == 0)
        break;
      if (tt == Token::Type::LParen)
        paren++;
      else if (tt == Token::Type::RParen)
        paren--;
      else if (tt == Token::Type::LBrace)
        brace++;
      else if (tt == Token::Type::RBrace)
        brace--;
      else if (tt == Token::Type::LBracket)
        bracket++;
      else if (tt == Token::Type::RBracket)
        bracket--;
      out.push_back(advance());
    }
    out.push_back({Token::Type::Eof, ""});
    return out;
  }
};

// ---------------------------------------------------------------------------
// JsEngine Implementation
// ---------------------------------------------------------------------------
JsEngine::JsEngine(DomInterface dom) : m_dom(dom) {
  m_globalEnv = std::make_shared<JsEnvironment>();

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
          [dom](const std::vector<JsValue> &args) {
            if (args.empty())
              return JsValue();
            std::string id = args[0].toString();
            JsValue el(ValueType::Object);
            el.isElement = true;
            el.elementId = id;
            if (dom.getElementText) {
              JsValue text(dom.getElementText(id));
              el.setProperty("innerText", text);
              el.setProperty("innerHTML", text);
              el.setProperty("textContent", text);
            }
            // no-op event binding so scripts using it don't break
            el.setProperty(
                "addEventListener",
                JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                    [](const std::vector<JsValue> &) { return JsValue(); })));
            return el;
          })));
  // querySelector / getElementsByTagName are best-effort no-ops returning an
  // element-ish object so scripts continue running.
  auto stubElement =
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [](const std::vector<JsValue> &) {
            JsValue el(ValueType::Object);
            el.isElement = true;
            el.setProperty(
                "addEventListener",
                JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                    [](const std::vector<JsValue> &) { return JsValue(); })));
            return el;
          }));
  documentObj.setProperty("querySelector", stubElement);
  documentObj.setProperty("createElement", stubElement);
  documentObj.setProperty(
      "addEventListener",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [](const std::vector<JsValue> &) { return JsValue(); })));
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
                     std::cout << "[JS alert] "
                               << (a.empty() ? "" : a[0].toString())
                               << std::endl;
                     return JsValue();
                   })));
  m_globalEnv->set("window", windowObj);
  m_globalEnv->set("alert", windowObj.getProperty("alert"));
  m_globalEnv->set(
      "setTimeout",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [](const std::vector<JsValue> &) { return JsValue(); })));
  m_globalEnv->set(
      "setInterval",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [](const std::vector<JsValue> &) { return JsValue(); })));
}

void JsEngine::execute(const std::string &code) {
  try {
    std::vector<Token> tokens = tokenize(code);
    Parser parser(tokens, m_globalEnv, m_dom);
    parser.parseProgram();
  } catch (const std::exception &e) {
    std::cerr << "JS execution error: " << e.what() << std::endl;
  } catch (...) {
    // swallow control-flow signals that escaped to top level
  }
}

} // namespace Js
} // namespace DesktopWebview
