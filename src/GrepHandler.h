#pragma once

#include "AuDecoder.h"

#include <algorithm>
#include <stack>

/**
 * This ValueHandler looks for specific patterns, and if the pattern is found,
 * rewinds the data stream to the start of the record, then delegates to another
 * ValueHandler (the OutputHandler) to output the matched record.
 *
 * @tparam OutputHandler A ValueHandler to delegate matching records to.
 */
template<typename OutputHandler>
class GrepHandler : public NoopValueHandler {
  OutputHandler &handler_;
  Dictionary &dictionary_;

  // Patterns to search for
  const std::vector<std::string> &pKey_;
  const std::vector<uint64_t> &pUint64_t_;
  const std::vector<int64_t> &pInt64_t_;
  const std::vector<std::string> &pFullStr_;

  std::vector<char> str_;
  bool matched_;

  // Keeps track of the context we're in so we know if the string we're constructing is a key or a value
  enum class Context : uint8_t {
    BARE,
    OBJECT,
    ARRAY
  };
  struct ContextMarker {
    Context context;
    size_t counter;
  };

  std::stack<ContextMarker> context;

public:
  GrepHandler(Dictionary &dictionary, OutputHandler &handler,
              const std::vector<std::string> &pKey,
              const std::vector<uint64_t> &pUint64_t,
              const std::vector<int64_t> &pInt64_t,
              const std::vector<std::string> &pFullStr)
      : handler_(handler), dictionary_(dictionary),
        pKey_(pKey), pUint64_t_(pUint64_t), pInt64_t_(pInt64_t),
        pFullStr_(pFullStr),
        matched_(false) {
    context.push({Context::BARE, 0});
  }

  bool isKey() {
    auto &c = context.top();
    return (c.context == Context::OBJECT) && (c.counter % 2 == 0);
  }

  void incrCounter() {
    context.top().counter++;
  }

  void onValue(FileByteSource &source) {
    matched_ = false;
    auto sov = source.pos();
    ValueParser<GrepHandler> parser(source, *this);
    parser.value();

    if (matched_) {
      source.seek(sov);
      handler_.onValue(source);
    }
  }

  template<typename C, typename V>
  bool find(const C &container, const V &value) {
    return std::find(container.cbegin(), container.cend(), value)
           != container.cend();
  }

  void onNull() override {
    incrCounter();
  }
  void onBool(bool) override {
    incrCounter();
  }
  void onInt(int64_t value) override {
    if (find(pInt64_t_, value)) matched_ = true;
    incrCounter();
  }
  void onUint(uint64_t value) override {
    if (find(pUint64_t_, value)) matched_ = true;
    incrCounter();
  }
  void onDouble(double) override {
    incrCounter();
  }

  void onDictRef(size_t dictIdx) {
    assert(dictIdx < dictionary_.size());
    if (isKey()) {
      if (find(pKey_, dictionary_[dictIdx])) matched_ = true;
    } else {
      if (find(pFullStr_, dictionary_[dictIdx])) matched_ = true;
    }
    incrCounter();
  }

  void onObjectStart() override {
    context.push({Context::OBJECT, 0});
  }

  void onObjectEnd() override {
    context.pop();
    incrCounter();
  }

  void onArrayStart() {
    context.push({Context::ARRAY, 0});
  }

  void onArrayEnd() {
    context.pop();
    incrCounter();
  }

  void onStringStart(size_t len) override {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() override {
    auto sv = std::string_view(str_.data(), str_.size());
    if (isKey()) {
      if (find(pKey_, sv)) matched_ = true;
    } else {
      if (find(pFullStr_, sv)) matched_ = true;
    }
    incrCounter();
  }

  void onStringFragment(std::string_view frag) override {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
};
