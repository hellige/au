#pragma once

#include "au/AuCommon.h"
#include "au/AuByteSource.h"
#include "au/Handlers.h"
#include "au/ParseError.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <iomanip>
#include <string>
#include <string_view>
#include <utility>
#include <cstddef>
#include <chrono>
#include <variant>

// TODO add position/expectation info to all error messages

namespace au {

class StringBuilder {
  std::string str_;
  size_t maxLen_;

public:
  StringBuilder(size_t maxLen) : maxLen_(maxLen) {}

  void onStringStart(size_t, size_t len) {
    if (len > maxLen_)
      throw std::length_error("String too long");
    str_.reserve(len);
  }
  void onStringFragment(std::string_view frag) {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
  void onStringEnd() {}

  const std::string &str() const { return str_; }
};

class BaseParser {
protected:
  static constexpr int AU_FORMAT_VERSION = FormatVersion1::AU_FORMAT_VERSION;

  AuByteSource &source_;

  explicit BaseParser(AuByteSource &source)
      : source_(source) {}

  void expect(char e) const {
    auto c = source_.next();
    if (c == e) return;
    AU_THROW("Unexpected character: " << c);
  }

  uint32_t readBackref() const {
    uint32_t val;
    source_.read<uint32_t>(&val, sizeof(val));
    return val;
  }

  double readDouble() const {
    double val;
    static_assert(sizeof(val) == 8, "sizeof(double) must be 8");
    source_.read(&val, sizeof(val));
    return val;
  }

  time_point readTime() const {
    uint64_t nanos;
    source_.read(&nanos, sizeof(nanos));
    std::chrono::nanoseconds n(nanos);
    return time_point() + n;
  }

  uint64_t readVarint() const {
    auto shift = 0u;
    uint64_t result = 0;
    while (true) {
      if (shift >= 64u)
        AU_THROW("Bad varint encoding");
      auto next = source_.next();
      if (next.isEof())
        AU_THROW("Unexpected end of file");
      auto i = next.byteValue();
      const auto valueMask = std::byte(0x7f);
      const auto moreMask = std::byte(0x80);
      result |= static_cast<uint64_t>(i & valueMask) << shift;
      shift += 7;
      if ((i & moreMask) != moreMask) break;
    }
    return result;
  }

  uint64_t parseFormatVersion() const {
    uint64_t version;
    auto c = source_.next();
    if ((c.charValue() & ~0x1f) == marker::SmallInt::Positive) {
      version = c.charValue() & 0x1f;
    } else if (c == marker::Varint) {
      version = readVarint();
    } else {
      AU_THROW("Expected version number");
    }

    // note: this would be one possible place to check that the format is one
    // of multiple supported versions, return the version number, and then
    // dispatch to one of several value parsers. i think that would currently
    // do the right thing for tail as well as for other use sites.

    if (version != AU_FORMAT_VERSION) {
      AU_THROW("Bad format version: expected " << AU_FORMAT_VERSION
                                            << ", got " << version);
    }
    return version;
  }

  template <typename Handler>
  void parseFullString(Handler &handler) const {
    size_t sov = source_.pos();
    auto c = source_.next();
    if ((c.uint8Value() & ~0x1fu) == 0x20) {
      parseString(sov, c.uint8Value() & 0x1fu, handler);
    } else if (c == marker::String) {
      parseString(sov, handler);
    } else {
      AU_THROW("Expected a string");
    }
  }

  template<typename Handler>
  void parseString(size_t pos, size_t len, Handler &handler) const {
    handler.onStringStart(pos, len);
    source_.readFunc(len, [&](std::string_view fragment) {
      handler.onStringFragment(fragment);
    });
    handler.onStringEnd();
  }

  template<typename Handler>
  void parseString(size_t pos, Handler &handler) const {
    auto len = readVarint();
    parseString(pos, len, handler);
  }

  void term() const {
    expect(marker::RecordEnd);
    expect('\n');
  }
};

struct TooDeeplyNested : std::runtime_error {
  TooDeeplyNested() : runtime_error("File too deeply nested") {}
};

template<typename Handler>
class ValueParser : BaseParser {
  Handler &handler_;
  /** A positive value that when multiplied by -1 represents the most negative
  number we support (std::numeric_limits<int64_t>::min() * -1). */
  static constexpr uint64_t NEG_INT_LIMIT =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;

  // TODO was 8192, reduced to avoid stack overflow with clang asan. it's known
  // that clang asan has ~3x stack overhead, so if this is ok, probably best
  // to put it back to 8192 by default and reduce it only for asan builds...
  static inline constexpr size_t MaxDepth = 2048;
  mutable size_t depth{0};
  struct DepthRaii {
    const ValueParser &parent;
    explicit DepthRaii(const ValueParser &vp) : parent(vp) {
      parent.depth++;
      if (parent.depth > MaxDepth)
        throw TooDeeplyNested();
    }
    ~DepthRaii() { parent.depth--; }
  };

public:
  ValueParser(AuByteSource &source, Handler &handler)
      : BaseParser(source), handler_(handler) {}

  void value() const {
    size_t sov = source_.pos();
    auto c = source_.next();
    if (c.isEof())
      AU_THROW("Unexpected EOF at start of value");
    if (c.charValue() & 0x80) {
      handler_.onDictRef(sov, c.uint8Value() & ~0x80u);
      return;
    }
    {
      auto val = c.uint8Value() & ~0xe0u;
      if (c.charValue() & marker::SmallInt::Negative) {
        if (c.charValue() & 0x20)
          handler_.onUint(sov, val);
        else
          handler_.onInt(sov, -static_cast<int>(val));
        return;
      }
      if (c.uint8Value() & 0x20) {
        parseString(sov, val, handler_);
        return;
      }
    }
    switch (c.charValue()) {
      case marker::True:
        handler_.onBool(sov, true);
        break;
      case marker::False:
        handler_.onBool(sov, false);
        break;
      case marker::Null:
        handler_.onNull(sov);
        break;
      case marker::Varint:
        handler_.onUint(sov, readVarint());
        break;
      case marker::NegVarint: {
        auto i = readVarint();
        if (i > NEG_INT_LIMIT) {
          AU_THROW("Signed int overflows int64_t: (-)" << i << " 0x"
                << std::setfill('0') << std::setw(16) << std::hex << i);
        }
        handler_.onInt(sov, -static_cast<int64_t>(i));
        break;
      }
      case marker::PosInt64: {
        uint64_t val;
        source_.read(&val, sizeof(val));
        handler_.onUint(sov, val);
        break;
      }
      case marker::NegInt64: {
        uint64_t val;
        source_.read(&val, sizeof(val));
        if (val > NEG_INT_LIMIT) {
          AU_THROW("Signed int overflows int64_t: (-)" << val << " 0x"
                << std::setfill('0') << std::setw(16) << std::hex << val);
        }
        handler_.onInt(sov, -static_cast<int64_t>(val));
        break;
      }
      case marker::Double:
        handler_.onDouble(sov, readDouble());
        break;
      case marker::Timestamp:
        handler_.onTime(sov, readTime());
        break;
      case marker::DictRef:
        handler_.onDictRef(sov, readVarint());
        break;
      case marker::String:
        parseString(sov, handler_);
        break;
      case marker::ArrayStart:
        parseArray();
        break;
      case marker::ObjectStart:
        parseObject();
        break;
      default:
        AU_THROW("Unexpected character at start of value: " << c);
    }
  }

private:
  void key() const {
    size_t sov = source_.pos();
    auto c = source_.next();
    if (c.isEof())
      AU_THROW("Unexpected EOF at start of key");
    if (c.charValue() & 0x80) {
      handler_.onDictRef(sov, c.uint8Value() & ~0x80u);
      return;
    }
    auto val = c.uint8Value() & ~0xe0u;
    if ((c.uint8Value() & ~0x1f) == 0x20) {
      parseString(sov, val, handler_);
      return;
    }
    switch (c.charValue()) {
      case marker::DictRef:
        handler_.onDictRef(sov, readVarint());
        break;
      case marker::String:
        parseString(sov, handler_);
        break;
      default:
        AU_THROW("Unexpected character at start of key: " << c);
    }
  }

  void parseArray() const {
    DepthRaii raii(*this);
    handler_.onArrayStart();
    while (source_.peek() != marker::ArrayEnd) value();
    expect(marker::ArrayEnd);
    handler_.onArrayEnd();
  }

  void parseObject() const {
    DepthRaii raii(*this);
    handler_.onObjectStart();
    while (source_.peek() != marker::ObjectEnd) {
      key();
      value();
    }
    expect(marker::ObjectEnd);
    handler_.onObjectEnd();
  }
};

template<typename Handler>
class RecordParser : BaseParser {
  Handler &handler_;

public:
  RecordParser(AuByteSource &source, Handler &handler)
      : BaseParser(source), handler_(handler) {}

  void parseStream(bool expectHeader = true) const {
    if (expectHeader) checkHeader();
    while (source_.peek() != EOF) record();
  }

  bool parseUntilValue() {
    while (source_.peek() != EOF)
      if (record()) return true;
    return false;
  }

  bool record() const {
    auto c = source_.next();
    if (c.isEof()) AU_THROW("Unexpected EOF at start of record");
    handler_.onRecordStart(source_.pos() - 1);
    switch (c.charValue()) {
      case 'H': {   // Header / metadata
        expect('A');
        expect('U');
        auto version = parseFormatVersion();
        StringBuilder sb(FormatVersion1::MAX_METADATA_SIZE);
        parseFullString(sb);
        handler_.onHeader(version, sb.str());
        term();
        break;
      }
      case 'C':     // Clear dictionary
        parseFormatVersion();
        term();
        handler_.onDictClear();
        break;
      case 'A': {   // Add dictionary entry
        auto backref = readBackref();
        handler_.onDictAddStart(backref);
        while (source_.peek() != marker::RecordEnd)
          parseFullString(handler_);
        term();
        break;
      }
      case 'V': {   // Add value
        auto backref = readBackref();
        auto len = readVarint();
        auto startOfValue = source_.pos();
        handler_.onValue(backref, len - 2, source_);
        term();
        if (source_.pos() - startOfValue != len)
          AU_THROW("could be a parse error, or internal error: value handler "
                "didn't skip value!");
        return true;
      }
      default:
        AU_THROW("Unexpected character at start of record: " << c);
    }
    return false;
  }

private:
  struct HeaderHandler : NoopRecordHandler {
    bool headerSeen = false;
    void onHeader(uint64_t, const std::string &) override {
      headerSeen = true;
    }
  };

  void checkHeader() const {
    // this is a special case. empty files should be considered ok, even
    // though they don't have a header/magic bytes, etc...
    if (source_.peek() == EOF) return;
    HeaderHandler hh;
    try {
      RecordParser<HeaderHandler>(source_, hh).record();
    } catch (const au::parse_error &) {
      // don't care what it was...
    }
    if (!hh.headerSeen)
      AU_THROW("This file doesn't appear to start with an au header record");
  }
};

template<typename Handler>
ValueParser(AuByteSource &source, Handler &handler) -> ValueParser<Handler>;
template<typename Handler>
RecordParser(AuByteSource &source, Handler &handler) -> RecordParser<Handler>;

}
