#ifndef NET_HPP
#define NET_HPP

#include <string>

namespace DesktopWebview {
namespace Net {

// Initialize socket and SSL libraries
void Init();

// Cleanup libraries
void Cleanup();

// Default media type used for request bodies when none is supplied.
inline constexpr const char *kDefaultContentType =
    "application/x-www-form-urlencoded";

// Perform custom HTTPS requests using raw OpenSSL sockets.
//
// The URL format must be either: https://host/path, http://host/path, or
// simply host (defaulting to HTTPS on port 443). Each call returns the full,
// raw HTTP response (status line + headers + "\r\n\r\n" + body), or an empty
// string on failure. Use Net::ExtractBody() to peel off the body for parsing.

// HTTP GET.
std::string Get(const std::string &url);

// HTTP POST with a request body.
std::string Post(const std::string &url, const std::string &body,
                 const std::string &contentType = kDefaultContentType);

// HTTP PUT with a request body.
std::string Put(const std::string &url, const std::string &body,
                const std::string &contentType = kDefaultContentType);

// HTTP DELETE.
std::string Delete(const std::string &url);

// Split a raw HTTP response into its body, discarding the status line and
// headers. If the header/body delimiter ("\r\n\r\n") is not found, the input is
// returned unchanged.
std::string ExtractBody(const std::string &response);

} // namespace Net
} // namespace DesktopWebview

#endif // NET_HPP
