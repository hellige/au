#pragma once

#include "AuDecoder.h"

#include <optional>
#include <variant>

// TODO teach grep to emit au-encoded when desired (-e flag?)

struct Pattern {
  struct StrPattern {
    std::string pattern;
    bool fullMatch;
  };

  std::optional<std::string> keyPattern;
  std::optional<int64_t> intPattern;
  std::optional<uint64_t> uintPattern;
  std::optional<double> doublePattern;
  std::optional<StrPattern> strPattern;

  bool requiresKeyMatch() const { return static_cast<bool>(keyPattern); }

  bool matchesKey(std::string_view key) const {
    if (!keyPattern) return true;
    return *keyPattern == key;
  }

  bool matchesValue(uint64_t val) const {
    if (!uintPattern) return false;
    return *uintPattern == val;
  }

  bool matchesValue(int64_t val) const {
    if (!intPattern) return false;
    return *intPattern == val;

  }

  bool matchesValue(double val) const {
    if (!doublePattern) return false;
    return *doublePattern == val;
  }

  bool matchesValue(std::string_view sv) const {
    if (!strPattern) return false;
    if (strPattern->fullMatch) {
      return strPattern->pattern == sv;
    }

    return sv.find(strPattern->pattern) != std::string::npos;
  }
};

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

  const Pattern &pattern_;

  std::vector<char> str_;
  bool matched_;

  // Keeps track of the context we're in so we know if the string we're
  // constructing or reading is a key or a value
  enum class Context : uint8_t {
    BARE,
    OBJECT,
    ARRAY
  };
  struct ContextMarker {
    Context context;
    size_t counter;
    bool checkVal;
    ContextMarker(Context context, size_t counter, bool checkVal)
        : context(context), counter(counter), checkVal(checkVal) {}
  };

  std::vector<ContextMarker> context_;

public:
  GrepHandler(Dictionary &dictionary,
              OutputHandler &handler,
              Pattern &pattern)
      : handler_(handler),
        dictionary_(dictionary),
        pattern_(pattern),
        matched_(false) {
    str_.reserve(1<<16);
  }

  bool isKey() const {
    auto &c = context_.back();
    return (c.context == Context::OBJECT) && (c.counter % 2 == 0);
  }

  void incrCounter() {
    context_.back().counter++;
  }

  void onValue(FileByteSource &source) {
    context_.clear();
    context_.emplace_back(Context::BARE, 0, !pattern_.requiresKeyMatch());
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

  void onNull(size_t) override {
    incrCounter();
  }

  void onBool(size_t, bool) override {
    incrCounter();
  }

  void onInt(size_t, int64_t value) override {
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onUint(size_t, uint64_t value) override {
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }
  void onDouble(size_t, double value) override {
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onDictRef(size_t, size_t dictIdx) override {
    assert(dictIdx < dictionary_.size());
    checkString(dictionary_[dictIdx]); // TODO optimize by indexing dict first
    incrCounter();
  }

  void onObjectStart() override {
    context_.emplace_back(Context::OBJECT, 0, false);
  }

  void onObjectEnd() override {
    context_.pop_back();
    incrCounter();
  }

  void onArrayStart() override {
    context_.emplace_back(Context::ARRAY, 0, context_.back().checkVal);
  }

  void onArrayEnd() override {
    context_.pop_back();
    incrCounter();
  }

  void onStringStart(size_t, size_t len) override {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() override {
    checkString(std::string_view(str_.data(), str_.size()));
    incrCounter();
  }

  void onStringFragment(std::string_view frag) override {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }

private:
  void checkString(std::string_view sv) {
    if (isKey()) {
      if (pattern_.matchesKey(sv))
        context_.back().checkVal = true;
      return;
    } else {
      if (context_.back().checkVal && pattern_.matchesValue(sv))
        matched_ = true;
    }
  }
};

namespace {

void doGrep(Pattern &pattern, const std::string &filename) {
  Dictionary dictionary;
  JsonOutputHandler jsonHandler(dictionary);
  GrepHandler<JsonOutputHandler> grepHandler(
      dictionary, jsonHandler, pattern);
  AuRecordHandler<decltype(grepHandler)> recordHandler(
      dictionary, grepHandler);
  AuDecoder(filename).decode(recordHandler, false);
}

}