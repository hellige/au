#pragma once

#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "AuDecoder.h"

// TODO disable rapidjson debug? is it just NDEBUG?

// TODO this whole file should be split up and rearranged.

class Dictionary {
  // TODO support arbitrary values in dictionary?
  std::vector<std::string> dictionary_; // TODO maybe a vector of string_view into a big buffer would be better
  size_t lastDictPos_{};

public:
  Dictionary() {
    dictionary_.reserve(1u << 16u);
  }

  void add(size_t sor, std::string_view value) {
    dictionary_.emplace_back(value);
    lastDictPos_ = sor;
  }
//
//  void add(size_t sor, std::string &&value) {
//    dictionary_.emplace_back(value);
//    lastDictPos_ = sor;
//  }

  void clear(size_t sor) {
    dictionary_.clear();
    lastDictPos_ = sor;
  }

  size_t lastDictPos() const { return lastDictPos_; }
  const std::string &operator[](size_t idx) const {
    return dictionary_.at(idx);
  }
  bool valid(size_t dictPos) const { return dictPos == lastDictPos_; }
  size_t size() const {
    return dictionary_.size();
  }
};

template<typename ValueHandler>
class RecordHandler {
  Dictionary &dictionary_;
  ValueHandler &valueHandler_;
  std::vector<char> str_;
  size_t sor_;

public:
  RecordHandler(Dictionary &dictionary, ValueHandler &valueHandler)
      : dictionary_(dictionary), valueHandler_(valueHandler), sor_(0) {
    str_.reserve(1 << 16);
  }

  void onRecordStart(size_t pos) {
    sor_ = pos;
  }

  void onDictClear() {
    dictionary_.clear(sor_);
  }

  void onDictAddStart(size_t relDictPos) {
    if (!dictionary_.valid(sor_ - relDictPos))
      THROW("onDictAddStart wrong backref: "
                << sor_ << " " << relDictPos << " "
                << dictionary_.lastDictPos()); // TODO improve
  }

  void onValue(size_t relDictPos, size_t, FileByteSource &source) {
    if (!dictionary_.valid(sor_ - relDictPos))
      THROW("onValue wrong backref: sor = " << sor_ << " relDictPos = "
                                            << relDictPos
                                            << " lastDictPos = "
                                            << dictionary_.lastDictPos()); // TODO improve
    valueHandler_.onValue(source);
  }

  void onStringStart(size_t len) {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() {
    dictionary_.add(sor_, std::string_view(str_.data(), str_.size()));
  }

  void onStringFragment(std::string_view frag) {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }

  void onParseEnd() {}
};

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

  void onNull() { writer_.Null(); }
  void onBool(bool v) { writer_.Bool(v); }
  void onInt(int64_t v) { writer_.Int64(v); }
  void onUint(uint64_t v) { writer_.Uint64(v); }
  void onDouble(double v) {
    using namespace std::literals;
    if (std::isfinite(v)) writer_.Double(v);
    else if (std::isnan(v)) writer_.Raw("nan"sv);
    else if (v < 0) writer_.Raw("-inf"sv);
    else writer_.Raw("inf"sv);
  }

  void onDictRef(size_t idx) {
    // TODO error handling, arbitary types? need to distinguish keys?
    const auto &v = dictionary_[idx];
    writer_.String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()));
  }

  void onStringStart(size_t len) {
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
