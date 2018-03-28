#pragma once

#include "au/AuDecoder.h"
#include "AuRecordHandler.h"
#include "Tail.h"

#include <cassert>
#include <chrono>
#include <optional>
#include <variant>

struct Pattern {
  struct StrPattern {
    std::string pattern;
    bool fullMatch;
  };

  enum class Atom {
    True, False, Null
  };

  std::optional<std::string> keyPattern;
  std::optional<Atom> atomPattern;
  std::optional<int64_t> intPattern;
  std::optional<uint64_t> uintPattern;
  std::optional<double> doublePattern;
  std::optional<StrPattern> strPattern;
  std::optional<
      std::pair<std::chrono::system_clock::time_point,
          std::chrono::system_clock::time_point>>
      timestampPattern; // half-open interval [start, end)

  std::optional<uint32_t> numMatches;
  std::optional<size_t> scanSuffixAmount;
  uint32_t beforeContext = 0;
  uint32_t afterContext = 0;
  bool bisect = false;
  bool count = false;
  bool matchOrGreater = false;

  bool requiresKeyMatch() const { return static_cast<bool>(keyPattern); }

  bool matchesKey(std::string_view key) const {
    if (!keyPattern) return true;
    return *keyPattern == key;
  }

  bool matchesValue(Atom val) const {
    // atom search is incompatible with binary search...
    if (matchOrGreater) return false;
    if (!atomPattern) return false;
    return *atomPattern == val;
  }

  bool matchesValue(std::chrono::system_clock::time_point val) const {
    if (!timestampPattern) return false;
    if (matchOrGreater) return val >= timestampPattern->first;
    return val >= timestampPattern->first && val < timestampPattern->second;
  }

  bool matchesValue(uint64_t val) const {
    if (!uintPattern) return false;
    if (matchOrGreater) return val >= *uintPattern;
    return *uintPattern == val;
  }

  bool matchesValue(int64_t val) const {
    if (!intPattern) return false;
    if (matchOrGreater) return val >= *intPattern;
    return *intPattern == val;
  }

  bool matchesValue(double val) const {
    if (!doublePattern) return false;
    if (matchOrGreater) return val >= *doublePattern;
    return *doublePattern == val;
  }

  bool matchesValue(std::string_view sv) const {
    if (!strPattern) return false;
    if (strPattern->fullMatch) {
      if (matchOrGreater) return sv >= strPattern->pattern;
      return strPattern->pattern == sv;
    }

    // substring search is incompatible with binary search...
    if (matchOrGreater) return false;
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
class GrepHandler {
  const Pattern &pattern_;

  std::vector<char> str_;
  const Dictionary::Dict *dictionary_ = nullptr;
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
  GrepHandler(const Pattern &pattern)
      : pattern_(pattern),
        matched_(false) {
    str_.reserve(1<<16);
  }

  bool matched() const { return matched_; }

  bool isKey() const {
    auto &c = context_.back();
    return (c.context == Context::OBJECT) && (c.counter % 2 == 0);
  }

  void incrCounter() {
    context_.back().counter++;
  }

  void onValue(FileByteSource &source, const Dictionary::Dict &dict) {
    dictionary_ = &dict;
    context_.clear();
    context_.emplace_back(Context::BARE, 0, !pattern_.requiresKeyMatch());
    matched_ = false;
    ValueParser<GrepHandler> parser(source, *this);
    parser.value();
  }

  template<typename C, typename V>
  bool find(const C &container, const V &value) {
    return std::find(container.cbegin(), container.cend(), value)
           != container.cend();
  }

  void onNull(size_t) {
    if (context_.back().checkVal && pattern_.matchesValue(Pattern::Atom::Null))
      matched_ = true;
    incrCounter();
  }

  void onBool(size_t, bool val) {
    auto atom = val ? Pattern::Atom::True : Pattern::Atom::False;
    if (context_.back().checkVal && pattern_.matchesValue(atom))
      matched_ = true;
    incrCounter();
  }

  void onInt(size_t, int64_t value) {
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onUint(size_t, uint64_t value) {
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onTime(size_t, std::chrono::system_clock::time_point value) {
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onDouble(size_t, double value) {
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onDictRef(size_t, size_t dictIdx) {
    assert(dictIdx < dictionary_->size());
    checkString(dictionary_->at(dictIdx)); // TODO optimize by indexing dict first
    incrCounter();
  }

  void onObjectStart() {
    context_.emplace_back(Context::OBJECT, 0, false);
  }

  void onObjectEnd() {
    context_.pop_back();
    incrCounter();
  }

  void onArrayStart() {
    context_.emplace_back(Context::ARRAY, 0, context_.back().checkVal);
  }

  void onArrayEnd() {
    context_.pop_back();
    incrCounter();
  }

  void onStringStart(size_t, size_t len) {
    if (!pattern_.strPattern
        && !(pattern_.requiresKeyMatch() && isKey()))
      return;
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() {
    checkString(std::string_view(str_.data(), str_.size()));
    incrCounter();
  }

  void onStringFragment(std::string_view frag) {
    if (!pattern_.strPattern
        && !(pattern_.requiresKeyMatch() && isKey()))
      return;
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }

private:
  void checkString(std::string_view sv) {
    if (isKey()) {
      context_.back().checkVal = pattern_.matchesKey(sv);
      return;
    } else {
      if (context_.back().checkVal && pattern_.matchesValue(sv))
        matched_ = true;
    }
  }
};


namespace {

template <typename OutputHandler>
void reallyDoGrep(Pattern &pattern, Dictionary &dictionary,
                  FileByteSource &source, OutputHandler &handler) {
  if (pattern.count) pattern.beforeContext = pattern.afterContext = 0;

  GrepHandler grepHandler(pattern);
  AuRecordHandler recordHandler(dictionary, grepHandler);
  AuRecordHandler outputRecordHandler(dictionary, handler);
  try {
    std::vector<size_t> posBuffer;
    posBuffer.reserve(pattern.beforeContext+1);
    size_t force = 0;
    size_t total = 0;
    size_t matchPos = source.pos();
    size_t numMatches = std::numeric_limits<size_t>::max();
    if (pattern.numMatches) numMatches = *pattern.numMatches;
    size_t suffixLength = std::numeric_limits<size_t>::max();
    if (pattern.scanSuffixAmount) suffixLength = *pattern.scanSuffixAmount;

    while (source.peek() != EOF) {
      if (!force) {
        if (total >= numMatches) break;
        if (source.pos() - matchPos > suffixLength) break;
      }

      if (!pattern.count) {
        if (posBuffer.size() == pattern.beforeContext + 1)
          posBuffer.erase(posBuffer.begin());
      }
      posBuffer.push_back(source.pos());
      if (!RecordParser(source, recordHandler).parseUntilValue())
        break;
      if (grepHandler.matched() && total < numMatches) {
        matchPos = posBuffer.back();
        total++;
        if (pattern.count) continue;
        source.seek(posBuffer.front());
        while (!posBuffer.empty()) {
          RecordParser(source, outputRecordHandler).parseUntilValue();
          posBuffer.pop_back();
        }
        force = pattern.afterContext;
      } else if (force) {
        source.seek(posBuffer.back());
        RecordParser(source, outputRecordHandler).parseUntilValue();
        force--;
      }
    }

    if (pattern.count) {
      std::cout << total << std::endl;
    }
  } catch (parse_error &e) {
    std::cerr << e.what() << std::endl;
  }
}

void seekSync(TailByteSource &source, Dictionary &dictionary, size_t pos) {
  source.seek(pos);
  TailHandler tailHandler(dictionary, source);
  if (!tailHandler.sync()) {
    THROW("Failed to find record at position " << pos);
  }
}

template <typename OutputHandler>
void doBisect(Pattern &pattern, const std::string &filename,
              OutputHandler &handler) {
  constexpr size_t SCAN_THRESHOLD = 256 * 1024;
  constexpr size_t PREFIX_AMOUNT = 512 * 1024;
  // it's important that the suffix amount be large enough to cover the entire
  // scan length + the prefix buffer. this is to guarantee that we will search
  // AT LEAST the entire scan region for the first match before giving up.
  // after finding the first match, we'll keep scanning until we go
  // SUFFIX_AMOUNT without seeing any matches. but we do want to make sure we
  // look for the first match in the entire region where it could possibly be
  // (and a bit beyond).
  constexpr size_t SUFFIX_AMOUNT = SCAN_THRESHOLD + PREFIX_AMOUNT + 266 * 1024;
  static_assert(SUFFIX_AMOUNT > PREFIX_AMOUNT + SCAN_THRESHOLD);

  Pattern bisectPattern(pattern);
  bisectPattern.matchOrGreater = true;

  Dictionary dictionary(32);
  GrepHandler grepHandler(bisectPattern);
  AuRecordHandler recordHandler(dictionary, grepHandler);
  TailByteSource source(filename, false);

  try {
    size_t start = 0;
    size_t end = source.endPos();
    while (end > start) {
      if (end - start <= SCAN_THRESHOLD) {
        seekSync(source, dictionary,
                 start > PREFIX_AMOUNT ? start - PREFIX_AMOUNT : 0);
        pattern.scanSuffixAmount = SUFFIX_AMOUNT;
        reallyDoGrep(pattern, dictionary, source, handler);
        return;
      }

      size_t next = start + (end-start)/2;
      seekSync(source, dictionary, next);

      auto sor = source.pos();
      if (!RecordParser(source, recordHandler).parseUntilValue())
        break;

      // the bisectPattern fails to match if the current record *strictly*
      // precedes any records matching the pattern (i.e., it matches any record
      // which is greater than or equal to the pattern). so we should eventually
      // find the approximate location of the first such record.
      if (grepHandler.matched()) {
        end = sor;
      } else {
        start = sor;
      }
    }
  } catch (parse_error &e) {
    std::cerr << e.what() << std::endl;
  }
}

template <typename OutputHandler>
void doGrep(Pattern &pattern, const std::string &filename,
            OutputHandler &handler) {
  if (pattern.bisect) {
    doBisect(pattern, filename, handler);
    return;
  }

  Dictionary dictionary;
  FileByteSource source(filename, false);
    reallyDoGrep(pattern, dictionary, source, handler);
}

}
