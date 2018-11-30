#pragma once

#include "au/AuDecoder.h"
#include "au/AuEncoder.h"
#include "Dictionary.h"
#include "AuRecordHandler.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace au {

class AuOutputHandler {
  AuEncoder encoder_;
  std::vector<char> str_;

  struct ValueHandler {
    AuWriter &writer_;
    std::vector<char> &str_;
    Dictionary::Dict &dictionary_;

    ValueHandler(AuWriter &writer,
                 std::vector<char> &str,
                 Dictionary::Dict &dictionary)
    : writer_(writer), str_(str), dictionary_(dictionary) {}

    void onObjectStart() { writer_.startMap(); }
    void onObjectEnd() { writer_.endMap(); }
    void onArrayStart() { writer_.startArray(); }
    void onArrayEnd() { writer_.endArray(); }
    void onNull(size_t) { writer_.null(); }
    void onBool(size_t, bool v) { writer_.value(v); }
    void onInt(size_t, int64_t v) { writer_.value(v); }
    void onUint(size_t, uint64_t v) { writer_.value(v); }
    void onDouble(size_t, double v) { writer_.value(v); }
    void onTime(size_t, std::chrono::system_clock::time_point nanos) {
      writer_.value(nanos);
    }
    void onDictRef(size_t, size_t idx) {
      const auto &v = dictionary_.at(idx);
      writer_.value(v);
    }
    void onStringStart(size_t, size_t len) {
      str_.clear();
      str_.reserve(len);
    }
    void onStringEnd() {
      writer_.value(std::string_view(str_.data(), str_.size()));
    }
    void onStringFragment(std::string_view frag) {
      str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
    }
  };

public:
  explicit AuOutputHandler(const std::string &metadata = "")
  : encoder_(metadata, 250'000, 100) {
    str_.reserve(1u << 16);
  }

  void onValue(AuByteSource &source, Dictionary::Dict &dictionary) {
    encoder_.encode([&] (AuWriter &writer) {
      ValueHandler handler(writer, str_, dictionary);
      ValueParser parser(source, handler);
      parser.value();
    }, [] (std::string_view dict, std::string_view value) {
      std::cout << dict << value; // TODO why use cout any longer?
      return dict.size() + value.size(); // TODO need to check whether it was really written?
    });
  }
};

}
