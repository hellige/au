#include "NumericPattern.h"

#include <gmock/gmock.h>

#include <cmath>
#include <limits>
#include <string>

namespace au {

static void expectParses(const std::string &pattern, double expected) {
  auto val = parseDoublePattern(pattern);
  ASSERT_TRUE(val.has_value()) << "expected '" << pattern << "' to parse";
  EXPECT_EQ(expected, *val) << "for pattern '" << pattern << "'";
}

static void expectRejected(const std::string &pattern) {
  EXPECT_FALSE(parseDoublePattern(pattern).has_value())
      << "expected '" << pattern << "' to be rejected";
}

TEST(NumericPattern, ParsesPlainDecimals) {
  expectParses("0", 0.0);
  expectParses("1.5", 1.5);
  expectParses("2.5", 2.5);
  expectParses("100.25", 100.25);
  expectParses("1e3", 1000.0);
  expectParses("1E3", 1000.0);
}

TEST(NumericPattern, ParsesNegatives) {
  expectParses("-1.5", -1.5);
  expectParses("-0.001", -0.001);
  expectParses("-1e3", -1000.0);
  expectParses("-0", -0.0);
}

TEST(NumericPattern, ParsesExplicitPlusSign) {
  expectParses("+1.5", 1.5);
}

TEST(NumericPattern, ParsesLeadingDecimalPoint) {
  expectParses(".5", 0.5);
  expectParses("-.5", -0.5);
}

TEST(NumericPattern, ParsesValuesWhichUnderflow) {
  auto denormal = parseDoublePattern("1e-320");
  ASSERT_TRUE(denormal.has_value());
  EXPECT_GT(*denormal, 0.0);
  EXPECT_LT(*denormal, std::numeric_limits<double>::min());

  expectParses("1e-999", 0.0);
}

TEST(NumericPattern, RejectsValuesWhichOverflow) {
  expectRejected("1e400");
  expectRejected("-1e400");
}

TEST(NumericPattern, AcceptsInfinity) {
  auto inf = parseDoublePattern("inf");
  ASSERT_TRUE(inf.has_value());
  EXPECT_TRUE(std::isinf(*inf));
  EXPECT_GT(*inf, 0.0);

  auto negInf = parseDoublePattern("-Infinity");
  ASSERT_TRUE(negInf.has_value());
  EXPECT_TRUE(std::isinf(*negInf));
  EXPECT_LT(*negInf, 0.0);
}

TEST(NumericPattern, RejectsNan) {
  expectRejected("nan");
  expectRejected("NaN");
  expectRejected("-nan");
}

TEST(NumericPattern, RejectsHexFloats) {
  expectRejected("0x1f");
  expectRejected("0X1F");
  expectRejected("0x1p3");
  expectRejected("-0x1p3");
  expectRejected("0xdeadbeef");
}

TEST(NumericPattern, RejectsLeadingWhitespace) {
  expectRejected(" 1.5");
  expectRejected("\t1.5");
}

TEST(NumericPattern, RejectsTrailingJunk) {
  expectRejected("1.5x");
  expectRejected("1.5 ");
  expectRejected("1.5.5");
}

TEST(NumericPattern, RejectsNonNumbers) {
  expectRejected("");
  expectRejected("hello");
  expectRejected("-");
  expectRejected("+");
  expectRejected(".");
}

// parseDoublePattern inspects errno, so a failing call must not poison the next
TEST(NumericPattern, DoesNotLeaveErrnoSetOnSuccess) {
  expectRejected("1e400");
  expectParses("1.5", 1.5);
}

}
