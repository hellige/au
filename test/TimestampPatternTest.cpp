#include <gmock/gmock.h>

#include "TimestampPattern.h"

#include <cinttypes>
#include <optional>
#include <string_view>

namespace {

std::string str(const au::TimestampPattern &tp) {
  using namespace std::chrono;

  std::tm startTm;
  std::tm endTm;
  std::time_t ttstart = system_clock::to_time_t(tp.start);
  std::time_t ttend = system_clock::to_time_t(tp.end);
  gmtime_r(&ttstart, &startTm);
  gmtime_r(&ttend, &endTm);

  auto startNanos = (tp.start - time_point_cast<seconds>(tp.start)).count();
  auto endNanos = (tp.end - time_point_cast<seconds>(tp.end)).count();

  char buf[64], buf2[64], buf3[256];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &startTm);
  strftime(buf2, sizeof(buf2), "%Y-%m-%d %H:%M:%S", &endTm);
  snprintf(buf3, sizeof(buf3), "%s.%09" PRId64 " - %s.%09" PRId64, buf,
    startNanos, buf2, endNanos);
  return buf3;
}

std::string str(const std::optional<au::TimestampPattern> &tp) {
  if (!tp) return "None";
  return str(*tp);
}

}

TEST(TimestampPatternTest, ParsePrefix) {
  auto pp = [](auto str, size_t len, const char *delims, int max,
      int min = 0, int base = 0) -> std::optional<std::pair<int, int>> {
    int start, end;
    auto sv = std::string_view(str);
    if (au::parsePrefix(sv, len, delims, start, end, max, min, base))
      return std::make_pair(start, end);
    return std::nullopt;
  };

  auto pr = pp("2", 2, ":", 23);
  ASSERT_TRUE(pr);
  EXPECT_EQ(20, pr->first);
  EXPECT_EQ(24, pr->second);

  pr = pp("11", 2, ":", 23);
  ASSERT_TRUE(pr);
  EXPECT_EQ(11, pr->first);
  EXPECT_EQ(12, pr->second);

  pr = pp("1", 2, ":", 23);
  ASSERT_TRUE(pr);
  EXPECT_EQ(10, pr->first);
  EXPECT_EQ(20, pr->second);

  pr = pp("0", 2, ":", 23);
  ASSERT_TRUE(pr);
  EXPECT_EQ(00, pr->first);
  EXPECT_EQ(10, pr->second);

  pr = pp("2000", 4, "-", 9999, 1900, 1900);
  ASSERT_TRUE(pr);
  EXPECT_EQ(100, pr->first);
  EXPECT_EQ(101, pr->second);
}

TEST(TimestampPatternTest, RejectsWithTrailingDelim) {
  EXPECT_FALSE(au::parseTimestampPattern("2021-12-01T00:12:"));
  EXPECT_TRUE(au::parseTimestampPattern("2021-12-01T00:12"));
  EXPECT_TRUE(au::parseTimestampPattern("2021-12-01T00:12:3"));
}

TEST(TimestampPatternTest, RejectsWithGarbageAfter) {
  EXPECT_FALSE(au::parseTimestampPattern("2021-12-01T00:12:34abc"));
}

TEST(TimestampPatternTest, RejectsWithTooMuchPrecision) {
  EXPECT_FALSE(au::parseTimestampPattern("2021-12-01T00:12:34.123456789012"));
}

TEST(TimestampPatternTest, RejectsWithGarbageAfterFullLength) {
  EXPECT_FALSE(au::parseTimestampPattern("2021-12-01T00:12:34.123456789abc"));
}

TEST(TimestampPatternTest, AcceptsFullLength) {
  EXPECT_TRUE(au::parseTimestampPattern("2021-12-01T00:12:34.123456789"));
}

TEST(TimestampPatternTest, NonStrictAcceptsWithGarbageAfterFullLength) {
  EXPECT_TRUE(au::parseTimestampPattern<false>(
    "2021-12-01T00:12:34.123456789abc"));
}

TEST(TimestampPatternTest, NonStrictAcceptsWithTooMuchPrecision) {
  EXPECT_TRUE(au::parseTimestampPattern<false>(
    "2021-12-01T00:12:34.123456789012"));
}

TEST(TimestampPatternTest, NonStrictWithGarbageMatchesStrict) {
  EXPECT_EQ(
    au::parseTimestampPattern<false>("2021-12-01T00:12:34.123456789abc"),
    au::parseTimestampPattern("2021-12-01T00:12:34.123456789"));
}

TEST(TimestampPatternTest, NonStrictAcceptsWithTrailingDelim) {
  auto result = au::parseTimestampPattern<false>("2021-12-01 00:12:");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->start,
    au::parseTimestampPattern("2021-12-01 00:12")->start);
}

TEST(TimestampPatternTest, NonStrictRejectsSomeTrailingDelims) {
  EXPECT_FALSE(au::parseTimestampPattern<false>("2021-"));
  EXPECT_FALSE(au::parseTimestampPattern<false>("2021-12-"));
  EXPECT_TRUE(au::parseTimestampPattern<false>("2021-12-01 "));
  EXPECT_FALSE(au::parseTimestampPattern<false>("2021-12-01 00:"));
  EXPECT_TRUE(au::parseTimestampPattern<false>("2021-12-01 00:12:"));
  EXPECT_TRUE(au::parseTimestampPattern<false>("2021-12-01 00:12:34."));

  // note that non-delimiter trailing chars are also ruled out in the same
  // cases:
  EXPECT_FALSE(au::parseTimestampPattern<false>("2021/"));
  EXPECT_FALSE(au::parseTimestampPattern<false>("2021-12/"));
  EXPECT_TRUE(au::parseTimestampPattern<false>("2021-12-01/"));
  EXPECT_FALSE(au::parseTimestampPattern<false>("2021-12-01 00/"));
  EXPECT_TRUE(au::parseTimestampPattern<false>("2021-12-01 00:12/"));
  EXPECT_TRUE(au::parseTimestampPattern<false>("2021-12-01 00:12:34/"));
}

TEST(TimestampPatternTest, VariousFormatsMatch) {
  EXPECT_EQ(
    au::parseTimestampPattern("2021-12-01 00:12:34,123"),
    au::parseTimestampPattern("2021-12-01T00:12:34.123"));
}

TEST(TimestampPatternTest, ParseTime) {
  EXPECT_EQ(
    str(au::parseTimePattern("20")),
    "1970-01-01 20:00:00.000000000 - 1970-01-01 21:00:00.000000000");

  EXPECT_FALSE(au::parseTimePattern("2022-11-09"));

  EXPECT_EQ(
    str(au::parseTimePattern("05")),
    "1970-01-01 05:00:00.000000000 - 1970-01-01 06:00:00.000000000");

  EXPECT_EQ(
    str(au::parseTimePattern("0")),
    "1970-01-01 00:00:00.000000000 - 1970-01-01 10:00:00.000000000");

  EXPECT_EQ(
    str(au::parseTimePattern("1")),
    "1970-01-01 10:00:00.000000000 - 1970-01-01 20:00:00.000000000");

  EXPECT_EQ(
    str(au::parseTimePattern("2")),
    "1970-01-01 20:00:00.000000000 - 1970-01-02 00:00:00.000000000");
}

TEST(TimestampPatternTest, ParsePartialDate) {
  EXPECT_EQ(
    str(au::parseTimestampPattern("2000-01-01")),
    "2000-01-01 00:00:00.000000000 - 2000-01-02 00:00:00.000000000");

  EXPECT_FALSE(au::parseTimestampPattern("2000-00"));
  EXPECT_FALSE(au::parseTimestampPattern("2000-13"));

  EXPECT_EQ(
    str(au::parseTimestampPattern("2000-0")),
    "2000-01-01 00:00:00.000000000 - 2000-10-01 00:00:00.000000000");

  EXPECT_EQ(
    str(au::parseTimestampPattern("2000-1")),
    "2000-10-01 00:00:00.000000000 - 2001-01-01 00:00:00.000000000");

  EXPECT_EQ(
    str(au::parseTimestampPattern("2000-12")),
    "2000-12-01 00:00:00.000000000 - 2001-01-01 00:00:00.000000000");

  EXPECT_EQ(
    str(au::parseTimestampPattern("2000")),
    "2000-01-01 00:00:00.000000000 - 2001-01-01 00:00:00.000000000");

  EXPECT_EQ(
    str(au::parseTimestampPattern("20")),
    "2000-01-01 00:00:00.000000000 - 2100-01-01 00:00:00.000000000");
}

TEST(TimestampPatternTest, ParseTimeNanos) {
  EXPECT_EQ(
    str(au::parseTimePattern("21:00:10.123")),
    "1970-01-01 21:00:10.123000000 - 1970-01-01 21:00:10.124000000");

  EXPECT_EQ(
    str(au::parseTimePattern("21:00:10.123456789")),
    "1970-01-01 21:00:10.123456789 - 1970-01-01 21:00:10.123456790");

  EXPECT_EQ(
    str(au::parseTimePattern("23:59:59.999")),
    "1970-01-01 23:59:59.999000000 - 1970-01-02 00:00:00.000000000");
}

TEST(TimestampPatternTest, ParseFlexPattern) {
  EXPECT_EQ(
    str(au::parseFlexPattern("1")),
    "1970-01-01 10:00:00.000000000 - 1970-01-01 20:00:00.000000000");

  EXPECT_EQ(
    str(au::parseFlexPattern("21")),
    "1970-01-01 21:00:00.000000000 - 1970-01-01 22:00:00.000000000");

  EXPECT_EQ(
    str(au::parseFlexPattern("202")),
    "2020-01-01 00:00:00.000000000 - 2030-01-01 00:00:00.000000000");

  EXPECT_EQ(
    str(au::parseFlexPattern("20:3")),
    "1970-01-01 20:30:00.000000000 - 1970-01-01 20:40:00.000000000");

  EXPECT_EQ(
    str(au::parseFlexPattern("2022-02-28 23")),
    "2022-02-28 23:00:00.000000000 - 2022-03-01 00:00:00.000000000");

  EXPECT_EQ(
    str(au::parseFlexPattern("2022-02-28")),
    "2022-02-28 00:00:00.000000000 - 2022-03-01 00:00:00.000000000");

  EXPECT_EQ(
    str(au::parseFlexPattern("23")),
    "1970-01-01 23:00:00.000000000 - 1970-01-02 00:00:00.000000000");
}
