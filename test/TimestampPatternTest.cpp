#include <gmock/gmock.h>

#include "TimestampPattern.h"

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
  EXPECT_TRUE(result);
  EXPECT_EQ(result->first,
    au::parseTimestampPattern("2021-12-01 00:12")->first);
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
