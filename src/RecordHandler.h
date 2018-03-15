#pragma once

#include "Dictionary.h"
#include "au/ParseError.h"

#include <vector>

class FileByteSource;

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

  void onHeader(uint64_t) {}

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

  void onStringStart(size_t, size_t len) {
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
