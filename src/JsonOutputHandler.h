#pragma once

#include "AuDecoder.h"
#include "Dictionary.h"
#include "AuRecordHandler.h"

#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cmath>
#include <chrono>
#include <inttypes.h>
#include <iostream>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

// TODO disable rapidjson debug? is it just NDEBUG?

// TODO this whole file should be split up and rearranged.

class JsonOutputHandler {
  rapidjson::StringBuffer buffer_;
  std::vector<char> str_;
  struct RawDecode {
    typedef char Ch;
    static constexpr bool supportUnicode = false;
    template<typename InputStream>
    static bool Decode(InputStream &is, unsigned *codepoint) {
      unsigned char c = static_cast<unsigned char>(is.Take());
      *codepoint = c;
      return true;
    }
  };
  struct OurWriter : rapidjson::Writer<decltype(buffer_),
      RawDecode,
      rapidjson::ASCII<>> {
    template<typename... Args>
    explicit OurWriter(Args &&... args)
        : Writer(std::forward<Args>(args)...) {}

    void Raw(std::string_view raw) {
      Prefix(rapidjson::kNullType);
      for (auto c : raw) os_->Put(c);
    }
  };
  OurWriter writer_;
  Dictionary::Dict *dictionary_ = nullptr;

public:
  explicit JsonOutputHandler()
      : buffer_(nullptr, 1u << 16),
        writer_(buffer_) {
    str_.reserve(1u << 16);
  }

  void onValue(FileByteSource &source, Dictionary::Dict &dictionary) {
    dictionary_ = &dictionary;
    ValueParser<JsonOutputHandler> parser(source, *this);
    parser.value();
    // TODO this function is silly
    if (buffer_.GetSize()) {
      assert(writer_.IsComplete());
      std::cout
          << std::string_view(buffer_.GetString(), buffer_.GetSize())
          << std::endl;
    }
    // TODO doing this even if exceptions are thrown? bring some RAII?
    buffer_.Clear();
    writer_.Reset(buffer_);
  }

  void onObjectStart() { writer_.StartObject(); }
  void onObjectEnd() { writer_.EndObject(); }
  void onArrayStart() { writer_.StartArray(); }
  void onArrayEnd() { writer_.EndArray(); }

  // TODO must distinguish keys? doesn't look like it

  void onNull(size_t) { writer_.Null(); }
  void onBool(size_t, bool v) { writer_.Bool(v); }
  void onInt(size_t, int64_t v) { writer_.Int64(v); }
  void onUint(size_t, uint64_t v) { writer_.Uint64(v); }
  void onDouble(size_t, double v) {
    using namespace std::literals;
    if (std::isfinite(v)) writer_.Double(v);
    else if (std::isnan(v)) writer_.Raw("nan"sv);
    else if (v < 0) writer_.Raw("-inf"sv);
    else writer_.Raw("inf"sv);
  }

  void onTime(size_t, std::chrono::nanoseconds nanos) {
    using namespace std::chrono;
    using Clock = std::chrono::high_resolution_clock;

    auto s = duration_cast<seconds>(nanos); // Because to_time_t might round
    auto tp = time_point<Clock, seconds>(s);
    std::time_t tt = system_clock::to_time_t(tp);
    std::tm *tm = gmtime(&tt);

    //                   12345678901234567890123456
    char strTime[sizeof("yyyy-mm-ddThh:mm:ss.mmmuuu")];
    strftime(strTime, 21, "%FT%T.", tm);

    // Isolate the sub-second (fractional portion)
    auto micros = duration_cast<microseconds>(nanos - s);
    snprintf(strTime + 20, 7, "%06" PRIu64, micros.count());

    writer_.String(strTime,
                   static_cast<rapidjson::SizeType>(sizeof(strTime) - 1));
  }

  void onDictRef(size_t, size_t idx) {
    // TODO error handling, arbitary types? need to distinguish keys?
    const auto &v = dictionary_->at(idx);
    writer_.String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()));
  }

  void onStringStart(size_t, size_t len) {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() {
    writer_.String(str_.data(), static_cast<rapidjson::SizeType>(str_.size()));
  }

  void onStringFragment(std::string_view frag) {
    // TODO is this the best way to do this?
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
};
