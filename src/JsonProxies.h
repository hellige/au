#pragma once

#include "TimestampPattern.h"

#include <rapidjson/reader.h>

namespace au {

namespace {

template <typename Handler>
struct JsonSaxProxy
    : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>,
      JsonSaxProxy<Handler>> {
  Handler &handler;

  JsonSaxProxy(Handler &handler) : handler(handler) {}

  bool tryTime(const char *str, rapidjson::SizeType length) {
    auto result = parseTimestampPattern(std::string_view(str, length));
    if (result) {
      handler.onTime(0, result->start);
      return true;
    }
    return false;
  }

  bool Null() { handler.onNull(0); return true; }
  bool Bool(bool b) { handler.onBool(0, b); return true; }
  bool Int(int i) { handler.onInt(0, i); return true; }
  bool Uint(unsigned u) { handler.onUint(0, u); return true; }
  bool Int64(int64_t i) { handler.onInt(0, i); return true; }
  bool Uint64(uint64_t u) { handler.onUint(0, u); return true; }
  bool Double(double d) { handler.onDouble(0, d); return true; }

  bool String(const char *str, rapidjson::SizeType length, [[maybe_unused]] bool copy) {
    constexpr size_t MAX_TIMESTAMP_LEN =
        sizeof("yyyy-mm-ddThh:mm:ss.mmmuuunnn") - 1;
    if (length == MAX_TIMESTAMP_LEN
               || length == MAX_TIMESTAMP_LEN - 3
               || length == MAX_TIMESTAMP_LEN - 6
               || length == MAX_TIMESTAMP_LEN - 10) {
      // try times with ms, us, ns or just seconds...
      if (tryTime(str, length)) return true;
    }
    handler.onStringStart(0, length);
    handler.onStringFragment(std::string_view(str, length));
    handler.onStringEnd();
    return true;
  }

  bool StartObject() {
    handler.onObjectStart();
    return true;
  }

  bool EndObject([[maybe_unused]] rapidjson::SizeType memberCount) {
    handler.onObjectEnd();
    return true;
  }

  bool StartArray() {
    handler.onArrayStart();
    return true;
  }

  bool EndArray([[maybe_unused]] rapidjson::SizeType elementCount) {
    handler.onArrayEnd();
    return true;
  }

  bool Key(const char *str, rapidjson::SizeType length, [[maybe_unused]] bool copy) {
    handler.onStringStart(0, length);
    handler.onStringFragment(std::string_view(str, length));
    handler.onStringEnd();
    return true;
  }
};

template <typename Handler>
JsonSaxProxy(Handler &handler) -> JsonSaxProxy<Handler>;

struct AuByteSourceStream {
  typedef char Ch;
  AuByteSource &source;

  AuByteSourceStream(AuByteSource &source) : source(source) {}

  Ch Peek() const {
    auto c = source.peek();
    if (!c.isEof()) return c.charValue();
    return 0;
  }
  Ch Take() { return source.next().charValue(); }
  size_t Tell() const { return source.pos(); }

  // rapidjson requires these for compilation, but won't call them.
  Ch* PutBegin() { assert(false); return nullptr; }
  void Put(Ch) { assert(false); }
  void Flush() { assert(false); }
  size_t PutEnd(Ch*) { assert(false); return 0; }
};

}

}
