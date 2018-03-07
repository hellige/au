#pragma once

#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// TODO disable rapidjson debug? is it just NDEBUG?

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
    OurWriter(Args &&... args)
    : Writer(std::forward<Args>(args)...) {}

    void Raw(std::string_view raw) {
      Prefix(rapidjson::kNullType);
      for (auto c : raw) os_->Put(c);
    }
  };
  OurWriter writer_;
  // TODO support arbitrary values in dictionary?
  std::vector<std::string> dictionary_; // TODO maybe a vector of string_view into a big buffer would be better
  bool dictAdd_;

public:
  JsonHandler()
  : buffer_(nullptr, 1<<16),
	writer_(buffer_),
	dictAdd_(false) {
	str_.reserve(1<<16);
	dictionary_.reserve(1<<16);
  }

  void onRecordEnd() {
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

  void onNull() { writer_.Null(); }
  void onBool(bool v) { writer_.Bool(v); }
  void onInt(int64_t v) { writer_.Int64(v); }
  void onUint(uint64_t v) { writer_.Uint64(v); }
  void onDouble(double v) {
    using namespace std::literals;
    if (std::isfinite(v)) writer_.Double(v);
    else if (std::isnan(v)) writer_.Raw("nan"s);
    else if (v < 0) writer_.Raw("-inf"s);
    else writer_.Raw("inf"s);
  }

  void onDictRef(size_t idx) {
	// TODO error handling, arbitary types? need to distinguish keys?
	const auto &v = dictionary_[idx];
	writer_.String(v.c_str(), v.size());
  }

  void onDictClear() {
	dictionary_.clear();
  }

  void onDictAddStart() {
    dictAdd_ = true;
  }

  void onDictAddEnd() {
    dictAdd_ = false;
  }

  void onStringStart(size_t len) {
	str_.clear();
	str_.reserve(len);
  }

  void onStringEnd() {
    if (dictAdd_)
    	dictionary_.emplace_back(str_.data(), str_.size());
    else
    	writer_.String(str_.data(), str_.size());
  }

  void onStringFragment(std::string_view frag) {
    // TODO is this the best way to do this?
    str_.insert(str_.end(), frag.data(), frag.data()+frag.size());
  }
};
