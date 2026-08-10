#pragma once

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace au {

namespace {

/** Stricter than strtod, which accepts things that make poor search patterns:
 * hex floats (it reads "0x1f" as 31.0, so searching for a hex string would
 * silently match numbers too), leading whitespace, and NaN (matching compares
 * with ==, so a NaN pattern could never match anything).
 *
 * Infinity is fine, and so are values that underflow: strtod reports ERANGE
 * for those, but a denormal or zero is representable and matchable. Only
 * overflow is rejected. */
inline std::optional<double> parseDoublePattern(const std::string &pattern) {
  if (pattern.empty()) return std::nullopt;
  if (std::isspace(static_cast<unsigned char>(pattern.front())))
    return std::nullopt;

  std::string_view digits(pattern);
  if (digits.front() == '+' || digits.front() == '-') digits.remove_prefix(1);
  if (digits.starts_with("0x") || digits.starts_with("0X"))
    return std::nullopt;

  const char *str = pattern.c_str();
  char *end;
  errno = 0;
  double val = std::strtod(str, &end);
  if (end != str + pattern.size()) return std::nullopt;
  if (errno == ERANGE && std::isinf(val)) return std::nullopt;
  if (std::isnan(val)) return std::nullopt;
  return val;
}

}

}
