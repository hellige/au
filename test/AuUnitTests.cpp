#include "au/AuEncoder.h"
#include "au/AuDecoder.h"

#include <gmock/gmock.h>

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace au {

TEST(AuStringIntern, NoIntern) {
  AuStringIntern si;
  EXPECT_EQ(0, si.dict().size());
  EXPECT_FALSE(si.idx(std::string("shrt"), AuIntern::ByFrequency));
  EXPECT_FALSE(si.idx(std::string("Long string"), AuIntern::ByFrequency));
  EXPECT_EQ(0, si.dict().size());
}

TEST(AuStringIntern, ForceIntern) {
  AuStringIntern si;
  EXPECT_EQ(0, si.dict().size());

  // Tiny strings are not interned even if forced
  EXPECT_FALSE(si.idx(std::string("tiny"), AuIntern::ForceIntern));
  EXPECT_EQ(0, si.dict().size());

  EXPECT_TRUE(si.idx(std::string("A normal string"), AuIntern::ForceIntern));
  EXPECT_EQ(1, si.dict().size());
}

TEST(AuStringIntern, InternFrequentStrings) {
  constexpr size_t INTERN_THRESH = 10;
  AuStringIntern si(AuStringIntern::Config{4, INTERN_THRESH});
  std::string str("Normal value");

  EXPECT_FALSE(si.idx(str, AuIntern::ByFrequency));
  EXPECT_EQ(0, si.dict().size());

  for (size_t i = 0; i < INTERN_THRESH * 2 && !HasFailure(); ++i) {
    if (i < INTERN_THRESH - 1) {
      EXPECT_FALSE(si.idx(str, AuIntern::ByFrequency)) << "i = " << i;
      EXPECT_EQ(0, si.dict().size()) << "i = " << i;
    } else {
      EXPECT_TRUE(si.idx(str, AuIntern::ByFrequency));
      EXPECT_EQ(1, si.dict().size());
    }
  }
}

TEST(AuStringIntern, ReIndex) {
  AuStringIntern si(AuStringIntern::Config{1, 2, 10});
  auto &dict = si.dict();

  using namespace std::string_literals;
  si.idx("twice"s, AuIntern::ForceIntern); // idx 0
  si.idx("once"s, AuIntern::ForceIntern);  // idx 1
  si.idx("thrice"s, AuIntern::ForceIntern);// idx 2
  si.idx("twice"s, AuIntern::ForceIntern);
  si.idx("thrice"s, AuIntern::ForceIntern);
  si.idx("thrice"s, AuIntern::ForceIntern);

  EXPECT_EQ(3, dict.size());
  EXPECT_EQ("twice"s, dict[0]);
  EXPECT_EQ("once"s, dict[1]);
  EXPECT_EQ("thrice"s, dict[2]);

  EXPECT_EQ(1, si.reIndex(2));

  EXPECT_EQ(2, dict.size());
  EXPECT_EQ("thrice"s, dict[0]);
  EXPECT_EQ("twice"s, dict[1]);

  EXPECT_EQ(0, *si.idx("thrice"s, AuIntern::ForceIntern));
  EXPECT_EQ(1, *si.idx("twice"s, AuIntern::ForceIntern));

  si.idx("quadrice"s, AuIntern::ForceIntern);
  EXPECT_EQ(2, *si.idx("quadrice"s, AuIntern::ForceIntern));
}

// when a purge creates a divergence between dictionary_ size and dictInOrder_
// size, and then idx() hits the capacity-triggered doReIndex, the nextEntry
// variable was captured BEFORE doReIndex shrank dictInOrder_.
// This produced an internIndex exceeding the actual vector size, leading to
// out-of-bounds access on a subsequent doReIndex.
TEST(AuStringIntern, NextEntryValidAfterPurgeTriggeredDoReIndex) {
  // clearThreshold=20 → initial reserve = floor(20 * 1.2) = 24
  AuStringIntern si(AuStringIntern::Config{1, 1, 100, 20});
  auto &dict = si.dict();

  using namespace std::string_literals;

  // Fill 20 of the 24 capacity slots
  for (int i = 0; i < 20; i++) {
    si.idx("entry_" + std::to_string(i), AuIntern::ForceIntern);
  }
  ASSERT_EQ(20u, dict.size());

  // Bump occurrence counts on the first 15 entries so they survive purge
  for (int round = 0; round < 5; round++) {
    for (int i = 0; i < 15; i++) {
      si.idx("entry_" + std::to_string(i), AuIntern::ForceIntern);
    }
  }

  // Purge entries with < 2 occurrences (removes entries 15-19 from dictionary_)
  // After: dictionary_.size()=15, dictInOrder_.size()=20 (diverged!)
  si.purge(2);

  // Fill remaining capacity: 4 new entries bring dictInOrder_ to 24 (== capacity)
  // dictionary_ grows to 19, dictInOrder_ grows to 24
  for (int i = 20; i < 24; i++) {
    si.idx("entry_" + std::to_string(i), AuIntern::ForceIntern);
  }
  ASSERT_EQ(24u, dict.size());

  // This intern triggers the capacity-based doReIndex inside idx().
  // doReIndex rebuilds dictInOrder_ from dictionary_ (19 entries), shrinking it.
  // BUG: nextEntry was captured as 24 (pre-doReIndex dictInOrder_.size()),
  //      but the new entry is emplaced at index 19 (post-doReIndex size).
  //      The stored internIndex (24) exceeds dictInOrder_.size() (20).
  auto result = si.idx("trigger_string_xx"s, AuIntern::ForceIntern);
  ASSERT_TRUE(result.has_value());

  // The intern index must be a valid index into dict()
  EXPECT_LT(*result, dict.size())
      << "internIndex " << *result << " >= dict size " << dict.size()
      << ": nextEntry was stale after capacity-triggered doReIndex";

  // The string at the returned index must be our string
  if (*result < dict.size()) {
    EXPECT_EQ("trigger_string_xx"s, dict[*result]);
  }

  // A subsequent reIndex must not crash (it accesses dictInOrder_[internIndex]
  // for every entry; a stale index would be out of bounds here).
  si.reIndex(1);

  // After reindex, the string should still be findable
  auto result2 = si.idx("trigger_string_xx"s, AuIntern::ForceIntern);
  ASSERT_TRUE(result2.has_value());
  EXPECT_LT(*result2, dict.size());
  EXPECT_EQ("trigger_string_xx"s, dict[*result2]);
}

struct AuFormatterTest : public ::testing::Test {
  AuVectorBuffer buf;
  AuStringIntern stringIntern;
  AuWriter writer;

  AuFormatterTest() : writer(buf, stringIntern) {}
};

TEST_F(AuFormatterTest, Null) {
  writer.null();
  writer.value(nullptr);
  EXPECT_EQ("\x00\x00"sv, buf.str());
}

TEST_F(AuFormatterTest, Bool) {
  writer.value(true);
  writer.value(false);
  EXPECT_EQ("\x01\x02"sv, buf.str());
}

#define C(v) static_cast<char>(v)

TEST_F(AuFormatterTest, Int) {
  writer.value(0).value(127).value(128);
  writer.value(-1).value(-127).value(-128);
  writer.value(0xff).value(0x100);

  std::vector<char> ints = {
      // Small positives
      marker::SmallInt::Positive | 0u, // 0
      marker::Varint, C(127),           // 127
      marker::Varint, C(0x80), 0x01,    // 128
      // Small negatives
      marker::SmallInt::Negative | 1u, // -1
      marker::NegVarint, C(127),        // -127
      marker::NegVarint, C(0x80), 0x01, // -128
      // Larger positives
      marker::Varint, C(0xff), 0x01,    // 0xff
      marker::Varint, C(0x80), 0x02,    // 0x100
  };
  EXPECT_EQ(std::string(ints.data(), ints.size()), buf.str());
}

TEST_F(AuFormatterTest, Int64) {
  writer.value(0x1234567890abcdef);
  writer.value(-0x1234567890abcdef);
  writer.value(0xf234567890abcdef);
  writer.value(0xffffFFFFffffFFFF);
  uint64_t val = 9223372036856150856ull;
  writer.value((int64_t)val);
  writer.value(val);
  writer.value(std::numeric_limits<int64_t>::min());
  std::vector<char> ints = {
      marker::PosInt64,
      C(0xef), C(0xcd), C(0xab), C(0x90), C(0x78), C(0x56), C(0x34), C(0x12),
      marker::NegInt64,
      C(0xef), C(0xcd), C(0xab), C(0x90), C(0x78), C(0x56), C(0x34), C(0x12),
      marker::PosInt64,
      C(0xef), C(0xcd), C(0xab), C(0x90), C(0x78), C(0x56), C(0x34), C(0xf2),
      marker::PosInt64,
      C(0xff), C(0xff), C(0xff), C(0xff), C(0xff), C(0xff), C(0xff), C(0xff),
      marker::NegInt64,
      C(0xb8), C(0x04), C(0xeb), C(0xff), C(0xff), C(0xff), C(0xff), C(0x7f),
      marker::PosInt64,
      C(0x48), C(0xfb), C(0x14), C(0x00), C(0x00), C(0x00), C(0x00), C(0x80),
      marker::NegInt64,
      C(0x00), C(0x00), C(0x00), C(0x00), C(0x00), C(0x00), C(0x00), C(0x80)
  };
  EXPECT_THAT(std::string_view(ints.data(), ints.size()),
              testing::ContainerEq(buf.str()));
}

TEST_F(AuFormatterTest, Time) {
  using namespace std::chrono;
  std::string expected;
  std::chrono::system_clock::time_point tp;

  tp += seconds(35);
  writer.value(tp);
  expected += std::string("\x04\x00\x9e\x29\x26\x08\x00\x00\x00", 9);

  EXPECT_EQ(expected, buf.str());
}

TEST_F(AuFormatterTest, Double) {
  double d = 5.9;
  writer.value(d);
  EXPECT_EQ(std::string("\x03\x9A\x99\x99\x99\x99\x99\x17\x40"), buf.str());
}

TEST_F(AuFormatterTest, Float) {
  float f = 5.9;
  writer.value(f);
  EXPECT_EQ(std::string("\x03\0\0\0\xA0\x99\x99\x17\x40", 9), buf.str());
}

TEST_F(AuFormatterTest, NaN) {
  writer.array(
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<long double>::quiet_NaN(), // converted to double
      std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0)
  );

  std::vector<char> NaNs = {
      marker::ArrayStart,
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf8), C(0x7f),
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf8), C(0x7f),
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf8), C(0x7f),
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf8), C(0xff),
      marker::ArrayEnd
  };
  EXPECT_EQ(std::string(NaNs.data(), NaNs.size()), buf.str());
}

TEST_F(AuFormatterTest, Inf) {
  writer.array(
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity()
  );

  std::vector<char> Infs = {
      marker::ArrayStart,
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf0), C(0x7f),
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf0), C(0xff),
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf0), C(0x7f),
      marker::Double, 0, 0, 0, 0, 0, 0, C(0xf0), C(0xff),
      marker::ArrayEnd
  };
  EXPECT_EQ(std::string(Infs.data(), Infs.size()), buf.str());
}

TEST_F(AuFormatterTest, ShortString) {
  writer.value("str");
  std::vector<char> str = {0x20 | 3u, 's', 't', 'r'};
  EXPECT_EQ(std::string(str.data(), str.size()), buf.str());
}

TEST_F(AuFormatterTest, LongString) {
  writer.value("aLongerString, longer than 32 chars, the important thing");
  EXPECT_EQ(
      "\x05\x38"sv
      "aLongerString, longer than 32 chars, the important thing"sv,
      buf.str());
}

TEST_F(AuFormatterTest, InternString) {
  stringIntern.idx(std::string("aLongInternedString"), AuIntern::ForceIntern);
  stringIntern.idx(std::string("another string"), AuIntern::ForceIntern);

  writer.value("aLongInternedString", true);
  writer.value("another string", true);

  std::vector<char> str = {(char)(0x80u | 0u), (char)(0x80u | 1u)};
  EXPECT_EQ(std::string(str.data(), str.size()), buf.str());
}

TEST_F(AuFormatterTest, EmptyMap) {
  writer.map();
  EXPECT_EQ(std::string("\x0d\x0e"), buf.str());
}

TEST_F(AuFormatterTest, FlatMap) {
  writer.map("Key1", "value1", "key1", "Value1");
  EXPECT_EQ(std::string("\x0d\x24Key1\x26value1\x24key1\x26Value1\x0e"),
            buf.str());
}

TEST_F(AuFormatterTest, NestedMap) {
  writer.map("k1", "v1", "nested", writer.mapVals([&](auto &sink) {
    sink(std::string_view("k2"), "v2");
  }));
  EXPECT_EQ(std::string("\x0d\x22k1\x22v1\x80\x0d\x22k2\x22v2\x0e\x0e"),
            buf.str());
}

TEST_F(AuFormatterTest, EmptyArray) {
  writer.array();
  EXPECT_EQ(std::string("\x0b\x0c"), buf.str());
}

TEST_F(AuFormatterTest, FlatArray) {
  writer.array(1, 2, 3);
  EXPECT_EQ(std::string("\x0b\x61\x62\x63\x0c"), buf.str());
}

TEST_F(AuFormatterTest, NestedArray) {
  writer.array(1, 2, writer.arrayVals([&]() {
    writer.value(3).value(4);
  }));
  EXPECT_EQ(std::string("\x0b\x61\x62\x0b\x63\x64\x0c\x0c"), buf.str());
}

}
