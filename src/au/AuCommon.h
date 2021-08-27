#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace au {

using time_point =
  std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

namespace FormatVersion1 {

constexpr uint32_t AU_FORMAT_VERSION = 1;
constexpr size_t MAX_METADATA_SIZE = 16 * 1024;

}

namespace marker {

enum M {
  Null,
  True,
  False,
  Double,
  Timestamp,
  String,
  Varint,
  NegVarint,
  PosInt64,
  NegInt64,
  DictRef,
  ArrayStart,
  ArrayEnd,
  ObjectStart,
  ObjectEnd,
  RecordEnd
};

enum SmallInt : uint8_t {
  Positive = 0x60,
  Negative = 0x40
};

}

}
