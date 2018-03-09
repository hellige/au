#include "AuEncoder.h"

#include "gtest/gtest.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

TEST(AuStringIntern, NoIntern) {
  AuStringIntern si;
  EXPECT_EQ(0, si.dict().size());
  EXPECT_FALSE(si.idx(std::string("shrt"), std::optional<bool>()));
  EXPECT_FALSE(si.idx(std::string("Long string"), std::optional<bool>()));
  EXPECT_EQ(0, si.dict().size());
}

TEST(AuStringIntern, ForceIntern) {
  AuStringIntern si;
  EXPECT_EQ(0, si.dict().size());

  // Tiny strings are not interned even if forced
  EXPECT_FALSE(si.idx(std::string("tiny"), true));
  EXPECT_EQ(0, si.dict().size());

  EXPECT_TRUE(si.idx(std::string("A normal string"), true));
  EXPECT_EQ(1, si.dict().size());
}

TEST(AuStringIntern, InternFrequentStrings) {
  constexpr size_t INTERN_THRESH = 10;
  AuStringIntern si(4, INTERN_THRESH, 1000);
  std::string str("Normal value");

  EXPECT_FALSE(si.idx(str, std::optional<bool>()));
  EXPECT_EQ(0, si.dict().size());

  for (size_t i = 0; i < INTERN_THRESH * 2 && !HasFailure(); ++i) {
    if (i < INTERN_THRESH - 1) {
      EXPECT_FALSE(si.idx(str, std::optional<bool>())) << "i = " << i;
      EXPECT_EQ(0, si.dict().size()) << "i = " << i;
    } else {
      EXPECT_TRUE(si.idx(str, std::optional<bool>()));
      EXPECT_EQ(1, si.dict().size());
    }
  }
}

struct AuFormatterTest : public ::testing::Test {
  std::ostringstream os;
  AuStringIntern stringIntern;
  AuFormatter formatter;

  AuFormatterTest() : formatter(os, stringIntern) {}
};

TEST_F(AuFormatterTest, Null) {
  formatter.null();
  formatter.value(nullptr);
  EXPECT_EQ(std::string("NN"), os.str());
}

TEST_F(AuFormatterTest, Bool) {
  formatter.value(true);
  formatter.value(false);
  EXPECT_EQ(std::string("TF"), os.str());
}

#define C(v) static_cast<char>(v)

TEST_F(AuFormatterTest, Int) {
  formatter.value(0).value(127).value(128);
  formatter.value(-1).value(-127).value(-128);
  formatter.value(0xff).value(0x100);

  std::vector<char> ints = {
      // Small positives
      'I', 0,             // 0
      'I', C(127),        // 127
      'I', C(0x80), 0x01, // 128
      // Small negatives
      'J', 1,             // -1
      'J', C(127),        // -127
      'J', C(0x80), 0x01, // -128
      // Larger positives
      'I', C(0xff), 0x01, // 0xff
      'I', C(0x80), 0x02, // 0x100
  };
  EXPECT_EQ(std::string(ints.data(), ints.size()), os.str());
}

TEST_F(AuFormatterTest, Int64) {
  formatter.value(0x1234567890abcdef);
  formatter.value(-0x1234567890abcdef);
  formatter.value(0xf234567890abcdef);
  formatter.value(0xffffFFFFffffFFFF);
  std::vector<char> ints = {
      'I', C(0xef), C(0x9b), C(0xaf), C(0x85), C(0x89), C(0xcf), C(0x95),
      C(0x9a), C(0x12),
      'J', C(0xef), C(0x9b), C(0xaf), C(0x85), C(0x89), C(0xcf), C(0x95),
      C(0x9a), C(0x12),
      'I', C(0xef), C(0x9b), C(0xaf), C(0x85), C(0x89), C(0xcf), C(0x95),
      C(0x9a), C(0xf2), C(0x01),
      'I', C(0xff), C(0xff), C(0xff), C(0xff), C(0xff), C(0xff), C(0xff),
      C(0xff), C(0xff), C(0x01)
  };
  EXPECT_EQ(std::string(ints.data(), ints.size()), os.str());
}

TEST_F(AuFormatterTest, Double) {
  double d = 5.9;
  formatter.value(d);
  EXPECT_EQ(std::string("D\x9A\x99\x99\x99\x99\x99\x17\x40"), os.str());
}

TEST_F(AuFormatterTest, Float) {
  float f = 5.9;
  formatter.value(f);
  EXPECT_EQ(std::string("D\0\0\0\xA0\x99\x99\x17\x40", 9), os.str());
}

TEST_F(AuFormatterTest, NaN) {
  formatter.array(
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<long double>::quiet_NaN(), // converted to double
      0 / 0.0,
      std::sqrt(-1)
  );

  std::vector<char> NaNs = { '[',
                             'D', 0, 0, 0, 0, 0, 0, C(0xf8), C(0x7f),
                             'D', 0, 0, 0, 0, 0, 0, C(0xf8), C(0x7f),
                             'D', 0, 0, 0, 0, 0, 0, C(0xf8), C(0x7f),
                             'D', 0, 0, 0, 0, 0, 0, C(0xf8), C(0xff),
                             'D', 0, 0, 0, 0, 0, 0, C(0xf8), C(0xff),
                             ']'
  };
  EXPECT_EQ(std::string(NaNs.data(), NaNs.size()), os.str());
}

TEST_F(AuFormatterTest, Inf) {
  formatter.array(
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity()
  );

  std::vector<char> Infs = { '[',
                             'D', 0, 0, 0, 0, 0, 0, C(0xf0), C(0x7f),
                             'D', 0, 0, 0, 0, 0, 0, C(0xf0), C(0xff),
                             'D', 0, 0, 0, 0, 0, 0, C(0xf0), C(0x7f),
                             'D', 0, 0, 0, 0, 0, 0, C(0xf0), C(0xff),
                             ']'
  };
  EXPECT_EQ(std::string(Infs.data(), Infs.size()), os.str());
}

TEST_F(AuFormatterTest, ShortString) {
  formatter.value("str");
  std::vector<char> str = {'S', 0x03, 's', 't', 'r'};
  EXPECT_EQ(std::string(str.data(), str.size()), os.str());
}

TEST_F(AuFormatterTest, LongString) {
  formatter.value("aLongerString");
  std::vector<char> str = {'S', 13, 'a', 'L', 'o', 'n', 'g', 'e', 'r', 'S', 't',
                           'r', 'i', 'n', 'g'};
  EXPECT_EQ(std::string(str.data(), str.size()), os.str());
}

TEST_F(AuFormatterTest, InternString) {
  stringIntern.idx(std::string("aLongInternedString"), true);
  stringIntern.idx(std::string("another string"), true);

  formatter.value("aLongInternedString", true);
  formatter.value("another string", true);

  EXPECT_EQ(std::string("X\0X\1", 4), os.str());
}

TEST_F(AuFormatterTest, EmptyMap) {
  formatter.map();
  EXPECT_EQ(std::string("{}"), os.str());
}

TEST_F(AuFormatterTest, FlatMap) {
  formatter.map("Key1", "value1", "key1", "Value1");
  EXPECT_EQ(std::string("{S\x04Key1S\x06value1S\04key1S\x06Value1}"), os.str());
}

TEST_F(AuFormatterTest, NestedMap) {
  formatter.map("k1", "v1", "nested", formatter.mapVals([&](auto &sink) {
    sink(std::string_view("k2"), "v2");
  }));
  EXPECT_EQ(std::string("{S\2k1S\2v1X\0{S\2k2S\2v2}}", 22), os.str());
}

TEST_F(AuFormatterTest, EmptyArray) {
  formatter.array();
  EXPECT_EQ(std::string("[]"), os.str());
}

TEST_F(AuFormatterTest, FlatArray) {
  formatter.array(1, 2, 3);
  EXPECT_EQ(std::string("[I\1I\2I\3]"), os.str());
}

TEST_F(AuFormatterTest, NestedArray) {
  formatter.array(1, 2, formatter.arrayVals([&]() {
    formatter.value(3).value(4);
  }));
  EXPECT_EQ(std::string("[I\1I\2[I\3I\4]]"), os.str());
}

TEST(Au, creation) {
  std::ostringstream os;
  Au au(os);
}

