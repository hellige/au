#pragma once

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
}
