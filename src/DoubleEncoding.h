#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace au {

namespace {

/** Self-contained candidate encodings for a double: each depends on nothing
 * but the value itself, which is what would let a reader decode a value record
 * found at an arbitrary offset. DoubleAnalysis.h adds the record-local schemes,
 * which need context, but only from within a single record. */
namespace doubleenc {

/// Above this the significand stops fitting in 2^53, so there's nothing to gain.
constexpr unsigned MAX_SCALE = 18;

constexpr double POW10[MAX_SCALE + 1] = {
  1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
  1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18
};

/// Integers below this convert to double and back losslessly.
constexpr double TWO53 = 9007199254740992.0;  // 2^53

enum class Tier {
  Zero,
  NegZero,     //< distinct bit pattern, which the decimal path can't produce
  NonFinite,
  Decimal,     //< significand / 10^scale
  Float32,
  Raw          //< marker + 8 bytes, as today
};

struct Encoding {
  Tier tier;
  unsigned scale;       //< only meaningful for Decimal
  int64_t significand;  //< only meaningful for Decimal
  unsigned bytes;       //< total encoded size, marker included
};

/// Marker plus 8 raw bytes.
constexpr unsigned RAW_BYTES = 9;

inline uint64_t bitsOf(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof(bits));
  return bits;
}

inline uint64_t zigzag(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}

inline unsigned varintLen(uint64_t v) {
  unsigned n = 1;
  while (v >= 0x80) { v >>= 7; ++n; }
  return n;
}

/// Scales up to here fit in the marker's low bits; larger need an extra byte.
constexpr unsigned SCALE_IN_MARKER_LIMIT = 7;

inline unsigned decimalBytes(unsigned scale, int64_t significand) {
  unsigned n = 1 + varintLen(zigzag(significand));
  if (scale > SCALE_IN_MARKER_LIMIT) n++;
  return n;
}

/** What the decoder would do. POW10[scale] is exactly representable, int64 ->
 * double is exact below 2^53, and IEEE-754 division is correctly rounded, so
 * this is bit-identical on any conforming platform. It must stay a single
 * division: -ffast-math or x87 excess precision would break that. */
inline double reconstructDecimal(unsigned scale, int64_t significand) {
  return static_cast<double>(significand) / POW10[scale];
}

/** Smallest scale reproducing d exactly, if any.
 *
 * Works by verification rather than by trusting the search: we compute a
 * candidate significand and check that reconstructing it gives back the
 * identical bit pattern. The result is therefore correct however good or bad
 * the heuristic is, and the fallback is always the encoding we already have.
 *
 * Only finite, non-zero values should be passed here. */
inline bool findDecimal(double d, unsigned &scale, int64_t &significand,
                        unsigned maxScale = MAX_SCALE) {
  const uint64_t bits = bitsOf(d);
  if (maxScale > MAX_SCALE) maxScale = MAX_SCALE;
  for (unsigned k = 0; k <= maxScale; ++k) {
    const double scaled = d * POW10[k];
    // |scaled| only grows with k, so give up rather than continue. Also
    // catches overflow to infinity.
    if (!(std::fabs(scaled) < TWO53)) return false;
    const auto sig = static_cast<int64_t>(std::llround(scaled));
    if (bitsOf(reconstructDecimal(k, sig)) == bits) {
      scale = k;
      significand = sig;
      return true;
    }
  }
  return false;
}

/** A decimal form that verifies is not automatically worth using: a value
 * needing 15 significant digits takes 8 varint bytes, which with a marker and
 * an explicit scale comes to 10, worse than storing the 8 raw bytes. */
inline Encoding classify(double d, unsigned maxScale = MAX_SCALE) {
  const uint64_t bits = bitsOf(d);

  if (bits == 0) return {Tier::Zero, 0, 0, 1};
  if (bits == 0x8000000000000000ull) return {Tier::NegZero, 0, 0, RAW_BYTES};
  if (!std::isfinite(d)) return {Tier::NonFinite, 0, 0, RAW_BYTES};

  Encoding best{Tier::Raw, 0, 0, RAW_BYTES};
  if (static_cast<double>(static_cast<float>(d)) == d)
    best = {Tier::Float32, 0, 0, 5};

  unsigned scale;
  int64_t significand;
  if (findDecimal(d, scale, significand, maxScale)) {
    // smallest verifying scale yields the smallest significand, so no larger
    // scale could be more compact
    const unsigned bytes = decimalBytes(scale, significand);
    if (bytes < best.bytes) best = {Tier::Decimal, scale, significand, bytes};
  }
  return best;
}

inline void zeroBytes(uint64_t v, unsigned &leading, unsigned &trailing) {
  if (!v) { leading = 8; trailing = 0; return; }
  leading = static_cast<unsigned>(__builtin_clzll(v)) / 8u;
  trailing = static_cast<unsigned>(__builtin_ctzll(v)) / 8u;
}

/// Marker, a control byte holding leading-zero count and length, then the
/// significant bytes. RAW_BYTES if that wouldn't be an improvement.
inline unsigned xorBytes(uint64_t x) {
  if (!x) return 1;
  unsigned leading, trailing;
  zeroBytes(x, leading, trailing);
  const unsigned significant = 8u - leading - trailing;
  const unsigned n = 2u + significant;
  return n < RAW_BYTES ? n : RAW_BYTES;
}

}

}

}
