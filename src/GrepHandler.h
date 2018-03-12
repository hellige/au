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
template <typename OutputHandler>
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
  bool isKey_;
  bool isStrVal_;

  // Keeps track of the context we're in so we know if the string we're constructing is a key or a value
  enum class Context : uint8_t {
    BARE,
    OBJECT,
    ARRAY
  };

  std::stack<Context> context;

public:
  GrepHandler(Dictionary &dictionary, OutputHandler &handler,
              const std::vector<std::string> &pKey,
              const std::vector<uint64_t> &pUint64_t,
              const std::vector<int64_t> &pInt64_t,
              const std::vector<std::string> &pFullStr)
      : handler_(handler), dictionary_(dictionary),
        pKey_(pKey), pUint64_t_(pUint64_t), pInt64_t_(pInt64_t), pFullStr_(pFullStr),
        matched_(false), isKey_(false), isStrVal_(false)
  {
    context.push(Context::BARE);
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
    return std::find(container.cbegin(), container.cend(), value) != container.cend();
  }

  void onUint(uint64_t value) override {
    if (find(pUint64_t_, value)) matched_ = true;
    if (context.top() == Context::OBJECT) isKey_ = true;
  }

  void onInt(int64_t value) override {
    if (find(pInt64_t_, value)) matched_ = true;
    if (context.top() == Context::OBJECT) isKey_ = true;
  }

  void onNull() override {
    if (context.top() == Context::OBJECT) isKey_ = true;
  }
  void onBool(bool) override {
    if (context.top() == Context::OBJECT) isKey_ = true;
  }
  void onDouble(double) override {
    if (context.top() == Context::OBJECT) isKey_ = true;
  }
  void onDictRef(size_t dictIdx) {
    if (isKey_) {
      if (find(pKey_, dictionary_[dictIdx])) matched_ = true;
      isKey_ = false;
    } else {
      if (find(pFullStr_, dictionary_[dictIdx])) matched_ = true;
      if (context.top() == Context::OBJECT) isKey_ = true;
    }
  }

  void onObjectStart() override {
    context.push(Context::OBJECT);
    isKey_ = true;
  }

  void onObjectEnd() override {
    context.pop();
    isKey_ = false;
  }

  void onStringStart(size_t len) override {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() override {
    auto sv = std::string_view(str_.data(), str_.size());
    if (isKey_) {
      if (find(pKey_, sv)) matched_ = true;
      isKey_ = false;
    } else {
      if (find(pFullStr_, sv)) matched_ = true;
    }
  }

  void onStringFragment(std::string_view frag) override {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
};
