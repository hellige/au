#pragma once

#include "AuDecoder.h"
#include "Dictionary.h"
#include "RecordHandler.h"

#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// TODO disable rapidjson debug? is it just NDEBUG?

// TODO this whole file should be split up and rearranged.

class JsonHandler {
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
  Dictionary &dictionary_;

public:
  explicit JsonHandler(Dictionary &dictionary)
      : buffer_(nullptr, 1u << 16),
        writer_(buffer_),
        dictionary_(dictionary) {
    str_.reserve(1u << 16);
  }

  void onValue(FileByteSource &source) {
    ValueParser<JsonHandler> parser(source, *this);
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

  void onDictRef(size_t, size_t idx) {
    // TODO error handling, arbitary types? need to distinguish keys?
    const auto &v = dictionary_[idx];
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
