#include "../include/Net.hpp"
#include "../include/Debugger.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET SocketType;
#define CLOSE_SOCKET(s) closesocket(s)
#define INVALID_SOCKET_VAL INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SocketType;
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET_VAL -1
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <random>
#include <sstream>
#include <vector>

namespace DesktopWebview {
namespace Net {

void Init() {
#if defined(_WIN32)
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "WSAStartup failed." << std::endl;
  }
#endif
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();
}

void Cleanup() {
#if defined(_WIN32)
  WSACleanup();
#endif
  EVP_cleanup();
  ERR_free_strings();
}

namespace {

// Build a DNS query packet for an A record lookup of the given hostname.
// Returns the raw packet bytes suitable for sending over UDP.
std::vector<uint8_t> BuildDnsQuery(const std::string &hostname) {
  std::vector<uint8_t> packet;

  // DNS Header (12 bytes)
  uint16_t id = 0;
  { // random transaction ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(0);
    id = dist(gen);
  }
  auto write16 = [&](uint16_t v) {
    packet.push_back(static_cast<uint8_t>(v >> 8));
    packet.push_back(static_cast<uint8_t>(v & 0xFF));
  };

  write16(id);     // Transaction ID
  write16(0x0100); // Flags: standard query, recursion desired
  write16(1);      // QDCOUNT: 1 question
  write16(0);      // ANCOUNT: 0
  write16(0);      // NSCOUNT: 0
  write16(0);      // ARCOUNT: 0

  // Question: QNAME (encoded hostname)
  size_t start = 0;
  for (size_t i = 0; i <= hostname.size(); ++i) {
    if (i == hostname.size() || hostname[i] == '.') {
      uint8_t len = static_cast<uint8_t>(i - start);
      packet.push_back(len);
      for (size_t j = start; j < i; ++j) {
        packet.push_back(static_cast<uint8_t>(hostname[j]));
      }
      start = i + 1;
    }
  }
  packet.push_back(0); // terminating zero-length label

  write16(1); // QTYPE: A record
  write16(1); // QCLASS: IN (Internet)

  return packet;
}

// Parse a DNS response and return the first A-record IP as a string
// (dotted decimal), or empty on failure.
std::string ParseDnsResponse(const std::vector<uint8_t> &response) {
  if (response.size() < 12)
    return "";

  // Check flags: response code must be 0
  uint16_t flags = static_cast<uint16_t>(response[2] << 8) | response[3];
  if ((flags & 0x000F) != 0)
    return "";

  uint16_t qdcount = static_cast<uint16_t>(response[4] << 8) | response[5];
  uint16_t ancount = static_cast<uint16_t>(response[6] << 8) | response[7];

  if (ancount == 0)
    return "";

  size_t pos = 12;

  // Skip the question section
  for (uint16_t q = 0; q < qdcount; ++q) {
    while (pos < response.size()) {
      uint8_t len = response[pos];
      if (len == 0) {
        ++pos;
        break;
      }
      if ((len & 0xC0) == 0xC0) {
        pos += 2;
        break;
      } // compressed label
      pos += len + 1;
    }
    pos += 4; // skip QTYPE + QCLASS
    if (pos > response.size())
      return "";
  }

  // Parse answer section
  for (uint16_t a = 0; a < ancount; ++a) {
    // NAME
    if (pos >= response.size())
      return "";
    if ((response[pos] & 0xC0) == 0xC0) {
      pos += 2; // compressed name pointer
    } else {
      while (pos < response.size()) {
        uint8_t len = response[pos];
        if (len == 0) {
          ++pos;
          break;
        }
        if ((len & 0xC0) == 0xC0) {
          pos += 2;
          break;
        }
        pos += len + 1;
      }
    }
    if (pos + 10 > response.size())
      return "";

    uint16_t type =
        static_cast<uint16_t>(response[pos] << 8) | response[pos + 1];
    pos += 2;
    pos += 2; // skip CLASS
    pos += 4; // skip TTL
    uint16_t rdlength =
        static_cast<uint16_t>(response[pos] << 8) | response[pos + 1];
    pos += 2;

    if (type == 1 && rdlength == 4 && pos + 4 <= response.size()) {
      // A record: 4-byte IPv4 address
      char ip[16];
      std::snprintf(ip, sizeof(ip), "%d.%d.%d.%d", response[pos],
                    response[pos + 1], response[pos + 2], response[pos + 3]);
      return std::string(ip);
    }
    pos += rdlength;
  }

  return "";
}

// Resolve a hostname to an IP address string via kDnsServer using raw DNS.
std::string ResolveHostname(const std::string &hostname) {
  std::vector<uint8_t> query = BuildDnsQuery(hostname);

  SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == INVALID_SOCKET_VAL)
    return "";

  struct sockaddr_in dns_addr = {};
  dns_addr.sin_family = AF_INET;
  dns_addr.sin_port = htons(static_cast<uint16_t>(std::stoi(kDnsPort)));
  if (inet_pton(AF_INET, kDnsServer, &dns_addr.sin_addr) != 1) {
    CLOSE_SOCKET(sock);
    return "";
  }

  if (sendto(sock, reinterpret_cast<const char *>(query.data()), query.size(),
             0, reinterpret_cast<struct sockaddr *>(&dns_addr),
             sizeof(dns_addr)) < 0) {
    CLOSE_SOCKET(sock);
    return "";
  }

  // Set receive timeout (3 seconds)
#if defined(_WIN32)
  DWORD timeout = 3000;
#else
  struct timeval timeout = {};
  timeout.tv_sec = 3;
#endif
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout), sizeof(timeout));

  std::vector<uint8_t> response(512);
  struct sockaddr_in from = {};
  socklen_t fromlen = sizeof(from);
  int n = static_cast<int>(
      recvfrom(sock, reinterpret_cast<char *>(response.data()), response.size(),
               0, reinterpret_cast<struct sockaddr *>(&from), &fromlen));
  CLOSE_SOCKET(sock);

  if (n <= 0)
    return "";
  response.resize(static_cast<size_t>(n));

  return ParseDnsResponse(response);
}

// Perform a single HTTP request over a fresh TLS connection and return the raw
// response. All public verbs (Get/Post/Put/Delete) funnel through here so the
// socket + OpenSSL handshake logic lives in exactly one place. A non-empty
// body adds Content-Type and Content-Length headers.
std::string
PerformRequest(const std::string &method, const std::string &url,
               const std::string &body = "",
               const std::string &contentType = kDefaultContentType) {
  std::string host, path, port;
  std::string remaining = url;

  bool isHttps = true;
  port = "443"; // Default to HTTPS port

  if (remaining.rfind("https://", 0) == 0) {
    remaining = remaining.substr(8);
  } else if (remaining.rfind("http://", 0) == 0) {
    remaining = remaining.substr(7);
    port = "80";
    isHttps = false;
  }

  size_t path_pos = remaining.find('/');
  if (path_pos != std::string::npos) {
    host = remaining.substr(0, path_pos);
    path = remaining.substr(path_pos);
  } else {
    host = remaining;
    path = "/";
  }

  size_t colon_pos = host.find(':');
  if (colon_pos != std::string::npos) {
    port = host.substr(colon_pos + 1);
    host = host.substr(0, colon_pos);
  }

  // Resolve hostname via kDnsServer (1.1.1.1)
  std::string ip = ResolveHostname(host);
  SocketType sock = INVALID_SOCKET_VAL;

  if (!ip.empty()) {
    // Custom DNS resolution succeeded: connect directly via IPv4
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(std::stoi(port)));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) == 1) {
      sock = socket(AF_INET, SOCK_STREAM, 0);
      if (sock != INVALID_SOCKET_VAL) {
        if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr),
                    sizeof(addr)) != 0) {
          CLOSE_SOCKET(sock);
          sock = INVALID_SOCKET_VAL;
        }
      }
    }
  }

  if (sock == INVALID_SOCKET_VAL) {
    // Fallback: use system resolver
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
      std::cerr << "Failed to resolve hostname: " << host << std::endl;
      return "";
    }
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
      sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (sock == INVALID_SOCKET_VAL)
        continue;
      if (connect(sock, p->ai_addr, p->ai_addrlen) == 0)
        break;
      CLOSE_SOCKET(sock);
      sock = INVALID_SOCKET_VAL;
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCKET_VAL) {
      std::cerr << "Failed to connect to host: " << host << std::endl;
      return "";
    }
  }

  if (sock == INVALID_SOCKET_VAL) {
    std::cerr << "Failed to connect to host: " << host << std::endl;
    return "";
  }

  // Form the HTTP request line + headers (+ optional body). Identical for the
  // plain-HTTP and HTTPS paths.
  std::stringstream request;
  request << method << " " << path << " HTTP/1.1\r\n"
          << "Host: " << host << "\r\n"
          << "User-Agent: HylmiBrowser/1.0\r\n"
          << "Accept: */*\r\n";
  if (!body.empty()) {
    request << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n";
  }
  request << "Connection: close\r\n\r\n";
  if (!body.empty()) {
    request << body;
  }
  std::string req_str = request.str();

  // Plain HTTP (e.g. http://localhost): send/recv over the raw socket, no TLS.
  if (!isHttps) {
    std::string response;
    size_t total = 0;
    while (total < req_str.size()) {
      int n = static_cast<int>(send(sock, req_str.data() + total,
                                    static_cast<int>(req_str.size() - total),
                                    0));
      if (n <= 0) {
        std::cerr << "Failed to send HTTP request." << std::endl;
        CLOSE_SOCKET(sock);
        return "";
      }
      total += static_cast<size_t>(n);
    }
    char rbuf[4096];
    int bytes = 0;
    do {
      bytes = static_cast<int>(recv(sock, rbuf, sizeof(rbuf), 0));
      if (bytes > 0) {
        response.append(rbuf, bytes);
      }
    } while (bytes > 0);
    CLOSE_SOCKET(sock);
    return response;
  }

  // Create SSL Context
  const SSL_METHOD *ssl_method = TLS_client_method();
  SSL_CTX *ctx = SSL_CTX_new(ssl_method);
  if (!ctx) {
    std::cerr << "Failed to create SSL context." << std::endl;
    CLOSE_SOCKET(sock);
    return "";
  }

  // Skip peer certificate verification for simplicity in custom client
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

  SSL *ssl = SSL_new(ctx);
  if (!ssl) {
    std::cerr << "Failed to create SSL structure." << std::endl;
    SSL_CTX_free(ctx);
    CLOSE_SOCKET(sock);
    return "";
  }

  SSL_set_fd(ssl, (int)sock);

  // Enable SNI (Server Name Indication)
  SSL_set_tlsext_host_name(ssl, host.c_str());

  if (SSL_connect(ssl) <= 0) {
    std::cerr << "SSL connection handshake failed." << std::endl;
    ERR_print_errors_fp(stderr);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    CLOSE_SOCKET(sock);
    return "";
  }

  if (SSL_write(ssl, req_str.c_str(), req_str.length()) <= 0) {
    std::cerr << "Failed to send HTTP request over SSL." << std::endl;
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    CLOSE_SOCKET(sock);
    return "";
  }

  // Read response in chunks
  std::string response;
  char buf[4096];
  int bytes = 0;
  do {
    bytes = SSL_read(ssl, buf, sizeof(buf) - 1);
    if (bytes > 0) {
      buf[bytes] = '\0';
      response.append(buf, bytes);
    }
  } while (bytes > 0);

  // Shut down SSL connection and clean up socket
  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  CLOSE_SOCKET(sock);

  return response;
}

} // namespace

std::string Get(const std::string &url) { return PerformRequest("GET", url); }

std::string Post(const std::string &url, const std::string &body,
                 const std::string &contentType) {
  return PerformRequest("POST", url, body, contentType);
}

std::string Put(const std::string &url, const std::string &body,
                const std::string &contentType) {
  return PerformRequest("PUT", url, body, contentType);
}

std::string Delete(const std::string &url) {
  return PerformRequest("DELETE", url);
}

namespace {

// Decode an HTTP/1.1 "Transfer-Encoding: chunked" body: a sequence of
// <hex-size>CRLF <data> CRLF blocks terminated by a zero-size chunk. Apache and
// nginx use this instead of Content-Length for dynamically generated pages
// (e.g. directory listings), so without decoding the body still carries the
// chunk-size framing and is unusable.
std::string DecodeChunked(const std::string &body) {
  std::string out;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t lineEnd = body.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
      break;
    }
    // The size line may carry ";chunk-extension" after the hex size.
    std::string sizeLine = body.substr(pos, lineEnd - pos);
    size_t semi = sizeLine.find(';');
    if (semi != std::string::npos) {
      sizeLine = sizeLine.substr(0, semi);
    }
    size_t s = sizeLine.find_first_not_of(" \t");
    size_t e = sizeLine.find_last_not_of(" \t");
    if (s == std::string::npos) {
      pos = lineEnd + 2;
      continue;
    }
    sizeLine = sizeLine.substr(s, e - s + 1);

    unsigned long chunkSize = 0;
    try {
      chunkSize = std::stoul(sizeLine, nullptr, 16);
    } catch (...) {
      break; // malformed size line: stop with what we have
    }
    pos = lineEnd + 2; // past the size line's CRLF
    if (chunkSize == 0) {
      break; // last chunk
    }
    if (pos + chunkSize > body.size()) {
      out.append(body, pos, body.size() - pos); // truncated response
      break;
    }
    out.append(body, pos, chunkSize);
    pos += chunkSize;
    // Skip the CRLF that terminates the chunk data.
    if (pos + 2 <= body.size() && body[pos] == '\r' && body[pos + 1] == '\n') {
      pos += 2;
    }
  }
  return out;
}

} // namespace

std::string ExtractBody(const std::string &response) {
  size_t delimiter = response.find("\r\n\r\n");
  if (delimiter == std::string::npos) {
    return response;
  }
  std::string headers = response.substr(0, delimiter);
  std::string body = response.substr(delimiter + 4);

  // De-chunk if the server used Transfer-Encoding: chunked.
  std::string lowerHeaders = headers;
  std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  size_t te = lowerHeaders.find("transfer-encoding:");
  if (te != std::string::npos) {
    size_t eol = lowerHeaders.find("\r\n", te);
    std::string value = lowerHeaders.substr(
        te, eol == std::string::npos ? std::string::npos : eol - te);
    if (value.find("chunked") != std::string::npos) {
      return DecodeChunked(body);
    }
  }
  return body;
}

} // namespace Net
} // namespace DesktopWebview
