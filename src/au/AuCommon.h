#pragma once
#include <cstdint>

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
