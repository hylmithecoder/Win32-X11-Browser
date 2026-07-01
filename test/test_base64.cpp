#include "Base64.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
  using namespace DesktopWebview::Base64;

  // ---- decode tests --------------------------------------------------------
  {
    // "Hello" = "SGVsbG8="
    auto out = decode("SGVsbG8=");
    assert(out.size() == 5);
    assert(out[0] == 'H' && out[1] == 'e' && out[2] == 'l' && out[3] == 'l' &&
           out[4] == 'o');
    std::printf("PASS decode 'Hello'\n");
  }
  {
    // "Man" = "TWFu"
    auto out = decode("TWFu");
    assert(out.size() == 3);
    assert(out[0] == 'M' && out[1] == 'a' && out[2] == 'n');
    std::printf("PASS decode 'Man'\n");
  }
  {
    // With whitespace: "SG Vs\nbG8="
    auto out = decode("SG Vs\nbG8=");
    assert(out.size() == 5);
    assert(std::string(out.begin(), out.end()) == "Hello");
    std::printf("PASS decode with whitespace\n");
  }
  {
    // Empty
    auto out = decode("");
    assert(out.empty());
    std::printf("PASS decode empty\n");
  }
  {
    // No padding: "TWFu" (3 chars -> exact 3 bytes)
    auto out = decode("TWFu");
    assert(out.size() == 3);
    std::printf("PASS decode no-pad exact\n");
  }

  // ---- encode tests --------------------------------------------------------
  {
    std::string s = encode(reinterpret_cast<const std::uint8_t *>("Hello"), 5);
    assert(s == "SGVsbG8=");
    std::printf("PASS encode 'Hello' -> %s\n", s.c_str());
  }
  {
    std::string s = encode(reinterpret_cast<const std::uint8_t *>("Man"), 3);
    assert(s == "TWFu");
    std::printf("PASS encode 'Man' -> %s\n", s.c_str());
  }
  {
    std::string s = encode(reinterpret_cast<const std::uint8_t *>("Ma"), 2);
    assert(s == "TWE=");
    std::printf("PASS encode 'Ma' -> %s\n", s.c_str());
  }
  {
    std::string s = encode(reinterpret_cast<const std::uint8_t *>("M"), 1);
    assert(s == "TQ==");
    std::printf("PASS encode 'M' -> %s\n", s.c_str());
  }
  {
    std::string s = encode(nullptr, 0);
    assert(s.empty());
    std::printf("PASS encode null\n");
  }

  // ---- round-trip test -----------------------------------------------------
  {
    std::string original = "The quick brown fox jumps over the lazy dog!";
    std::vector<std::uint8_t> bytes(original.begin(), original.end());
    std::string encoded = encode(bytes);
    auto decoded = decode(encoded);
    assert(decoded.size() == original.size());
    assert(std::string(decoded.begin(), decoded.end()) == original);
    std::printf("PASS round-trip (%zu bytes)\n", original.size());
  }

  // ---- GPU status ----------------------------------------------------------
  std::printf("OpenCL available: %s\n", isOpenCLAvailable() ? "yes" : "no");

  std::printf("\nAll Base64 tests passed!\n");
  return 0;
}
