#pragma once
// Lowercase-hex <-> bytes helpers (header-only). Used by the vector adapter and
// the tests; the conformance vectors carry raw bytes as hex strings.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loom::proto::hex {

inline std::string to_hex(std::span<const std::uint8_t> bytes) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(bytes.size() * 2);
  for (std::uint8_t b : bytes) {
    s.push_back(digits[b >> 4]);
    s.push_back(digits[b & 0x0f]);
  }
  return s;
}

inline std::vector<std::uint8_t> from_hex(std::string_view h) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  std::vector<std::uint8_t> out;
  out.reserve(h.size() / 2);
  for (std::size_t i = 0; i + 1 < h.size(); i += 2)
    out.push_back(static_cast<std::uint8_t>((nib(h[i]) << 4) | nib(h[i + 1])));
  return out;
}

}  // namespace loom::proto::hex
