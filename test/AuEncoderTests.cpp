#include "au/AuEncoder.h"
#include "au/BufferByteSource.h"
#include "JsonOutputHandler.h"

#include "gtest/gtest.h"

#include <vector>

namespace au {

struct AuEncoderTest : public ::testing::Test {
  AuEncoder au;
  std::vector<char> storage;
  static AuEncoderTest *test;

  AuEncoderTest() { test = this; }

  size_t store(std::string_view s1, std::string_view s2) {
    storage.insert(storage.end(), s1.begin(), s1.end());
    storage.insert(storage.end(), s2.begin(), s2.end());
    return s1.size() + s2.size();
  }

  static size_t write(std::string_view s1, std::string_view s2) {
    return (AuEncoderTest::test->store)(s1, s2);
  }

  /** Encodes the contents of the Au buffer as JSON and returns it.
   * @param stripEndl If true, trip the "\n" from the end of the result string.
   * @return
   */
  std::string getJson(bool stripEndl = true) {
    std::stringstream ss;
    JsonOutputHandler handler(ss);
    Dictionary dictionary;
    AuRecordHandler recordHandler(dictionary, handler);
    BufferByteSource source(storage.data(), storage.size());
    RecordParser(source, recordHandler).parseStream();
    if (stripEndl) {
      auto res = ss.str();
      return res.substr(0, res.size() - 1);
    }
    return ss.str();
  }

  void printJson() {
    std::cout << getJson();
  }

  void dumpHex() {
    std::cout << std::hex;
    for (auto c : storage) {
      std::cout << std::setw(2) << std::setfill('0') << +c << " ";
    }
    std::cout << "\n";
  }
};

AuEncoderTest *AuEncoderTest::test;

TEST_F(AuEncoderTest, creation) {
  AuEncoder au;
}

TEST_F(AuEncoderTest, smallInt) {
  au.encode([](AuWriter &writer) {
    writer.value(2);
  }, AuEncoderTest::write);
  ASSERT_EQ("2", getJson());
  dumpHex();
}

TEST_F(AuEncoderTest, smallNegInt) {
  auto exponent = [] () -> std::int8_t {
    return -9;
  };
  au.encode([&](AuWriter &writer) {
    writer.value(exponent());
  }, AuEncoderTest::write);
  ASSERT_EQ("-9", getJson());
}

TEST_F(AuEncoderTest, bigNegInt) {
  auto exponent = [] () -> std::int32_t {
    return -99999;
  };
  au.encode([&](AuWriter &writer) {
    writer.value(exponent());
  }, AuEncoderTest::write);
  ASSERT_EQ("-99999", getJson());
}

TEST_F(AuEncoderTest, bigInt) {
  au.encode([](AuWriter &writer) {
    writer.value(299792458);
  }, AuEncoderTest::write);
  ASSERT_EQ("299792458", getJson());
}

TEST_F(AuEncoderTest, array1) {
  au.encode([](AuWriter &writer) {
    writer.array(1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144);
  }, AuEncoderTest::write);
  ASSERT_EQ("[1,1,2,3,5,8,13,21,34,55,89,144]", getJson());
}

TEST_F(AuEncoderTest, MixedObjectValue) {
  std::vector<int> vec{1, 2, 3, 5, 7};
  au.encode(
    [&](AuWriter &writer) {
      writer.map(
          "SimpleKey", "AStringValue",
          "listOfNumbers", writer.arrayVals([&]() {
            for (auto val : vec) {
              writer.value(val);
            }
          })
      );
    }, AuEncoderTest::write);

  ASSERT_EQ(R"_({"SimpleKey":"AStringValue","listOfNumbers":[1,2,3,5,7]})_",
      getJson());
  //printJson();
}

TEST_F(AuEncoderTest, MixedObjectValue2) {
  au.encode(
      [&](AuWriter &writer) {
        writer.mapVals([&](auto &sink) {

          sink("SimpleKey", "AStringValue");
          sink("Numeric", 42);

        })
        (); // mapVals returns a function that needs to be called...
      }, AuEncoderTest::write);

  ASSERT_EQ(R"_({"SimpleKey":"AStringValue","Numeric":42})_",
            getJson());
  //printJson();
}

TEST_F(AuEncoderTest, MixedObjectMap) {
  std::vector<int> vec{1, 5, 7};
  au.encode(
      [&](AuWriter &writer) {
        writer.map(
            "SimpleKey", "AStringValue",
            "listOfObjects", writer.arrayVals([&]() {
              for (auto val : vec) {
                writer.map("val", val);
              }
            })
        );
      }, AuEncoderTest::write);

  ASSERT_EQ(R"_({"SimpleKey":"AStringValue","listOfObjects":[{"val":1},{"val":5},{"val":7}]})_",
            getJson());
  //printJson();
}

TEST_F(AuEncoderTest, MultiRecord) {
  au.encode([&](AuWriter &writer) {
    writer.map("1st", "record", "key", 3.141);
  }, AuEncoderTest::write);
  au.encode([&](AuWriter &writer) {
    writer.map("2nd", "record", "transcends", 2.71828);
  }, AuEncoderTest::write);
  ASSERT_EQ(R"_({"1st":"record","key":3.141})_" "\n"
            R"_({"2nd":"record","transcends":2.71828})_", getJson());
}

}
