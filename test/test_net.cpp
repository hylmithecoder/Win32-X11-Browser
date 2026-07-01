#include "Debugger.hpp"
#include "Net.hpp"
#include <iostream>

using namespace DesktopWebview;

int main() {
  Net::Init();

  std::string url = "https://rule34.xxx//css/mobile.css?46";
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "Testing Custom HTTPS GET (via OpenSSL & Raw Sockets)"
            << std::endl;
  std::cout << "URL: " << url << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  std::string response = Net::Get(url);

  if (response.empty()) {
    std::cerr << "Error: Received empty response from host." << std::endl;
    Net::Cleanup();
    return 1;
  }

  // Parse response into Headers and Body
  size_t delimiter = response.find("\r\n\r\n");
  if (delimiter != std::string::npos) {
    std::string headers = response.substr(0, delimiter);
    std::string body = response.substr(delimiter + 4);

    std::cout << "\n--- RESPONSE HEADERS ---" << std::endl;
    std::cout << headers << std::endl;

    std::cout << "\n--- RESPONSE BODY ---" << std::endl;
    std::cout << body; // icanhazip.com body typically ends with a newline
  } else {
    std::cout << "\n--- RAW RESPONSE ---" << std::endl;
    std::cout << response << std::endl;
  }

  std::cout << "=========================================================="
            << std::endl;

  Net::Cleanup();
  return 0;
}
