#pragma once

#include "Dictionary.h"
#include "au/ParseError.h"

#include <vector>

class FileByteSource;

template<typename ValueHandler>
class AuRecordHandler {
  Dictionary &dictionary_;
  ValueHandler &valueHandler_;
  std::vector<char> str_;
  size_t sor_ = 0;
  Dictionary::Dict *dict_ = nullptr;

public:
  AuRecordHandler(Dictionary &dictionary, ValueHandler &valueHandler)
      : dictionary_(dictionary), valueHandler_(valueHandler) {
    str_.reserve(1 << 16);
  }

  void onRecordStart(size_t pos) {
    sor_ = pos;
  }

  void onHeader(uint64_t, const std::string &) {}

  void onDictClear() {
    dictionary_.clear(sor_);
  }

  void onDictAddStart(size_t relDictPos) {
    auto &dictionary = dictionary_.findDictionary(sor_, relDictPos);
    if (!dictionary.includes(sor_))
      dict_ = &dictionary;
  }

  void onValue(size_t relDictPos, size_t, FileByteSource &source) {
    auto &dictionary = dictionary_.findDictionary(sor_, relDictPos);
    valueHandler_.onValue(source, dictionary);
  }

  void onStringStart(size_t, size_t len) {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() {
    if (dict_) dict_->add(sor_, std::string_view(str_.data(), str_.size()));
  }

  void onStringFragment(std::string_view frag) {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
};
