#pragma once

#include "au/AuDecoder.h"
#include "AuRecordHandler.h"
#include "GrepHandler.h"
#include "Tail.h"
#include "TimestampPattern.h"

#include <rapidjson/error/en.h>
#include <rapidjson/reader.h>

#include <cassert>
#include <chrono>
#include <optional>
#include <variant>

namespace au {

namespace {

template <typename Handler>
struct JsonSaxProxy
    : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>,
      JsonSaxProxy<Handler>> {
  Handler &handler;

  JsonSaxProxy(Handler &handler) : handler(handler) {}

  bool tryTime(const char *str, rapidjson::SizeType length) {
    using namespace std::chrono;
    auto result = parseTimestampPattern(std::string_view(str, length));
    if (result) {
      handler.onTime(0, result->first);
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
  Ch* PutBegin() { assert(false); return 0; }
  void Put(Ch) { assert(false); }
  void Flush() { assert(false); }
  size_t PutEnd(Ch*) { assert(false); return 0; }
};

template <typename OutputHandler>
int reallyDoJGrep(Pattern &pattern,
                  AuByteSource &source, OutputHandler &handler) {
  if (pattern.count) pattern.beforeContext = pattern.afterContext = 0;

  AuByteSourceStream wrappedSource(source);
  GrepHandler grepHandler(pattern);
  JsonSaxProxy proxy(grepHandler);
  try {
    std::vector<size_t> posBuffer;
    posBuffer.reserve(pattern.beforeContext+1);
    size_t force = 0;
    size_t total = 0;
    size_t matchPos = source.pos();
    size_t numMatches = std::numeric_limits<size_t>::max();
    if (pattern.numMatches) numMatches = *pattern.numMatches;
    size_t suffixLength = std::numeric_limits<size_t>::max();
    if (pattern.scanSuffixAmount) suffixLength = *pattern.scanSuffixAmount;

    rapidjson::Reader reader;
    while (source.peek() != EOF) {
      if (!force) {
        if (total >= numMatches) break;
        if (source.pos() - matchPos > suffixLength) break;
      }

      if (!pattern.count) {
        if (posBuffer.size() == pattern.beforeContext + 1)
          posBuffer.erase(posBuffer.begin());
      }
      posBuffer.push_back(source.pos());
      source.setPin(posBuffer.front());
      grepHandler.initializeForValue();
      static constexpr auto parseOpt = rapidjson::kParseStopWhenDoneFlag +
                                       rapidjson::kParseFullPrecisionFlag +
                                       rapidjson::kParseNanAndInfFlag;
      auto res = reader.Parse<parseOpt>(wrappedSource, proxy);
      if (!res) break; // TODO error here?
      if (grepHandler.matched() && total < numMatches) {
        matchPos = posBuffer.back();
        total++;
        if (pattern.count) continue;
        // this is a little tricky. this seek() might send us backward over a
        // number of records, which might cross over one or more dictionary
        // resets. but since we know we've been in sync up to this point, we
        // should always expect the needed dictionary to be within the last
        // few that we're keeping cached. so no dictionary rebuild will be
        // needed here, unless we seek backward over a large number of
        // dictionary resets (like, more than 32 according to the current code)
        source.seek(posBuffer.front());
        while (!posBuffer.empty()) {
          auto p2 = JsonSaxProxy(handler);
          handler.startJsonValue();
          reader.Parse<parseOpt>(wrappedSource, p2);
          handler.endJsonValue();
          posBuffer.pop_back();
        }
        source.clearPin();
        force = pattern.afterContext;
      } else if (force) {
        source.seek(posBuffer.back());
        auto p2 = JsonSaxProxy(handler);
        handler.startJsonValue();
        reader.Parse<parseOpt>(wrappedSource, p2);
        handler.endJsonValue();
        force--;
      }
    }

    if (pattern.count) {
      std::cout << total << std::endl;
    }
  } catch (parse_error &e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  return 0;
}

void seekJSync(AuByteSource &source, size_t pos) {
  source.seek(pos);
  if (!source.scanTo("\n")) {
    AU_THROW("Failed to find record at position " << pos);
  }
}

template <typename OutputHandler>
int doJBisect(Pattern &pattern, AuByteSource &source,
              OutputHandler &handler) {
  constexpr size_t SCAN_THRESHOLD = 256 * 1024;
  constexpr size_t PREFIX_AMOUNT = 512 * 1024;
  // it's important that the suffix amount be large enough to cover the entire
  // scan length + the prefix buffer. this is to guarantee that we will search
  // AT LEAST the entire scan region for the first match before giving up.
  // after finding the first match, we'll keep scanning until we go
  // SUFFIX_AMOUNT without seeing any matches. but we do want to make sure we
  // look for the first match in the entire region where it could possibly be
  // (and a bit beyond).
  constexpr size_t SUFFIX_AMOUNT = SCAN_THRESHOLD + PREFIX_AMOUNT + 266 * 1024;
  static_assert(SUFFIX_AMOUNT > PREFIX_AMOUNT + SCAN_THRESHOLD);

  if (!source.isSeekable()) {
    std::cerr << "Cannot binary search in non-seekable file '" << source.name()
      << "'" << std::endl;
    return -1;
  }

  Pattern bisectPattern(pattern);
  bisectPattern.matchOrGreater = true;

  AuByteSourceStream wrappedSource(source);
  GrepHandler grepHandler(bisectPattern);
  JsonSaxProxy proxy(grepHandler);
  rapidjson::Reader reader;

  try {
    size_t start = 0;
    size_t end = source.endPos();
    while (end > start) {
      if (end - start <= SCAN_THRESHOLD) {
        seekJSync(source, start > PREFIX_AMOUNT ? start - PREFIX_AMOUNT : 0);
        pattern.scanSuffixAmount = SUFFIX_AMOUNT;
        return reallyDoJGrep(pattern, source, handler);
      }

      size_t next = start + (end-start)/2;
      seekJSync(source, next);

      auto sor = source.pos();
      grepHandler.initializeForValue();
      static constexpr auto parseOpt = rapidjson::kParseStopWhenDoneFlag +
                                       rapidjson::kParseFullPrecisionFlag +
                                       rapidjson::kParseNanAndInfFlag;
      if (!reader.Parse<parseOpt>(wrappedSource, proxy))
        break;

      // the bisectPattern fails to match if the current record *strictly*
      // precedes any records matching the pattern (i.e., it matches any record
      // which is greater than or equal to the pattern). so we should eventually
      // find the approximate location of the first such record.
      if (grepHandler.matched()) {
        if (sor < end) {
          end = sor;
        } else {
          // this is an indication that we've jumped back to bisect the range
          // (start, end) but that in scanning forward to find the first record,
          // we ended up at or even past the end of the range. (basically, this
          // means the file contains a huge record.) if we update end
          // and bisect again, the same thing will happen again and we'll end
          // up doing this forever. in this case, we'll just set start and end
          // in such a way as to force a scan on the next iteration.
          end = start + 1;
        }
      } else {
        start = sor;
      }
    }
  } catch (parse_error &e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  return 0;
}

template <typename OutputHandler>
int doJGrep(Pattern &pattern, AuByteSource &source, OutputHandler &handler) {
  if (pattern.bisect) {
    return doJBisect(pattern, source, handler);
  }

  return reallyDoJGrep(pattern, source, handler);
}

}

}
