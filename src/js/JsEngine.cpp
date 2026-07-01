#include "JsEngine.hpp"
#include "Debugger.hpp"
#include "Variabel.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace Debug;

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
    KeywordAsync,
    KeywordAwait,
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
      else if (id == "async")
        kw = Token::Type::KeywordAsync;
      else if (id == "await")
        kw = Token::Type::KeywordAwait;
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
// -- now defined in JsEngine.hpp
// ---------------------------------------------------------------------------

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
  JsEngine *engine; // Promise/timer access for `await` and async functions

public:
  Parser(const std::vector<Token> &t, std::shared_ptr<JsEnvironment> e,
         DomInterface d, JsEngine *eng)
      : tokens(t), env(e), dom(d), engine(eng) {}

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
  // closure runs the body in a child environment when invoked. When `isAsync`
  // is set, the function always returns a Promise instead of a raw value: the
  // body still runs synchronously (this interpreter has no coroutines to
  // truly suspend it), but a plain `return` fulfils the promise and a
  // RejectSignal escaping from an `await` inside the body (see doAwait)
  // rejects it, instead of either value unwinding further up the C++ stack.
  JsValue makeFunction(std::vector<std::string> params,
                       std::shared_ptr<std::vector<Token>> body,
                       std::shared_ptr<JsEnvironment> defEnv, DomInterface d,
                       JsEngine *eng, bool isAsync) {
    return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
        [params, body, defEnv, d, eng,
         isAsync](const std::vector<JsValue> &args) -> JsValue {
          auto local = std::make_shared<JsEnvironment>(defEnv);
          for (size_t k = 0; k < params.size(); ++k) {
            local->set(params[k], k < args.size() ? args[k] : JsValue());
          }
          JsValue argsArr(ValueType::Object);
          argsArr.objVal->isArray = true;
          argsArr.objVal->elements = args;
          local->set("arguments", argsArr);
          Parser p(*body, local, d, eng);
          if (!isAsync) {
            try {
              p.parseProgram();
            } catch (ReturnSignal &r) {
              return r.value;
            }
            return JsValue();
          }
          JsValue promise = eng->makePromise();
          try {
            p.parseProgram();
            eng->resolvePromiseObj(promise.objVal, JsValue()); // fell off the
                                                               // end: undefined
          } catch (ReturnSignal &r) {
            // If r.value is itself a promise/thenable, resolvePromiseObj
            // adopts its eventual state instead of nesting Promise<Promise>.
            eng->resolvePromiseObj(promise.objVal, r.value);
          } catch (RejectSignal &r) {
            eng->rejectPromiseObj(promise.objVal, r.value);
          }
          return promise;
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
      } catch (RejectSignal &) {
        // An awaited rejection aborts the rest of this (async) function body
        // rather than being treated as a recoverable per-statement error.
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
    Parser p(t, e, dom, engine);
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
    if (t == Token::Type::KeywordAsync &&
        peek(1).type == Token::Type::KeywordFunction) {
      advance(); // async
      parseFunctionDeclaration(true);
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

  void parseFunctionDeclaration(bool isAsync = false) {
    advance(); // function
    std::string name;
    if (check(Token::Type::Identifier)) {
      name = advance().value;
    }
    std::vector<std::string> params = parseParamList();
    auto body = std::make_shared<std::vector<Token>>(collectBlock());
    JsValue fn = makeFunction(params, body, env, dom, engine, isAsync);
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
      Parser cp(condTokens, env, dom, engine);
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
        Parser cp(condTokens, loopEnv, dom, engine);
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

      Parser op1(objTokens, env, dom, engine);
      JsValue obj = op1.parseExpression();
      Parser ip(idxTokens, env, dom, engine);
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

    Parser parentParser(parentTokens, env, dom, engine);
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
      if (propName == "value" && dom.setElementAttribute) {
        dom.setElementAttribute(parentObj.elementId, "value",
                                newVal.toString());
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

  // Evaluate `await v`. Per spec, awaiting a non-promise just yields the
  // value unchanged. Awaiting a promise fast-forwards the engine's virtual
  // clock -- draining microtasks and running due timers one step at a time
  // via advanceOneStep() -- until it settles; this interpreter has no
  // coroutines to truly suspend execution and resume later, so "waiting"
  // means synchronously running exactly the async work needed to reach a
  // result, nothing more. A rejection unwinds as a RejectSignal, caught by
  // the nearest async-function wrapper (see makeFunction) or, if none, by
  // JsEngine::execute's top-level catch-all.
  JsValue doAwait(JsValue v) {
    if (!(v.objVal && v.objVal->isPromise)) {
      return v;
    }
    int guard = 0;
    while (v.objVal->promiseState == 0 && guard++ < 1000000) {
      if (!engine->advanceOneStep()) {
        break; // nothing left could ever settle this promise
      }
    }
    if (v.objVal->promiseState == 2) {
      throw RejectSignal{v.objVal->promiseResult};
    }
    return v.objVal->promiseResult; // fulfilled, or still pending -> undefined
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
    if (check(Token::Type::KeywordAwait)) {
      advance();
      return doAwait(parseUnary());
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
    // DOM Element dynamic getters
    if (obj.isElement) {
      if (name == "value" && dom.getElementAttribute) {
        return JsValue(dom.getElementAttribute(obj.elementId, "value"));
      }
      if (name == "checked" && dom.getElementAttribute) {
        std::string checked = dom.getElementAttribute(obj.elementId, "checked");
        return JsValue(checked == "checked" || !checked.empty());
      }
      if ((name == "innerText" || name == "innerHTML" ||
           name == "textContent") &&
          dom.getElementText) {
        return JsValue(dom.getElementText(obj.elementId));
      }
    }
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
      if (name == "map")
        return JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
            [arr](const std::vector<JsValue> &a) {
              JsValue newArr(ValueType::Object);
              newArr.objVal->isArray = true;
              if (!a.empty() && a[0].callback) {
                for (const auto &item : arr->elements) {
                  newArr.objVal->elements.push_back(a[0].callback({item}));
                }
              }
              return newArr;
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
          bool isSpread = false;
          if (peek(0).type == Token::Type::Dot &&
              peek(1).type == Token::Type::Dot &&
              peek(2).type == Token::Type::Dot) {
            isSpread = true;
            advance();
            advance();
            advance();
          }
          JsValue val = parseExpression();
          if (isSpread) {
            if (val.type == ValueType::Object && val.objVal &&
                val.objVal->isArray) {
              for (const JsValue &item : val.objVal->elements) {
                arr.objVal->elements.push_back(item);
              }
            } else {
              arr.objVal->elements.push_back(val);
            }
          } else {
            arr.objVal->elements.push_back(val);
          }
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
      return makeFunction(params, body, env, dom, engine, false);
    }
    // async function(...){...}  |  async (params) => ...  |  async x => ...
    if (check(Token::Type::KeywordAsync)) {
      size_t save = index;
      advance(); // async
      if (match(Token::Type::KeywordFunction)) {
        if (check(Token::Type::Identifier))
          advance(); // optional name
        std::vector<std::string> params = parseParamList();
        auto body = std::make_shared<std::vector<Token>>(collectBlock());
        return makeFunction(params, body, env, dom, engine, true);
      }
      if (check(Token::Type::Identifier) &&
          peek(1).type == Token::Type::Arrow) {
        std::vector<std::string> params{advance().value};
        advance(); // =>
        return parseArrowBody(params, true);
      }
      if (check(Token::Type::LParen) && isArrowAhead()) {
        std::vector<std::string> params = parseParamList();
        consume(Token::Type::Arrow, "Expected '=>'");
        return parseArrowBody(params, true);
      }
      // Not actually `async function`/`async (...)=>` syntax (e.g. `async`
      // used as a plain identifier) -- back off and fall through as usual.
      index = save;
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

  JsValue parseArrowBody(std::vector<std::string> params,
                         bool isAsync = false) {
    if (check(Token::Type::LBrace)) {
      auto body = std::make_shared<std::vector<Token>>(collectBlock());
      return makeFunction(params, body, env, dom, engine, isAsync);
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
    return makeFunction(params, body, env, dom, engine, isAsync);
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
  initBuiltins();
}

// ---------------------------------------------------------------------------
// Async support: Promise primitives, timers, microtask/macrotask pumping
// ---------------------------------------------------------------------------

JsValue JsEngine::makePromise() {
  JsValue p(ValueType::Object);
  p.objVal->isPromise = true;
  auto obj = p.objVal;
  JsEngine *eng = this;
  p.setProperty(
      "then", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                  [obj, eng](const std::vector<JsValue> &args) {
                    JsValue onFulfilled = args.size() > 0 ? args[0] : JsValue();
                    JsValue onRejected = args.size() > 1 ? args[1] : JsValue();
                    return eng->promiseThen(obj, onFulfilled, onRejected);
                  })));
  p.setProperty("catch",
                JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                    [obj, eng](const std::vector<JsValue> &args) {
                      JsValue onRejected = args.empty() ? JsValue() : args[0];
                      return eng->promiseThen(obj, JsValue(), onRejected);
                    })));
  p.setProperty(
      "finally",
      JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
          [obj, eng](const std::vector<JsValue> &args) {
            JsValue onFinally = args.empty() ? JsValue() : args[0];
            // Run onFinally regardless of outcome, then pass the original
            // value through (or re-reject with the original reason).
            JsValue passFulfill(
                std::function<JsValue(const std::vector<JsValue> &)>(
                    [onFinally](const std::vector<JsValue> &a) {
                      if (onFinally.callback) {
                        onFinally.callback({});
                      }
                      return a.empty() ? JsValue() : a[0];
                    }));
            JsValue passReject(
                std::function<JsValue(const std::vector<JsValue> &)>(
                    [onFinally](const std::vector<JsValue> &a) -> JsValue {
                      if (onFinally.callback) {
                        onFinally.callback({});
                      }
                      throw RejectSignal{a.empty() ? JsValue() : a[0]};
                    }));
            return eng->promiseThen(obj, passFulfill, passReject);
          })));
  return p;
}

void JsEngine::resolvePromiseObj(std::shared_ptr<JsObject> obj, JsValue value) {
  if (!obj || obj->promiseState != 0) {
    return; // already settled (or resolved more than once): ignore
  }
  // Resolving with another promise/thenable adopts its eventual state instead
  // of nesting Promise<Promise<T>>.
  if (value.objVal && value.objVal->isPromise) {
    auto inner = value.objVal;
    JsEngine *eng = this;
    auto adopt = [inner, obj, eng]() {
      if (inner->promiseState == 1) {
        eng->resolvePromiseObj(obj, inner->promiseResult);
      } else {
        eng->rejectPromiseObj(obj, inner->promiseResult);
      }
    };
    if (inner->promiseState != 0) {
      enqueueMicrotask(adopt);
    } else {
      inner->onSettled.push_back(adopt);
    }
    return;
  }
  obj->promiseState = 1;
  obj->promiseResult = value;
  std::vector<std::function<void()>> callbacks;
  callbacks.swap(obj->onSettled);
  for (auto &cb : callbacks) {
    enqueueMicrotask(cb);
  }
}

void JsEngine::rejectPromiseObj(std::shared_ptr<JsObject> obj, JsValue reason) {
  if (!obj || obj->promiseState != 0) {
    return;
  }
  obj->promiseState = 2;
  obj->promiseResult = reason;
  std::vector<std::function<void()>> callbacks;
  callbacks.swap(obj->onSettled);
  for (auto &cb : callbacks) {
    enqueueMicrotask(cb);
  }
}

JsValue JsEngine::promiseThen(std::shared_ptr<JsObject> obj,
                              JsValue onFulfilled, JsValue onRejected) {
  JsValue derived = makePromise();
  auto derivedObj = derived.objVal;
  JsEngine *eng = this;
  auto settle = [obj, onFulfilled, onRejected, derivedObj, eng]() {
    bool fulfilled = obj->promiseState == 1;
    JsValue handler = fulfilled ? onFulfilled : onRejected;
    JsValue result = obj->promiseResult;
    if (handler.callback) {
      try {
        JsValue r = handler.callback({result});
        eng->resolvePromiseObj(derivedObj, r);
      } catch (RejectSignal &rs) {
        eng->rejectPromiseObj(derivedObj, rs.value);
      }
    } else if (fulfilled) {
      eng->resolvePromiseObj(derivedObj, result);
    } else {
      // No rejection handler: propagate the rejection to the derived
      // promise unchanged (as real Promise chains do).
      eng->rejectPromiseObj(derivedObj, result);
    }
  };
  if (obj->promiseState != 0) {
    enqueueMicrotask(settle);
  } else {
    obj->onSettled.push_back(settle);
  }
  return derived;
}

void JsEngine::enqueueMicrotask(std::function<void()> task) {
  m_microtasks.push_back(std::move(task));
}

void JsEngine::drainMicrotasks() {
  // A microtask can enqueue more microtasks (chained .then()); drain until
  // empty, per spec ordering (all microtasks run before the next macrotask).
  while (!m_microtasks.empty()) {
    std::function<void()> task = m_microtasks.front();
    m_microtasks.erase(m_microtasks.begin());
    task();
  }
}

void JsEngine::pump(double nowMs) {
  m_virtualNowMs = std::max(m_virtualNowMs, nowMs);
  // Run every timer due by now, earliest first; interval timers are
  // rescheduled rather than removed. A callback that itself schedules a
  // due-by-now timer (e.g. setTimeout(fn, 0)) is picked up in the same pump.
  bool ranOne = true;
  while (ranOne) {
    ranOne = false;
    size_t earliest = m_timers.size();
    double earliestDue = 0;
    for (size_t i = 0; i < m_timers.size(); ++i) {
      if (m_timers[i].cancelled || m_timers[i].dueMs > m_virtualNowMs) {
        continue;
      }
      if (earliest == m_timers.size() || m_timers[i].dueMs < earliestDue) {
        earliest = i;
        earliestDue = m_timers[i].dueMs;
      }
    }
    if (earliest == m_timers.size()) {
      break;
    }
    Timer &t = m_timers[earliest];
    if (t.repeating) {
      t.dueMs += t.intervalMs > 0 ? t.intervalMs : 1.0;
    } else {
      t.cancelled = true; // one-shot: consumed
    }
    if (t.callback.callback) {
      t.callback.callback(t.args);
    }
    drainMicrotasks();
    ranOne = true;
  }
  // Garbage-collect consumed one-shot/cancelled timers so a long-lived page
  // doesn't grow this vector without bound.
  m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                [](const Timer &t) { return t.cancelled; }),
                 m_timers.end());
}

bool JsEngine::advanceOneStep() {
  if (!m_microtasks.empty()) {
    std::function<void()> task = m_microtasks.front();
    m_microtasks.erase(m_microtasks.begin());
    task();
    return true;
  }
  size_t earliest = m_timers.size();
  double earliestDue = 0;
  for (size_t i = 0; i < m_timers.size(); ++i) {
    if (m_timers[i].cancelled) {
      continue;
    }
    if (earliest == m_timers.size() || m_timers[i].dueMs < earliestDue) {
      earliest = i;
      earliestDue = m_timers[i].dueMs;
    }
  }
  if (earliest == m_timers.size()) {
    return false; // nothing left that could ever settle the awaited promise
  }
  Timer &t = m_timers[earliest];
  m_virtualNowMs = std::max(m_virtualNowMs, t.dueMs); // jump the clock to it
  if (t.repeating) {
    t.dueMs += t.intervalMs > 0 ? t.intervalMs : 1.0;
  } else {
    t.cancelled = true;
  }
  if (t.callback.callback) {
    t.callback.callback(t.args);
  }
  return true;
}

void JsEngine::execute(const std::string &code) {
  try {
    std::vector<Token> tokens = tokenize(code);
    Parser parser(tokens, m_globalEnv, m_dom, this);
    parser.parseProgram();
    // Let any Promise chains kicked off synchronously during this script run
    // as far as they can (their .then() reactions) before yielding, matching
    // the JS event loop draining microtasks after every task.
    drainMicrotasks();
  } catch (const std::exception &e) {
    std::cerr << "JS execution error: " << e.what() << std::endl;
  } catch (...) {
    // swallow control-flow signals that escaped to top level (including an
    // unhandled RejectSignal from a top-level `await` on a rejected promise)
  }
}

JsValue JsEngine::getOrElement(const std::string &id) {
  if (id.empty()) {
    return JsValue();
  }
  auto it = m_elementCache.find(id);
  if (it != m_elementCache.end()) {
    return it->second;
  }

  JsValue el(ValueType::Object);
  el.isElement = true;
  el.elementId = id;

  el.setProperty("addEventListener",
                 JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                     [elObj = el.objVal](const std::vector<JsValue> &args) {
                       if (args.size() >= 2) {
                         std::string eventType = args[0].toString();
                         JsValue callback = args[1];
                         if (elObj) {
                           elObj->properties["_listener_" + eventType] =
                               callback;
                         }
                       }
                       return JsValue();
                     })));

  m_elementCache[id] = el;
  return el;
}

void JsEngine::triggerEvent(const std::string &eventType,
                            const std::string &elementId) {
  JsValue el = getOrElement(elementId);
  if (el.type == ValueType::Object && el.objVal) {
    auto it = el.objVal->properties.find("_listener_" + eventType);
    if (it != el.objVal->properties.end() && it->second.callback) {
      JsValue eventObj(ValueType::Object);
      eventObj.setProperty(
          "preventDefault",
          JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
              [](const std::vector<JsValue> &) { return JsValue(); })));
      it->second.callback({eventObj});
    }
  }
}

} // namespace Js
} // namespace DesktopWebview
