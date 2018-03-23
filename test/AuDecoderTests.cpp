#include "JsonOutputHandler.h"

#include "gtest/gtest.h"

TEST(JsonOutputHandler, Time) {
  using namespace std::chrono;
  JsonOutputHandler json;
  json.onTime(0, nanoseconds(1'000'000));
  EXPECT_EQ(json.str(), R"("1970-01-01T00:00:00.001000")");
}