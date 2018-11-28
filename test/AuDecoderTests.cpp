#include "JsonOutputHandler.h"

#include "gtest/gtest.h"

namespace au {

TEST(JsonOutputHandler, Time) {
  using namespace std::chrono;
  JsonOutputHandler json;
  json.onTime(0, system_clock::time_point() + nanoseconds(123'456'789));
  EXPECT_EQ(json.str(), R"("1970-01-01T00:00:00.123456789")");
}

}