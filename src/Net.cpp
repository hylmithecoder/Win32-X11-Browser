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
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SocketType;
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET_VAL -1
#endif

#include <iostream>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sstream>

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

  port = "443"; // Default to HTTPS port

  if (remaining.rfind("https://", 0) == 0) {
    remaining = remaining.substr(8);
  } else if (remaining.rfind("http://", 0) == 0) {
    remaining = remaining.substr(7);
    port = "80";
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

  // Resolve hostname to IP address
  struct addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
    std::cerr << "Failed to resolve hostname: " << host << std::endl;
    return "";
  }

  SocketType sock = INVALID_SOCKET_VAL;
  struct addrinfo *p = nullptr;
  for (p = res; p != nullptr; p = p->ai_next) {
    sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sock == INVALID_SOCKET_VAL)
      continue;

    if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) {
      break; // Successfully connected
    }
    CLOSE_SOCKET(sock);
    sock = INVALID_SOCKET_VAL;
  }

  freeaddrinfo(res);

  if (sock == INVALID_SOCKET_VAL) {
    std::cerr << "Failed to connect to host: " << host << std::endl;
    return "";
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

  // Form the HTTP request line + headers (+ optional body).
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

std::string ExtractBody(const std::string &response) {
  size_t delimiter = response.find("\r\n\r\n");
  if (delimiter == std::string::npos) {
    return response;
  }
  return response.substr(delimiter + 4);
}

} // namespace Net
} // namespace DesktopWebview
