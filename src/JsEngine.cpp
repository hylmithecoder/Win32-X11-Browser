#include "../include/JsEngine.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
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
    std::string s = std::to_string(numberVal);
    s.erase(s.find_last_not_of('0') + 1, std::string::npos);
    if (!s.empty() && s.back() == '.') {
      s.pop_back();
    }
    return s;
  } else if (type == ValueType::String) {
    return stringVal;
  } else if (type == ValueType::Boolean) {
    return boolVal ? "true" : "false";
  } else if (type == ValueType::Object) {
    return "[object Object]";
  }
  return "undefined";
}

JsValue JsValue::getProperty(const std::string &name) const {
  if (objVal) {
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
    Assign, // =
    Plus,   // +
    Minus,  // -
    Mul,    // *
    Div,    // /
    Dot,    // .
    LParen, // (
    RParen, // )
    LBrace, // {
    RBrace, // }
    Semi,   // ;
    Comma,  // ,
    Equal,  // ==
    Eof
  };
  Type type;
  std::string value;
};

static std::vector<Token> tokenize(const std::string &code) {
  std::vector<Token> tokens;
  size_t i = 0;
  while (i < code.size()) {
    char c = code[i];
    if (std::isspace(c)) {
      i++;
      continue;
    }
    // Single line comments
    if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
      i += 2;
      while (i < code.size() && code[i] != '\n') {
        i++;
      }
      continue;
    }
    // Multi line comments
    if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
      i += 2;
      while (i + 1 < code.size() && !(code[i] == '*' && code[i + 1] == '/')) {
        i++;
      }
      i += 2;
      continue;
    }
    // String literals
    if (c == '"' || c == '\'') {
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
    // Numeric literals
    if (std::isdigit(c)) {
      std::string n;
      while (i < code.size() && (std::isdigit(code[i]) || code[i] == '.')) {
        n += code[i];
        i++;
      }
      tokens.push_back({Token::Type::Number, n});
      continue;
    }
    // Identifiers and Keywords
    if (std::isalpha(c) || c == '_' || c == '$') {
      std::string id;
      while (i < code.size() &&
             (std::isalnum(code[i]) || code[i] == '_' || code[i] == '$')) {
        id += code[i];
        i++;
      }
      if (id == "var" || id == "let" || id == "const") {
        tokens.push_back({Token::Type::KeywordVar, id});
      } else if (id == "function") {
        tokens.push_back({Token::Type::KeywordFunction, id});
      } else if (id == "return") {
        tokens.push_back({Token::Type::KeywordReturn, id});
      } else if (id == "if") {
        tokens.push_back({Token::Type::KeywordIf, id});
      } else if (id == "else") {
        tokens.push_back({Token::Type::KeywordElse, id});
      } else {
        tokens.push_back({Token::Type::Identifier, id});
      }
      continue;
    }
    // Operators
    if (c == '=') {
      if (i + 1 < code.size() && code[i + 1] == '=') {
        tokens.push_back({Token::Type::Equal, "=="});
        i += 2;
      } else {
        tokens.push_back({Token::Type::Assign, "="});
        i++;
      }
      continue;
    }
    if (c == '+') {
      tokens.push_back({Token::Type::Plus, "+"});
      i++;
      continue;
    }
    if (c == '-') {
      tokens.push_back({Token::Type::Minus, "-"});
      i++;
      continue;
    }
    if (c == '*') {
      tokens.push_back({Token::Type::Mul, "*"});
      i++;
      continue;
    }
    if (c == '/') {
      tokens.push_back({Token::Type::Div, "/"});
      i++;
      continue;
    }
    if (c == '.') {
      tokens.push_back({Token::Type::Dot, "."});
      i++;
      continue;
    }
    if (c == '(') {
      tokens.push_back({Token::Type::LParen, "("});
      i++;
      continue;
    }
    if (c == ')') {
      tokens.push_back({Token::Type::RParen, ")"});
      i++;
      continue;
    }
    if (c == '{') {
      tokens.push_back({Token::Type::LBrace, "{"});
      i++;
      continue;
    }
    if (c == '}') {
      tokens.push_back({Token::Type::RBrace, "}"});
      i++;
      continue;
    }
    if (c == ';') {
      tokens.push_back({Token::Type::Semi, ";"});
      i++;
      continue;
    }
    if (c == ',') {
      tokens.push_back({Token::Type::Comma, ","});
      i++;
      continue;
    }
    i++; // Skip unknown characters
  }
  tokens.push_back({Token::Type::Eof, ""});
  return tokens;
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

  Token peek() {
    if (index >= tokens.size()) {
      return {Token::Type::Eof, ""};
    }
    return tokens[index];
  }
  Token advance() {
    if (index >= tokens.size()) {
      return {Token::Type::Eof, ""};
    }
    return tokens[index++];
  }
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

  void parseProgram() {
    while (peek().type != Token::Type::Eof) {
      parseStatement();
    }
  }

  void parseStatement() {
    if (match(Token::Type::KeywordVar)) {
      Token name = advance();
      JsValue val;
      if (match(Token::Type::Assign)) {
        val = parseExpression();
      }
      env->set(name.value, val);
      match(Token::Type::Semi);
      return;
    }

    // Check lookahead for assignment `=`
    size_t eqIndex = index;
    int parenDepth = 0;
    int braceDepth = 0;
    bool foundAssign = false;
    while (eqIndex < tokens.size() &&
           tokens[eqIndex].type != Token::Type::Semi &&
           tokens[eqIndex].type != Token::Type::Eof) {
      if (tokens[eqIndex].type == Token::Type::LParen)
        parenDepth++;
      else if (tokens[eqIndex].type == Token::Type::RParen)
        parenDepth--;
      else if (tokens[eqIndex].type == Token::Type::LBrace)
        braceDepth++;
      else if (tokens[eqIndex].type == Token::Type::RBrace)
        braceDepth--;
      else if (tokens[eqIndex].type == Token::Type::Assign && parenDepth == 0 &&
               braceDepth == 0) {
        foundAssign = true;
        break;
      }
      eqIndex++;
    }

    if (foundAssign) {
      std::vector<Token> lhs(tokens.begin() + index, tokens.begin() + eqIndex);
      index = eqIndex + 1; // skip '='
      JsValue rhs = parseExpression();
      match(Token::Type::Semi);
      executeAssignment(lhs, rhs);
    } else {
      parseExpression();
      match(Token::Type::Semi);
    }
  }

  void executeAssignment(const std::vector<Token> &lhs, const JsValue &rhs) {
    if (lhs.empty())
      return;

    int lastDotIndex = -1;
    int parenDepth = 0;
    for (int i = 0; i < (int)lhs.size(); ++i) {
      if (lhs[i].type == Token::Type::LParen)
        parenDepth++;
      else if (lhs[i].type == Token::Type::RParen)
        parenDepth--;
      else if (lhs[i].type == Token::Type::Dot && parenDepth == 0) {
        lastDotIndex = i;
      }
    }

    if (lastDotIndex == -1) {
      if (lhs.size() == 1 && lhs[0].type == Token::Type::Identifier) {
        env->set(lhs[0].value, rhs);
      }
    } else {
      std::vector<Token> parentTokens(lhs.begin(), lhs.begin() + lastDotIndex);
      std::string propName = lhs[lastDotIndex + 1].value;

      Parser parentParser(parentTokens, env, dom);
      JsValue parentObj = parentParser.parseExpression();

      // DOM interface integration
      if (parentObj.isDocument) {
        if (propName == "title") {
          if (dom.setTitle) {
            dom.setTitle(rhs.toString());
          }
        }
      } else if (parentObj.isElement) {
        if (propName == "innerText" || propName == "innerHTML") {
          if (dom.setElementText) {
            dom.setElementText(parentObj.elementId, rhs.toString());
          }
        }
      }

      parentObj.setProperty(propName, rhs);
    }
  }

  JsValue parseExpression() { return parseEquality(); }

  JsValue parseEquality() {
    JsValue left = parseTerm();
    while (peek().type == Token::Type::Equal) {
      advance();
      JsValue right = parseTerm();
      left = JsValue(left.toString() == right.toString());
    }
    return left;
  }

  JsValue parseTerm() {
    JsValue left = parseFactor();
    while (peek().type == Token::Type::Plus ||
           peek().type == Token::Type::Minus) {
      Token op = advance();
      JsValue right = parseFactor();
      if (op.type == Token::Type::Plus) {
        if (left.type == ValueType::String || right.type == ValueType::String) {
          left = JsValue(left.toString() + right.toString());
        } else {
          left = JsValue(left.numberVal + right.numberVal);
        }
      } else {
        left = JsValue(left.numberVal - right.numberVal);
      }
    }
    return left;
  }

  JsValue parseFactor() {
    JsValue left = parsePrimary();
    while (peek().type == Token::Type::Mul || peek().type == Token::Type::Div) {
      Token op = advance();
      JsValue right = parsePrimary();
      if (op.type == Token::Type::Mul) {
        left = JsValue(left.numberVal * right.numberVal);
      } else {
        left = JsValue(right.numberVal != 0 ? left.numberVal / right.numberVal
                                            : 0);
      }
    }
    return left;
  }

  JsValue parsePrimary() {
    Token t = peek();
    if (match(Token::Type::Number)) {
      return JsValue(std::stod(t.value));
    }
    if (match(Token::Type::String)) {
      return JsValue(t.value);
    }
    if (match(Token::Type::Identifier)) {
      JsValue val = env->get(t.value);

      while (true) {
        if (match(Token::Type::Dot)) {
          Token member = advance();
          val = val.getProperty(member.value);
        } else if (match(Token::Type::LParen)) {
          std::vector<JsValue> args;
          if (peek().type != Token::Type::RParen) {
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
        } else {
          break;
        }
      }
      return val;
    }
    if (match(Token::Type::LParen)) {
      JsValue val = parseExpression();
      consume(Token::Type::RParen, "Expected ')'");
      return val;
    }
    advance();
    return JsValue();
  }
};

// ---------------------------------------------------------------------------
// JsEngine Implementation
// ---------------------------------------------------------------------------
JsEngine::JsEngine(DomInterface dom) : m_dom(dom) {
  m_globalEnv = std::make_shared<JsEnvironment>();

  // Setup console.log
  JsValue consoleObj(ValueType::Object);
  consoleObj.setProperty(
      "log", JsValue(std::function<JsValue(const std::vector<JsValue> &)>(
                 [](const std::vector<JsValue> &args) {
                   for (size_t i = 0; i < args.size(); ++i) {
                     std::cout << args[i].toString()
                               << (i + 1 < args.size() ? " " : "");
                   }
                   std::cout << std::endl;
                   return JsValue();
                 })));
  m_globalEnv->set("console", consoleObj);

  // Setup document
  JsValue documentObj(ValueType::Object);
  documentObj.isDocument = true;

  // Bind getElementById
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
              el.setProperty("innerText", JsValue(dom.getElementText(id)));
            }
            return el;
          })));

  m_globalEnv->set("document", documentObj);
}

void JsEngine::execute(const std::string &code) {
  try {
    std::vector<Token> tokens = tokenize(code);
    Parser parser(tokens, m_globalEnv, m_dom);
    parser.parseProgram();
  } catch (const std::exception &e) {
    std::cerr << "JS execution error: " << e.what() << std::endl;
  }
}

} // namespace Js
} // namespace DesktopWebview
