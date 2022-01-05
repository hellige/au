#pragma once

#include "au/AuCommon.h"

#include <string_view>
#include <chrono>
#include <cctype>
#include <cstring>
#include <utility>

namespace au {

namespace {

template <bool strict=true>
bool parsePrefix(std::string_view &str, size_t len, const char *delims,
                 int &start, int &end, int max, int min = 0, int base = 0) {
  if (str.empty()) {
    start = end = 0;
    return true;
  }

  auto result = 0;
  auto i = 0u;
  for (; i < len; i++) {
    if (i == str.size()) break;
    auto c = str[i];
    if (strchr(delims, c)) return false;
    if (!isdigit(c)) {
      if (strict) return false;
      break;
    }
    result = 10 * result + c - '0';
  }
  str.remove_prefix(i);
  start = end = result;
  if (str.empty()) {
    end += 1;
  } else {
    if (!strchr(delims, str[0]) && strict) return false;
    str.remove_prefix(1);
    if (str.empty() && strict) return false;
  }
  for (; i < len; i++) {
    start *= 10;
    end *= 10;
  }
  if (start < min || start > max) return false;
  start -= base;
  if (end < min || end > max + 1) return false;
  end -= base;
  return true;
}

using TimestampPattern = std::pair<time_point, time_point>;

template <bool strict=true>
std::optional<TimestampPattern>
parseTimestampPattern(std::string_view sv) {
  std::tm start;
  std::tm end;
  memset(&start, 0, sizeof(tm));
  memset(&end, 0, sizeof(tm));

  // note the occurence of <strict> vs always-strict. it seems reasonable, in
  // non-strict mode, to insist on at least a full yyyy-mm-dd date, and then if
  // followed by a number, at least "yyyy-mm-dd hh:mm". other formats like
  // "yyyy-mm" or "yyyy-mm-dd hh" are pretty rare, and seem highly likely to
  // just lead to confusion. note that this applies *only* in non-strict mode
  // with trailing characters, you can still use patterns like those for
  // searching.

  if (!parsePrefix(sv, 4, "-", start.tm_year, end.tm_year, 9999, 1900, 1900))
    return std::nullopt;
  if (!parsePrefix(sv, 2, "-", start.tm_mon, end.tm_mon, 12, 1, 1))
    return std::nullopt;
  if (!parsePrefix<strict>(sv, 2, "T ", start.tm_mday, end.tm_mday, 31, 1))
    return std::nullopt;
  if (!parsePrefix(sv, 2, ":", start.tm_hour, end.tm_hour, 23))
    return std::nullopt;
  if (!parsePrefix<strict>(sv, 2, ":", start.tm_min, end.tm_min, 59))
    return std::nullopt;
  if (!parsePrefix<strict>(sv, 2, ".,", start.tm_sec, end.tm_sec, 59))
    return std::nullopt;

  int startNanos;
  int endNanos;
  if (!parsePrefix<strict>(sv, 9, "", startNanos, endNanos, 999999999))
    return std::nullopt;

  std::time_t ttstart = timegm(&start);
  std::time_t ttend = timegm(&end);
  if (ttstart == -1 || ttend == -1) return std::nullopt;

  using namespace std::chrono;
  auto epoch = time_point();
  auto startInt = epoch + duration_cast<nanoseconds>(
      seconds(ttstart) + nanoseconds(startNanos));
  auto endInt = epoch + duration_cast<nanoseconds>(
      seconds(ttend) + nanoseconds(endNanos));

  if (startInt == endInt) endInt += nanoseconds(1);
  return std::make_pair(startInt, endInt);
}

}

}
