#include "DoubleEncoding.h"

#include <gmock/gmock.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace au {

using namespace doubleenc;

static uint64_t bits(double d) { return bitsOf(d); }

/// Whatever tier is chosen, decoding it must give back the original bits.
static void expectExact(double d) {
  const auto enc = classify(d);
  switch (enc.tier) {
    case Tier::Zero:
      EXPECT_EQ(bits(0.0), bits(d)) << "for " << d;
      break;
    case Tier::Decimal:
      EXPECT_EQ(bits(d), bits(reconstructDecimal(enc.scale, enc.significand)))
          << "for " << d << " as " << enc.significand << "e-" << enc.scale;
      break;
    case Tier::Float32:
      EXPECT_EQ(bits(d), bits(static_cast<double>(static_cast<float>(d))))
          << "for " << d;
      break;
    case Tier::NegZero:
    case Tier::NonFinite:
    case Tier::Raw:
      // stored verbatim, so exact by definition
      break;
  }
  EXPECT_LE(enc.bytes, RAW_BYTES)
      << "encoding " << d << " must never cost more than storing it raw";
  EXPECT_GE(enc.bytes, 1u);
}

TEST(DoubleEncoding, ZeroIsASingleByte) {
  auto enc = classify(0.0);
  EXPECT_EQ(Tier::Zero, enc.tier);
  EXPECT_EQ(1u, enc.bytes);
}

TEST(DoubleEncoding, NegativeZeroFallsBackToRaw) {
  auto enc = classify(-0.0);
  EXPECT_EQ(Tier::NegZero, enc.tier);
  EXPECT_EQ(RAW_BYTES, enc.bytes);
  EXPECT_NE(bits(0.0), bits(-0.0));
}

TEST(DoubleEncoding, NonFiniteFallsBackToRaw) {
  for (double d : {std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()}) {
    auto enc = classify(d);
    EXPECT_EQ(Tier::NonFinite, enc.tier);
    EXPECT_EQ(RAW_BYTES, enc.bytes);
  }
}

TEST(DoubleEncoding, RoundDecimalsCompressWell) {
  struct Case { double value; unsigned scale; int64_t significand; };
  const Case cases[] = {
    {1.0, 0, 1},
    {-1.0, 0, -1},
    {5.9, 1, 59},
    {-5.9, 1, -59},
    {0.5, 1, 5},
    {2.5, 1, 25},
    {100.25, 2, 10025},
    {0.001, 3, 1},
    {1e-7, 7, 1},
    {12345.67, 2, 1234567},
  };
  for (auto &c : cases) {
    auto enc = classify(c.value);
    EXPECT_EQ(Tier::Decimal, enc.tier) << "for " << c.value;
    EXPECT_EQ(c.scale, enc.scale) << "for " << c.value;
    EXPECT_EQ(c.significand, enc.significand) << "for " << c.value;
    EXPECT_LT(enc.bytes, RAW_BYTES) << "for " << c.value;
    EXPECT_EQ(bits(c.value), bits(reconstructDecimal(enc.scale,
                                                     enc.significand)));
  }
}

TEST(DoubleEncoding, PicksTheSmallestScale) {
  // 1.0 is representable at every scale; we want the cheapest.
  auto enc = classify(1.0);
  EXPECT_EQ(0u, enc.scale);
  EXPECT_EQ(2u, enc.bytes);  // marker + one varint byte
}

TEST(DoubleEncoding, FullPrecisionValuesDoNotUseDecimal) {
  // ~17 significant digits, so any decimal form would be larger
  for (double d : {M_PI, 1.0 / 3.0, 0.1 + 0.2}) {
    auto enc = classify(d);
    EXPECT_NE(Tier::Decimal, enc.tier) << "for " << d;
    EXPECT_EQ(RAW_BYTES, enc.bytes) << "for " << d;
  }
}

TEST(DoubleEncoding, NeverChoosesAnEncodingBiggerThanRaw) {
  std::mt19937_64 gen(12345);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  for (int i = 0; i < 20000; i++) {
    double d = dist(gen);
    EXPECT_LE(classify(d).bytes, RAW_BYTES) << "for " << d;
  }
}

TEST(DoubleEncoding, Float32ValuesUseTheFloatTier) {
  // a value which came from a float, but isn't a short decimal
  float f = 1.0f / 3.0f;
  double d = static_cast<double>(f);
  auto enc = classify(d);
  EXPECT_EQ(Tier::Float32, enc.tier);
  EXPECT_EQ(5u, enc.bytes);
  EXPECT_EQ(bits(d), bits(static_cast<double>(static_cast<float>(d))));
}

TEST(DoubleEncoding, ExactRoundTripAcrossManyDistributions) {
  std::mt19937_64 gen(999);
  std::uniform_real_distribution<double> unit(-1.0, 1.0);
  std::uniform_int_distribution<int> places(0, 9);
  std::uniform_int_distribution<int64_t> ints(-1000000, 1000000);

  for (int i = 0; i < 20000; i++) {
    expectExact(unit(gen));
    expectExact(static_cast<double>(ints(gen)));
    // rounded decimals, the case we most expect to see in logs
    double scale = std::pow(10.0, places(gen));
    expectExact(std::round(unit(gen) * 1e6 * scale) / scale);
    // values that started life as floats
    expectExact(static_cast<double>(static_cast<float>(unit(gen) * 1e3)));
  }
}

TEST(DoubleEncoding, ExactRoundTripAtBoundaries) {
  const double values[] = {
    0.0, -0.0, 1.0, -1.0,
    std::numeric_limits<double>::min(),          // smallest normal
    -std::numeric_limits<double>::min(),
    std::numeric_limits<double>::denorm_min(),   // smallest subnormal
    -std::numeric_limits<double>::denorm_min(),
    std::numeric_limits<double>::max(),
    -std::numeric_limits<double>::max(),
    std::numeric_limits<double>::epsilon(),
    9007199254740992.0,    // 2^53
    -9007199254740992.0,
    9007199254740991.0,    // 2^53 - 1
    9007199254740993.0,    // not representable; rounds to 2^53
    1e18, 1e19, 1e20, 1e-300, 1e300,
  };
  for (double d : values) expectExact(d);
}

TEST(DoubleEncoding, ExactRoundTripOverRandomBitPatterns) {
  // arbitrary bit patterns, including NaNs with payloads and subnormals
  std::mt19937_64 gen(4242);
  for (int i = 0; i < 50000; i++) {
    uint64_t b = gen();
    double d;
    std::memcpy(&d, &b, sizeof(d));
    expectExact(d);
  }
}

TEST(DoubleEncoding, ZigzagAndVarintLen) {
  EXPECT_EQ(0u, zigzag(0));
  EXPECT_EQ(1u, zigzag(-1));
  EXPECT_EQ(2u, zigzag(1));
  EXPECT_EQ(3u, zigzag(-2));
  EXPECT_EQ(1u, varintLen(0));
  EXPECT_EQ(1u, varintLen(127));
  EXPECT_EQ(2u, varintLen(128));
  EXPECT_EQ(2u, varintLen(16383));
  EXPECT_EQ(3u, varintLen(16384));
  EXPECT_EQ(10u, varintLen(~uint64_t(0)));
}

TEST(DoubleEncoding, ZeroByteCounts) {
  unsigned leading, trailing;
  zeroBytes(0, leading, trailing);
  EXPECT_EQ(8u, leading);
  EXPECT_EQ(0u, trailing);

  zeroBytes(0x00000000000000ffull, leading, trailing);
  EXPECT_EQ(7u, leading);
  EXPECT_EQ(0u, trailing);

  zeroBytes(0xff00000000000000ull, leading, trailing);
  EXPECT_EQ(0u, leading);
  EXPECT_EQ(7u, trailing);

  zeroBytes(0x0000ff00ff000000ull, leading, trailing);
  EXPECT_EQ(2u, leading);
  EXPECT_EQ(3u, trailing);
}

TEST(DoubleEncoding, XorSizing) {
  EXPECT_EQ(1u, xorBytes(0));                       // identical to previous
  EXPECT_EQ(3u, xorBytes(0x00000000000000ffull));   // marker + control + 1
  EXPECT_EQ(RAW_BYTES, xorBytes(~uint64_t(0)));     // no better than raw
}

TEST(DoubleEncoding, FindDecimalAgreesWithClassify) {
  std::mt19937_64 gen(31337);
  std::uniform_real_distribution<double> dist(-1e4, 1e4);
  for (int i = 0; i < 20000; i++) {
    double d = std::round(dist(gen) * 100.0) / 100.0;
    if (d == 0.0) continue;
    unsigned scale;
    int64_t significand;
    ASSERT_TRUE(findDecimal(d, scale, significand)) << "for " << d;
    EXPECT_EQ(bits(d), bits(reconstructDecimal(scale, significand)));
    auto enc = classify(d);
    if (enc.tier == Tier::Decimal) {
      EXPECT_EQ(scale, enc.scale);
      EXPECT_EQ(significand, enc.significand);
    }
  }
}

}
