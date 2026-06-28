#ifndef WRAPPER_HPP
#define WRAPPER_HPP

#include <string>
#include <vector>

// Forward-declare the libxml2 node type so this header does not drag the whole
// libxml2 API into every translation unit that only needs the wrapper.
extern "C" {
struct _xmlNode;
struct _xmlDoc;
}

namespace DesktopWebview {
namespace Wrapper {

// A lightweight, non-owning view over a single node in a parsed HTML document.
//
// A Node is only valid for as long as the HtmlDocument it came from is alive;
// it holds a raw pointer into that document's tree and copying a Node copies
// the pointer, not the underlying node.
class Node {
public:
  Node() = default;
  explicit Node(_xmlNode *node) : m_node(node) {}

  // True when this Node points at a real tree node.
  bool valid() const { return m_node != nullptr; }
  explicit operator bool() const { return valid(); }

  // True for element nodes (e.g. <div>) vs. text/comment/other nodes.
  bool isElement() const;
  // True for text nodes (the character data between tags).
  bool isText() const;

  // The tag name for elements (lower-cased by libxml2, e.g. "div"), or an
  // empty string for non-elements.
  std::string name() const;

  // The concatenated text content of this node and all of its descendants,
  // with markup stripped (libxml2's xmlNodeGetContent).
  std::string text() const;

  // Set the text content of this node (libxml2's xmlNodeSetContent).
  void setText(const std::string &content);

  // The value of an attribute (e.g. attribute("href")), or an empty string
  // when the attribute is absent.
  std::string attribute(const std::string &name) const;
  bool hasAttribute(const std::string &name) const;

  // Tree navigation. Each returns an invalid Node when there is nothing there.
  Node parent() const;
  Node firstChild() const;
  Node nextSibling() const;

  // Direct element children of this node (text/comment nodes are skipped).
  std::vector<Node> children() const;

  // All descendant elements whose tag matches `tag` (case-insensitive),
  // in document order. Searches the whole subtree, not just direct children.
  std::vector<Node> getElementsByTagName(const std::string &tag) const;

  // The first descendant element with the given id attribute, or an invalid
  // Node if none exists.
  Node getElementById(const std::string &id) const;

  // Underlying libxml2 node, for callers that need the raw API.
  _xmlNode *raw() const { return m_node; }

private:
  _xmlNode *m_node = nullptr;
};

// An owning, RAII wrapper around a parsed HTML document.
//
// HtmlDocument uses libxml2's lenient HTML parser, so malformed real-world
// markup is recovered rather than rejected. The document owns the underlying
// tree and frees it on destruction; Nodes handed out by this document must not
// outlive it. The type is movable but not copyable.
class HtmlDocument {
public:
  HtmlDocument() = default;
  ~HtmlDocument();

  HtmlDocument(const HtmlDocument &) = delete;
  HtmlDocument &operator=(const HtmlDocument &) = delete;
  HtmlDocument(HtmlDocument &&other) noexcept;
  HtmlDocument &operator=(HtmlDocument &&other) noexcept;

  // Parse an HTML string. `html` may be a full document or a fragment;
  // `baseUrl` is used by libxml2 only for diagnostics and relative-URL context.
  // Replaces any previously parsed document. Returns false on failure.
  bool parse(const std::string &html, const std::string &baseUrl = "");

  // Convenience: strip the HTTP status line/headers off a raw Net response
  // (via Net::ExtractBody) and parse the remaining body as HTML.
  bool parseResponse(const std::string &httpResponse,
                     const std::string &baseUrl = "");

  bool valid() const { return m_doc != nullptr; }

  // The document's root element, normally <html>. Invalid if not parsed.
  Node root() const;
  // The <head> element, or an invalid Node if absent.
  Node head() const;
  // The <body> element, or an invalid Node if absent.
  Node body() const;

  // The text of the document's <title>, or an empty string if there is none.
  std::string title() const;

  // Document-wide element queries (see Node's equivalents).
  std::vector<Node> getElementsByTagName(const std::string &tag) const;
  Node getElementById(const std::string &id) const;

  // Collect the href of every <a> element in document order.
  std::vector<std::string> links() const;

  // Print an indented outline of the element tree to stdout (debugging aid).
  void printTree() const;

private:
  void destroy();

  _xmlDoc *m_doc = nullptr;
};

} // namespace Wrapper
} // namespace DesktopWebview

#endif // WRAPPER_HPP
