#pragma once

#include "au/AuCommon.h"

#include <string_view>
#include <chrono>
#include <cctype>
#include <cstring>
#include <utility>

namespace au {

struct TimestampPattern {
  time_point start;
  time_point end;
  bool isRelativeTime;

  bool operator==(const TimestampPattern &that) const {
    return std::tie(start, end, isRelativeTime)
      == std::tie(that.start, that.end, that.isRelativeTime);
  }
};

namespace {

template <bool strict=true>
bool parsePrefix(std::string_view &str, size_t len, const char *delims,
                 int &start, int &end, int max, int min = 0, int base = 0) {
  if (str.empty()) {
    start = end = min - base;
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

  if (start > max) return false;
  // this is a little subtle. the goal is, when min is 1 for example, to accept
  // "0" (since it's a valid prefix of "01") but to reject "00" (since it's
  // fully specified and outside the valid range). the following check
  // accomplishes the goal. first, if start >= min, we're clearly ok. if
  // start < min, then end will be > min iff the string is a valid prefix.
  // e.g., suppose min is 012. then if the string is 01, end will be 020, which
  // is > 012 as needed. if the string is 011, then end will be 012, exactly
  // equal to min, failing the check and thus rejected.
  if (start < min) {
    if (end <= min) return false;
    start = min;
  }
  start -= base;
  end = std::min(end, max + 1);
  end -= base;
  return true;
}

template <bool strict=true>
std::optional<TimestampPattern>
inline parseTimestampPattern(std::string_view sv) {
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
  return TimestampPattern{startInt, endInt, false};
}

std::optional<TimestampPattern>
inline parseTimePattern(std::string_view sv) {
  std::tm start;
  std::tm end;
  memset(&start, 0, sizeof(tm));
  memset(&end, 0, sizeof(tm));
  start.tm_year = end.tm_year = 70;
  start.tm_mon = end.tm_mon = 0;
  start.tm_mday = end.tm_mday = 1;

  if (!parsePrefix(sv, 2, ":", start.tm_hour, end.tm_hour, 23))
    return std::nullopt;
  if (!parsePrefix(sv, 2, ":", start.tm_min, end.tm_min, 59))
    return std::nullopt;
  if (!parsePrefix(sv, 2, ".,", start.tm_sec, end.tm_sec, 59))
    return std::nullopt;

  int startNanos;
  int endNanos;
  if (!parsePrefix(sv, 9, "", startNanos, endNanos, 999999999))
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
  return TimestampPattern{startInt, endInt, true};
}

std::optional<TimestampPattern>
inline parseFlexPattern(const std::string &tsPat) {
  if (auto result = parseTimePattern(tsPat); result)
    return result;
  return parseTimestampPattern(tsPat);
}

}

}
