#ifndef BASE64_HPP
#define BASE64_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Base64 {

// Decode a base64-encoded string into raw bytes. Strips whitespace before
// decoding. Returns an empty vector on invalid input.
std::vector<std::uint8_t> decode(const std::string &input);

// Encode raw bytes into a base64 string (no line breaks).
std::string encode(const std::vector<std::uint8_t> &input);
std::string encode(const std::uint8_t *data, std::size_t len);

// Decode a base64-encoded string. Returns false on invalid input.
bool decode(const std::string &input, std::vector<std::uint8_t> &out);

// Initialise the OpenCL context (called lazily on first use). Returns true
// if a GPU device was found and the kernel was compiled; false means the CPU
// fallback path will be used.
bool initOpenCL();

// True when OpenCL is available and the GPU path is active.
bool isOpenCLAvailable();

} // namespace Base64
} // namespace DesktopWebview

#endif // BASE64_HPP
