#include "Net.hpp"
#include "Wrapper.hpp"
#include <iostream>

using namespace DesktopWebview;

static int g_failures = 0;

// Minimal check helper: prints PASS/FAIL and tracks failures for the exit code.
static void Check(const std::string &label, bool condition) {
  if (condition) {
    std::cout << "  [PASS] " << label << std::endl;
  } else {
    std::cout << "  [FAIL] " << label << std::endl;
    ++g_failures;
  }
}

static void OfflineParsingTests() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "Offline HTML parsing (libxml2 wrapper)" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  // Deliberately messy markup: missing </li>, unquoted-ish attributes, mixed
  // case tags. libxml2's HTML parser should recover from all of it.
  const std::string html =
      "<!DOCTYPE html><html><head><title>Hello Wrapper</title></head>"
      "<body><h1 id=\"main\">Heading</h1>"
      "<UL><li>one<li>two<li>three</UL>"
      "<a href=\"https://example.com\">Example</a>"
      "<a href=\"/about\">About</a>"
      "</body></html>";

  Wrapper::HtmlDocument doc;
  Check("parse() succeeds", doc.parse(html));
  Check("document is valid", doc.valid());

  Check("title() == 'Hello Wrapper'", doc.title() == "Hello Wrapper");

  Wrapper::Node body = doc.body();
  Check("body() found", body.valid());
  Check("body tag name is 'body'", body.name() == "body");

  std::vector<Wrapper::Node> items = doc.getElementsByTagName("li");
  Check("found 3 <li> elements", items.size() == 3);
  if (items.size() == 3) {
    Check("first <li> text == 'one'", items[0].text() == "one");
    Check("third <li> text == 'three'", items[2].text() == "three");
  }

  Wrapper::Node heading = doc.getElementById("main");
  Check("getElementById('main') found", heading.valid());
  Check("#main is an <h1>", heading.valid() && heading.name() == "h1");
  Check("#main text == 'Heading'",
        heading.valid() && heading.text() == "Heading");

  std::vector<std::string> links = doc.links();
  Check("collected 2 links", links.size() == 2);
  if (links.size() == 2) {
    Check("first link href correct", links[0] == "https://example.com");
    Check("second link href correct", links[1] == "/about");
  }

  std::cout << "\n--- ELEMENT TREE ---" << std::endl;
  doc.printTree();
}

static void LiveFetchTest() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Live fetch + parse (Net::Get -> Wrapper)" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  const std::string url = "https://portofolio.ilmeee.com/";
  std::cout << "URL: " << url << std::endl;

  std::string response = Net::Get(url);
  if (response.empty()) {
    std::cout << "  [SKIP] No response (offline?). Skipping live test."
              << std::endl;
    return;
  }

  Wrapper::HtmlDocument doc;
  if (!doc.parseResponse(response, url)) {
    std::cout << "  [SKIP] Could not parse response body." << std::endl;
    return;
  }

  std::cout << "  Title: " << doc.title() << std::endl;
  std::cout << "  Link count: " << doc.links().size() << std::endl;
  Check("live document has a non-empty title", !doc.title().empty());
}

int main() {
  Net::Init();

  OfflineParsingTests();
  LiveFetchTest();

  Net::Cleanup();

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All wrapper tests passed." << std::endl;
  } else {
    std::cout << g_failures << " wrapper test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;

  return g_failures == 0 ? 0 : 1;
}
