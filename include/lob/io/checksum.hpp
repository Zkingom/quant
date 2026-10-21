#pragma once

// FNV-1a 64-bit: the integrity check for journal batches and snapshot files
// (DESIGN.md §6.3). Not cryptographic; it detects accidental damage. Every
// step (xor a byte, multiply by an odd prime mod 2^64) is a bijection of the
// state, so any change confined to a single byte always changes the hash.

#include <cstdint>
#include <string>
#include <string_view>

namespace lob::io {

inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view bytes,
                                              std::uint64_t hash = kFnvOffsetBasis) noexcept {
  for (const char c : bytes) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

/// 16 lowercase hex digits.
[[nodiscard]] std::string to_hex16(std::uint64_t value);

}  // namespace lob::io
