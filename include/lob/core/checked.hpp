#pragma once

// Overflow-checked integer arithmetic. Returns std::nullopt instead of
// invoking undefined behaviour (signed) or wrapping (unsigned).
//
// Built on the GCC/Clang __builtin_*_overflow intrinsics, which are exact for
// every integral type and usable in constant expressions.

#include <concepts>
#include <optional>
#include <type_traits>

#if !defined(__GNUC__)
#error "checked.hpp requires GCC or Clang (__builtin_*_overflow)"
#endif

namespace lob {

template <typename T>
concept CheckedInteger = std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;

template <CheckedInteger T>
[[nodiscard]] constexpr std::optional<T> checked_add(T a, T b) noexcept {
  T result{};
  if (__builtin_add_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

template <CheckedInteger T>
[[nodiscard]] constexpr std::optional<T> checked_sub(T a, T b) noexcept {
  T result{};
  if (__builtin_sub_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

template <CheckedInteger T>
[[nodiscard]] constexpr std::optional<T> checked_mul(T a, T b) noexcept {
  T result{};
  if (__builtin_mul_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

}  // namespace lob
