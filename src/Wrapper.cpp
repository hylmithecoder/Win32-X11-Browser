#include "../include/Wrapper.hpp"
#include "../include/Net.hpp"

#include <iostream>
#include <utility>

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

namespace DesktopWebview {
namespace Wrapper {

namespace {

// Copy a libxml2 string into a std::string and release the libxml2 buffer.
// `owned` strings come from APIs like xmlGetProp/xmlNodeGetContent that hand
// ownership to the caller and must be freed with xmlFree.
std::string TakeXmlString(xmlChar *owned) {
  if (owned == nullptr) {
    return "";
  }
  std::string result(reinterpret_cast<const char *>(owned));
  xmlFree(owned);
  return result;
}

// View a borrowed libxml2 string (e.g. node->name) as a std::string without
// taking ownership.
std::string BorrowXmlString(const xmlChar *borrowed) {
  if (borrowed == nullptr) {
    return "";
  }
  return std::string(reinterpret_cast<const char *>(borrowed));
}

// Depth-first walk collecting every element whose name matches `tag`
// (case-insensitive). `start` and its following siblings are all searched,
// which lets the same routine serve both "this node's subtree" and "whole
// document" callers.
void CollectByTag(xmlNode *start, const xmlChar *tag, std::vector<Node> &out) {
  for (xmlNode *cur = start; cur != nullptr; cur = cur->next) {
    if (cur->type == XML_ELEMENT_NODE && xmlStrcasecmp(cur->name, tag) == 0) {
      out.emplace_back(cur);
    }
    if (cur->children != nullptr) {
      CollectByTag(cur->children, tag, out);
    }
  }
}

// Depth-first search for the first element carrying id="<id>".
xmlNode *FindById(xmlNode *start, const xmlChar *id) {
  for (xmlNode *cur = start; cur != nullptr; cur = cur->next) {
    if (cur->type == XML_ELEMENT_NODE) {
      xmlChar *value = xmlGetProp(cur, reinterpret_cast<const xmlChar *>("id"));
      if (value != nullptr) {
        bool match = xmlStrEqual(value, id) != 0;
        xmlFree(value);
        if (match) {
          return cur;
        }
      }
    }
    if (cur->children != nullptr) {
      if (xmlNode *found = FindById(cur->children, id)) {
        return found;
      }
    }
  }
  return nullptr;
}

void PrintSubtree(xmlNode *node, int depth) {
  for (xmlNode *cur = node; cur != nullptr; cur = cur->next) {
    if (cur->type != XML_ELEMENT_NODE) {
      continue;
    }
    for (int i = 0; i < depth; ++i) {
      std::cout << "  ";
    }
    std::cout << "<" << BorrowXmlString(cur->name);
    xmlChar *id = xmlGetProp(cur, reinterpret_cast<const xmlChar *>("id"));
    if (id != nullptr) {
      std::cout << " id=\"" << BorrowXmlString(id) << "\"";
      xmlFree(id);
    }
    std::cout << ">" << std::endl;
    PrintSubtree(cur->children, depth + 1);
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

bool Node::isElement() const {
  return m_node != nullptr && m_node->type == XML_ELEMENT_NODE;
}

bool Node::isText() const {
  return m_node != nullptr && (m_node->type == XML_TEXT_NODE ||
                               m_node->type == XML_CDATA_SECTION_NODE);
}

std::string Node::name() const {
  if (!isElement()) {
    return "";
  }
  return BorrowXmlString(m_node->name);
}

std::string Node::text() const {
  if (m_node == nullptr) {
    return "";
  }
  return TakeXmlString(xmlNodeGetContent(m_node));
}

std::string Node::attribute(const std::string &name) const {
  if (!isElement()) {
    return "";
  }
  return TakeXmlString(
      xmlGetProp(m_node, reinterpret_cast<const xmlChar *>(name.c_str())));
}

bool Node::hasAttribute(const std::string &name) const {
  if (!isElement()) {
    return false;
  }
  return xmlHasProp(m_node, reinterpret_cast<const xmlChar *>(name.c_str())) !=
         nullptr;
}

Node Node::parent() const {
  return Node(m_node != nullptr ? m_node->parent : nullptr);
}

Node Node::firstChild() const {
  return Node(m_node != nullptr ? m_node->children : nullptr);
}

Node Node::nextSibling() const {
  return Node(m_node != nullptr ? m_node->next : nullptr);
}

std::vector<Node> Node::children() const {
  std::vector<Node> result;
  if (m_node == nullptr) {
    return result;
  }
  for (xmlNode *cur = m_node->children; cur != nullptr; cur = cur->next) {
    if (cur->type == XML_ELEMENT_NODE) {
      result.emplace_back(cur);
    }
  }
  return result;
}

std::vector<Node> Node::getElementsByTagName(const std::string &tag) const {
  std::vector<Node> result;
  if (m_node != nullptr) {
    CollectByTag(m_node->children,
                 reinterpret_cast<const xmlChar *>(tag.c_str()), result);
  }
  return result;
}

Node Node::getElementById(const std::string &id) const {
  if (m_node == nullptr) {
    return Node();
  }
  return Node(FindById(m_node->children,
                       reinterpret_cast<const xmlChar *>(id.c_str())));
}

// ---------------------------------------------------------------------------
// HtmlDocument
// ---------------------------------------------------------------------------

HtmlDocument::~HtmlDocument() { destroy(); }

HtmlDocument::HtmlDocument(HtmlDocument &&other) noexcept
    : m_doc(std::exchange(other.m_doc, nullptr)) {}

HtmlDocument &HtmlDocument::operator=(HtmlDocument &&other) noexcept {
  if (this != &other) {
    destroy();
    m_doc = std::exchange(other.m_doc, nullptr);
  }
  return *this;
}

void HtmlDocument::destroy() {
  if (m_doc != nullptr) {
    xmlFreeDoc(reinterpret_cast<htmlDocPtr>(m_doc));
    m_doc = nullptr;
  }
}

bool HtmlDocument::parse(const std::string &html, const std::string &baseUrl) {
  destroy();
  if (html.empty()) {
    return false;
  }
  // Recover from malformed markup and stay quiet + offline, like a browser's
  // forgiving parser.
  const int options = HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                      HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
  htmlDocPtr doc = htmlReadMemory(
      html.c_str(), static_cast<int>(html.size()),
      baseUrl.empty() ? nullptr : baseUrl.c_str(), nullptr, options);
  m_doc = reinterpret_cast<_xmlDoc *>(doc);
  return m_doc != nullptr;
}

bool HtmlDocument::parseResponse(const std::string &httpResponse,
                                 const std::string &baseUrl) {
  return parse(Net::ExtractBody(httpResponse), baseUrl);
}

Node HtmlDocument::root() const {
  if (m_doc == nullptr) {
    return Node();
  }
  return Node(xmlDocGetRootElement(reinterpret_cast<htmlDocPtr>(m_doc)));
}

Node HtmlDocument::head() const {
  std::vector<Node> matches = getElementsByTagName("head");
  return matches.empty() ? Node() : matches.front();
}

Node HtmlDocument::body() const {
  std::vector<Node> matches = getElementsByTagName("body");
  return matches.empty() ? Node() : matches.front();
}

std::string HtmlDocument::title() const {
  std::vector<Node> matches = getElementsByTagName("title");
  return matches.empty() ? "" : matches.front().text();
}

std::vector<Node>
HtmlDocument::getElementsByTagName(const std::string &tag) const {
  std::vector<Node> result;
  Node r = root();
  if (!r.valid()) {
    return result;
  }
  // Include the root element itself, then its subtree.
  if (xmlStrcasecmp(r.raw()->name,
                    reinterpret_cast<const xmlChar *>(tag.c_str())) == 0) {
    result.emplace_back(r);
  }
  CollectByTag(r.raw()->children,
               reinterpret_cast<const xmlChar *>(tag.c_str()), result);
  return result;
}

Node HtmlDocument::getElementById(const std::string &id) const {
  Node r = root();
  if (!r.valid()) {
    return Node();
  }
  if (r.attribute("id") == id) {
    return r;
  }
  return r.getElementById(id);
}

std::vector<std::string> HtmlDocument::links() const {
  std::vector<std::string> hrefs;
  for (const Node &anchor : getElementsByTagName("a")) {
    if (anchor.hasAttribute("href")) {
      hrefs.push_back(anchor.attribute("href"));
    }
  }
  return hrefs;
}

void HtmlDocument::printTree() const {
  Node r = root();
  if (!r.valid()) {
    std::cout << "(empty document)" << std::endl;
    return;
  }
  PrintSubtree(r.raw(), 0);
}

} // namespace Wrapper
} // namespace DesktopWebview
