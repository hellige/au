#pragma once

#include "au/AuDecoder.h"
#include "Dictionary.h"
#include "AuRecordHandler.h"

#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

class JsonOutputHandler {
  std::ostream &out;
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
  explicit JsonOutputHandler(std::ostream &out = std::cout)
      : out(out),
        buffer_(nullptr, 1u << 16),
        writer_(buffer_) {
    str_.reserve(1u << 16);
  }

  void onValue(AuByteSource &source, Dictionary::Dict &dictionary) {
    buffer_.Clear();
    writer_.Reset(buffer_);
    dictionary_ = &dictionary;
    ValueParser<JsonOutputHandler> parser(source, *this);
    parser.value();
    if (!writer_.IsComplete()) {
      AU_THROW("rapidjson writer does not report a complete value after parse of"
            " au value!");
    }
    if (buffer_.GetSize()) {
      out
          << std::string_view(buffer_.GetString(), buffer_.GetSize())
          << std::endl;
    }
  }

  void onObjectStart() { writer_.StartObject(); }
  void onObjectEnd() { writer_.EndObject(); }
  void onArrayStart() { writer_.StartArray(); }
  void onArrayEnd() { writer_.EndArray(); }
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

  void onTime(size_t, std::chrono::system_clock::time_point timestamp) {
    using namespace std::chrono;
    using Clock = std::chrono::high_resolution_clock;

    auto nanos = timestamp.time_since_epoch();
    auto s = duration_cast<seconds>(nanos); // Because to_time_t might round
    auto tp = time_point<Clock, seconds>(s);
    std::time_t tt = system_clock::to_time_t(tp);
    std::tm *tm = gmtime(&tt);

    //                   12345678901234567890123456
    char strTime[sizeof("yyyy-mm-ddThh:mm:ss.mmmuuunnn")];
    strftime(strTime, 21, "%FT%T.", tm);

    // Isolate the sub-second (fractional portion)
    auto fraction = duration_cast<nanoseconds>(nanos - s);
    snprintf(strTime + 20, 10, "%09" PRIu64, fraction.count());
    writer_.String(strTime,
                   static_cast<rapidjson::SizeType>(sizeof(strTime) - 1));
  }

  void onDictRef(size_t, size_t idx) {
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
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }

  std::string str() {
    return std::string(buffer_.GetString(), buffer_.GetSize());
  }
};
