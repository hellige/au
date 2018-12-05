#pragma once

#include "au/ParseError.h"

#include <string>
#include <vector>

namespace au {

class Dictionary {
public:
  struct Dict {
    std::vector<std::string> dictionary_;
    size_t startPos_;
    size_t lastDictPos_;

    Dict(size_t startPos)
    : startPos_(startPos),
      lastDictPos_(startPos) {
      dictionary_.reserve(1u << 16u);
    }

    Dict(const Dict &) = delete;
    Dict &operator=(const Dict &) = delete;

    void reset(size_t sor) {
      dictionary_.clear();
      startPos_ = sor;
      lastDictPos_ = sor;
    }

    void add(size_t sor, std::string_view value) {
      dictionary_.emplace_back(value);
      lastDictPos_ = sor;
    }

    bool includes(size_t sor) const {
      return startPos_ <= sor && sor <= lastDictPos_;
    }

    const std::string &at(size_t idx) const {
      if (idx >= dictionary_.size()) {
        AU_THROW("Dictionary reference index "
                  << idx << " out of range. Dictionary started at position "
                  << startPos_ << ", last add occurred at position "
                  << lastDictPos_ << ", and currently has "
                  << dictionary_.size() << " entries.");
      }
      return dictionary_.at(idx);
    }
    const std::vector<std::string> &entries() const { return dictionary_; }
    size_t size() const { return dictionary_.size(); }
  };

private:
  // used as sort of a really dumb lru-cache
  std::vector<std::unique_ptr<Dict>> dictionaries_;
  uint32_t maxDicts_;

public:
  Dictionary(uint32_t maxDicts = 1)
  : maxDicts_(maxDicts) {
    dictionaries_.reserve(maxDicts_);
  }

  Dict &clear(size_t sor) {
    {
      Dict *dict = search(sor);

      if (dict)
      {
        if (dict->startPos_ == sor)
          return *dict;
        AU_THROW("dictionary mismatch. dict-clear at "
                 << sor << " appears to be within valid range of dictionary "
                 "starting at " << dict->startPos_
                 << ", last dict pos " << dict->lastDictPos_);
      }
    }

    if (dictionaries_.size() == maxDicts_) {
      std::unique_ptr<Dict> recycle(std::move(dictionaries_.front()));
      dictionaries_.erase(dictionaries_.begin());
      recycle->reset(sor);
      dictionaries_.emplace_back(std::move(recycle));
    } else {
      dictionaries_.emplace_back(new Dict(sor));
    }
    return *dictionaries_.back();
  }

  Dict &findDictionary(size_t sor, size_t relDictPos) {
    auto pos = sor - relDictPos;
    Dict *dict = search(pos);

    if (!dict)
      AU_THROW("wrong backref: no dictionary includes absolute position = "
                << pos << ": start-of-record = " << sor
                << " relDictPos = " << relDictPos);

    return *dict;
  }

  Dict *latest() {
    if (dictionaries_.empty()) return nullptr;
    return dictionaries_.back().get();
  }

  Dict *search(size_t pos) {
    // usually the one we want is the most recently added one... the other
    // case is something like a bisect, in which case we don't mind scanning.
    for (auto i = dictionaries_.size(); i-- > 0;) {
      if (dictionaries_[i]->includes(pos))
        return dictionaries_[i].get();
    }
    return nullptr;
  }
};

}
