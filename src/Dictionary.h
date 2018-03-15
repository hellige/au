#pragma once

#include <string>
#include <vector>

class Dictionary {
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