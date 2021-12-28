#include <gmock/gmock.h>

#include "au/BufferByteSource.h"
#include "au/helpers/KeyValueHandler.h"

TEST(HelpersTest, Builds) {
  au::BufferByteSource auBuf(nullptr, 0);
  au::KeyValueHandler handler(
    []([[maybe_unused]] const std::string &key,
       [[maybe_unused]] au::KeyValueHandler::ValType val) {
      ASSERT_TRUE(false);
    });
}
