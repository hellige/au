#pragma once

#include "au/AuDecoder.h"
#include "AuRecordHandler.h"
#include "JsonProxies.h"
#include "Tail.h"
#include "TimestampPattern.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <re2/re2.h>
#include <regex>
#include <string>
#include <variant>

namespace au {

struct Pattern {
  using StrOrRegex = std::variant<std::string, std::unique_ptr<re2::RE2>>;

  struct StrPattern {
    StrOrRegex pattern;
    bool fullMatch;
  };

  enum class Atom {
    True, False, Null
  };

  std::optional<StrOrRegex> keyPattern;
  std::optional<Atom> atomPattern;
  std::optional<int64_t> intPattern;
  std::optional<uint64_t> uintPattern;
  std::optional<double> doublePattern;
  std::optional<StrPattern> strPattern;
  std::optional<TimestampPattern> timestampPattern; // half-open interval [start, end)

  std::optional<uint32_t> numMatches;
  std::optional<size_t> scanSuffixAmount;
  uint32_t beforeContext = 0;
  uint32_t afterContext = 0;
  bool bisect = false;
  bool count = false;
  bool forceFollow = false;
  bool matchOrGreater = false;

  struct StrOrRegexVisitor {
    std::string_view value;
    bool fullMatch;
    bool matchOrGreater;

    bool operator()(const std::string &s) const {
      if (fullMatch) {
        if (matchOrGreater) return value >= s;
        return s == value;
      }

      // substring search is incompatible with binary search...
      if (matchOrGreater) return false;
      return value.find(s) != std::string::npos;
    }

    bool operator()(const std::unique_ptr<re2::RE2> &re) const {
      if (fullMatch)
        return re2::RE2::FullMatch(value, *re);
      return re2::RE2::PartialMatch(value, *re);
    }
  };

  bool requiresKeyMatch() const { return keyPattern.has_value(); }

  bool needsDateScan() const {
    return timestampPattern && timestampPattern->isRelativeTime;
  }

  bool matchesKey(std::string_view key) const {
    if (!keyPattern) return true;
    StrOrRegexVisitor visitor{
        .value = key, .fullMatch = true, .matchOrGreater = false};
    return std::visit(visitor, *keyPattern);
  }

  bool matchesValue(Atom val) const {
    // atom search is incompatible with binary search...
    if (matchOrGreater) return false;
    if (!atomPattern) return false;
    return *atomPattern == val;
  }

  bool matchesValue(time_point val) {
    if (!timestampPattern) return false;
    if (timestampPattern->isRelativeTime) guessDate(val);
    if (matchOrGreater) return val >= timestampPattern->start;
    return val >= timestampPattern->start && val < timestampPattern->end;
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
    if (!strPattern)
      return false;

    StrOrRegexVisitor visitor{.value = sv,
                              .fullMatch = strPattern->fullMatch,
                              .matchOrGreater = matchOrGreater};
    return std::visit(visitor, strPattern->pattern);
  }

  void guessDate(time_point val) {
    timestampPattern->isRelativeTime = false;

    using namespace std::chrono;
    auto tt = system_clock::to_time_t(time_point_cast<milliseconds>(val));
    std::tm tm;
    memset(&tm, 0, sizeof(tm));
    gmtime_r(&tt, &tm);

    std::tm daytm;
    memset(&daytm, 0, sizeof(tm));
    daytm.tm_year = tm.tm_year;
    daytm.tm_mon = tm.tm_mon;
    daytm.tm_mday = tm.tm_mday;
    auto base = duration_cast<nanoseconds>(seconds(timegm(&daytm)));

    if (timestampPattern->start + base < val) {
      // if the time is less than the first matching timestamp, then roll to
      // the next day
      daytm.tm_mday += 1;
      base = duration_cast<nanoseconds>(seconds(timegm(&daytm)));
    }

    timestampPattern->start += base;
    timestampPattern->end += base;
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
  Pattern &pattern_;

  std::vector<char> str_;
  const Dictionary::Dict *dictionary_ = nullptr;
  bool attempted_;
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
  GrepHandler(Pattern &pattern)
      : pattern_(pattern),
        attempted_(false),
        matched_(false) {
    str_.reserve(1<<16);
  }

  // it's not entirely clear whether attemptedMatch() should be based upon
  // whether a value was offered to the handler at a time when a match might
  // have been valid (as it is currently), or whether we should only mark
  // attempted as true when the pattern itself might accept the value (e.g.,
  // only when an actual timestamp is checked in the case of an explicit
  // timestamp-only match). the latter would be slightly uglier, so i'm sticking
  // with the first option for now. for my own common uses, this makes no
  // difference. but if anybody ever cares, it might be worth reconsidering.
  bool attemptedMatch() const { return attempted_; }
  bool matched() const { return matched_; }

  bool isKey() const {
    auto &c = context_.back();
    return (c.context == Context::OBJECT) && (c.counter % 2 == 0);
  }

  void incrCounter() {
    context_.back().counter++;
  }

  void onValue(AuByteSource &source, const Dictionary::Dict &dict) {
    initializeForValue(&dict);
    ValueParser<GrepHandler> parser(source, *this);
    parser.value();
  }

  void initializeForValue(const Dictionary::Dict *dict = nullptr) {
    dictionary_ = dict;
    context_.clear();
    context_.emplace_back(Context::BARE, 0, !pattern_.requiresKeyMatch());
    attempted_ = false;
    matched_ = false;
  }

  template<typename C, typename V>
  bool find(const C &container, const V &value) {
    return std::find(container.cbegin(), container.cend(), value)
           != container.cend();
  }

  void onNull(size_t) {
    attempted_ |= context_.back().checkVal;
    if (context_.back().checkVal && pattern_.matchesValue(Pattern::Atom::Null))
      matched_ = true;
    incrCounter();
  }

  void onBool(size_t, bool val) {
    auto atom = val ? Pattern::Atom::True : Pattern::Atom::False;
    attempted_ |= context_.back().checkVal;
    if (context_.back().checkVal && pattern_.matchesValue(atom))
      matched_ = true;
    incrCounter();
  }

  void onInt(size_t, int64_t value) {
    attempted_ |= context_.back().checkVal;
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onUint(size_t, uint64_t value) {
    attempted_ |= context_.back().checkVal;
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onTime(size_t, time_point value) {
    attempted_ |= context_.back().checkVal;
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onDouble(size_t, double value) {
    attempted_ |= context_.back().checkVal;
    if (context_.back().checkVal && pattern_.matchesValue(value))
      matched_ = true;
    incrCounter();
  }

  void onDictRef(size_t, size_t dictIdx) {
    // this could perhaps be optimized by indexing the dictionary as things are
    // added and then just checking whether dictIdx refers to a known matching
    // value. but, particularly since most dictionary entries and most patterns
    // are very short strings, it's not clear whether that would be worth it.
    // probably worth a try someday, but not essential...
    checkString(dictionary_->at(dictIdx));
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
      attempted_ |= context_.back().checkVal;
      if (context_.back().checkVal && pattern_.matchesValue(sv))
        matched_ = true;
    }
  }
};


namespace {

template <typename This>
class Grepper {
protected:
  Pattern &pattern;
  AuByteSource &source;
  GrepHandler grepHandler;

public:
  Grepper(Pattern &pattern, AuByteSource &source)
  : pattern(pattern),
    source(source),
    grepHandler(pattern) {}

  int doGrep() {
    if (pattern.needsDateScan()) {
      performDateScan();
    }

    if (pattern.bisect) {
      return doBisect();
    }

    return reallyDoGrep();
  }

private:
  void performDateScan() {
    constexpr size_t DATE_SCAN_RECORDS = 100;
    constexpr size_t DATE_SCAN_BYTES = 256 * 1024;

    auto pos = source.pos();
    source.setPin(pos);
    for (auto i = 0u; i < DATE_SCAN_RECORDS; i++) {
      if (source.pos() - pos > DATE_SCAN_BYTES)
        break;
      if (!static_cast<This *>(this)->parseValue())
        break;
      if (grepHandler.matched()) {
        assert(!pattern.needsDateScan());
        break;
      }
    }
    source.clearPin();
    source.seek(pos);
  }

  int reallyDoGrep() {
    if (pattern.count) pattern.beforeContext = pattern.afterContext = 0;

    try {
      std::vector<size_t> posBuffer;
      posBuffer.reserve(pattern.beforeContext+1);
      size_t force = 0;
      size_t total = 0;
      bool inMatchRegion = false;
      size_t suffixStartPos = source.pos();
      size_t numMatches = std::numeric_limits<size_t>::max();
      if (pattern.numMatches) numMatches = *pattern.numMatches;
      size_t suffixLength = std::numeric_limits<size_t>::max();
      if (pattern.scanSuffixAmount) suffixLength = *pattern.scanSuffixAmount;

      while (!source.peek().isEof()) {
        if (!force && source.pos() - suffixStartPos > suffixLength) break;

        auto candidatePos = source.pos();
        if (!pattern.count) {
          if (posBuffer.size() == pattern.beforeContext + 1)
            posBuffer.erase(posBuffer.begin());
          posBuffer.push_back(candidatePos);
          source.setPin(posBuffer.front());
        }

        if (!static_cast<This *>(this)->parseValue())
          break;
        auto matchedNow = false;
        if (grepHandler.matched() && total < numMatches) {
          inMatchRegion = true;
          matchedNow = true;
          // we only want to count records with *actual* matches, not records
          // matched by virtue of grep -F
          total++;
        } else if (grepHandler.attemptedMatch()) {
          inMatchRegion = false;
        }
        matchedNow |= pattern.forceFollow && inMatchRegion;
        if (matchedNow) {
          // avoid using posBuffer.back() for this, so that we can completely
          // ignore posBuffer in case pattern.count is true. also do this even
          // if we're force-following (not only if total < numMatches) so that
          // we don't fall out of the suffix length until we're really done
          suffixStartPos = source.pos();
          if (pattern.count) continue;
          // this is a little tricky. this seek() might send us backward over a
          // number of records, which might cross over one or more dictionary
          // resets. but since we know we've been in sync up to this point, we
          // should always expect the needed dictionary to be within the last
          // few that we're keeping cached. so no dictionary rebuild will be
          // needed here, unless we seek backward over a large number of
          // dictionary resets (like, more than 32 according to the current
          // code)
          source.clearPin();
          source.seek(posBuffer.front());
          while (!posBuffer.empty()) {
            static_cast<This *>(this)->outputValue();
            posBuffer.pop_back();
          }
          force = pattern.afterContext;
        } else if (force) {
          source.clearPin();
          source.seek(posBuffer.back());
          posBuffer.clear();
          static_cast<This *>(this)->outputValue();
          force--;
        }
      }

      if (pattern.count) {
        std::cout << total << std::endl;
      }
    } catch (parse_error &e) {
      std::cerr << e.what() << std::endl;
      return -1;
    }

      return 0;
  }

  int doBisect() {
    constexpr size_t SCAN_THRESHOLD = 256 * 1024;
    constexpr size_t PREFIX_AMOUNT = 512 * 1024;
    // it's important that the suffix amount be large enough to cover the entire
    // scan length + the prefix buffer. this is to guarantee that we will search
    // AT LEAST the entire scan region for the first match before giving up.
    // after finding the first match, we'll keep scanning until we go
    // SUFFIX_AMOUNT without seeing any matches. but we do want to make sure we
    // look for the first match in the entire region where it could possibly be
    // (and a bit beyond).
    constexpr size_t SUFFIX_AMOUNT = SCAN_THRESHOLD + PREFIX_AMOUNT + 512 * 1024;
    static_assert(SUFFIX_AMOUNT > PREFIX_AMOUNT + SCAN_THRESHOLD);

    if (!source.isSeekable()) {
      std::cerr << "Cannot binary search in non-seekable file '" << source.name()
        << "'" << std::endl;
      return -1;
    }

    auto origMatchOrGreater = pattern.matchOrGreater;
    pattern.matchOrGreater = true;

    try {
      size_t start = 0;
      size_t end = source.endPos();
      while (end > start) {
        if (end - start <= SCAN_THRESHOLD) {
          static_cast<This *>(this)->seekSync(
                  start > PREFIX_AMOUNT ? start - PREFIX_AMOUNT : 0);
          pattern.scanSuffixAmount = SUFFIX_AMOUNT;
          pattern.matchOrGreater = origMatchOrGreater;
          return reallyDoGrep();
        }

        size_t next = start + (end-start)/2;
        static_cast<This *>(this)->seekSync(next);

        auto startOfScan = source.pos();
        do {
            if (!static_cast<This *>(this)->parseValue())
            return 0;

          // the bisect pattern fails to match if the current record *strictly*
          // precedes any records matching the pattern (i.e., it matches any record
          // which is greater than or equal to the pattern). so we should eventually
          // find the approximate location of the first such record.
          if (grepHandler.matched()) {
            if (startOfScan < end) {
              end = startOfScan;
            } else {
              // this is an indication that we've jumped back to bisect the range
              // (start, end) but that in scanning forward to find the first record,
              // we ended up at or even past the end of the range. (basically, this
              // means the file contains a huge record.) if we update end
              // and bisect again, the same thing will happen again and we'll end
              // up doing this forever. in this case, we'll just set start and end
              // in such a way as to force a scan on the next iteration.
              end = start + 1;
            }
          } else if (grepHandler.attemptedMatch()) {
            start = startOfScan;
          }
        } while (!grepHandler.attemptedMatch());
      }
    } catch (parse_error &e) {
      std::cerr << e.what() << std::endl;
      return -1;
    }

    return 0;
  }
};

template <typename OutputHandler>
class AuGrepper : public Grepper<AuGrepper<OutputHandler>> {
  friend class Grepper<AuGrepper<OutputHandler>>;
  Dictionary dictionary_;
  AuRecordHandler<OutputHandler> outputRecordHandler_;
  AuRecordHandler<GrepHandler> grepRecordHandler_;

public:
  // clang warns too aggressively if the names of these arguments shadow the
  // base class member vars. hence "p" and "s"...
  AuGrepper(Pattern &p, AuByteSource &s, OutputHandler &handler)
  : Grepper<AuGrepper<OutputHandler>>(p, s),
    dictionary_(32),
    outputRecordHandler_(dictionary_, handler),
    grepRecordHandler_(dictionary_, this->grepHandler) {}

private:
  void seekSync(size_t pos) {
    this->source.seek(pos);
    TailHandler tailHandler(dictionary_, this->source);
    if (!tailHandler.sync()) {
      AU_THROW("Failed to find record at position " << pos);
    }
  }

  void outputValue() {
    // clang 10 and 11 erroneously warn here if "parser" is inlined.
    auto parser = RecordParser(this->source, outputRecordHandler_);
    parser.parseUntilValue();
  }

  bool parseValue() {
    // clang 10 and 11 erroneously warn here if "parser" is inlined.
    auto parser = RecordParser(this->source, grepRecordHandler_);
    return parser.parseUntilValue();
  }
};

template <typename OutputHandler>
class JsonGrepper : public Grepper<JsonGrepper<OutputHandler>> {
  static constexpr auto parseOpt = rapidjson::kParseStopWhenDoneFlag +
                                    rapidjson::kParseFullPrecisionFlag +
                                    rapidjson::kParseNanAndInfFlag;

  friend class Grepper<JsonGrepper<OutputHandler>>;
  rapidjson::Reader reader_;
  OutputHandler &handler_;

public:
  // clang warns too aggressively if the names of these arguments shadow the
  // base class member vars. hence "p" and "s"...
  JsonGrepper(Pattern &p, AuByteSource &s, OutputHandler &handler)
  : Grepper<JsonGrepper<OutputHandler>>(p, s),
    handler_(handler) {}

private:
  void seekSync(size_t pos) {
    this->source.seek(pos);
    // for json files, we can't tell if we've landed precisely on the beginning
    // of a record, since the only indicator is the newline separator (which
    // would be at pos-1). we could try to detect that specifically, but in the
    // normal course of bisecting a file, it really doesn't matter if we're off
    // by one record either way. however, it *does* matter if we happen to land
    // exactly on the start of the file, as may happen if the file is very
    // small. so in particular case, we do need to detect it and do something
    // special.
    if (pos == 0) return;
    if (!this->source.scanTo("\n")) {
      AU_THROW("Failed to find record at position " << pos);
    }
  }

  void outputValue() {
    JsonSaxProxy proxy(handler_);
    AuByteSourceStream wrappedSource(this->source);
    handler_.startJsonValue();
    reader_.Parse<parseOpt>(wrappedSource, proxy);
    handler_.endJsonValue();
  }

  bool parseValue() {
    this->grepHandler.initializeForValue();
    JsonSaxProxy proxy(this->grepHandler);
    AuByteSourceStream wrappedSource(this->source);
    return reader_.Parse<parseOpt>(wrappedSource, proxy);
  }
};

class AsciiGrepper : public Grepper<AsciiGrepper> {
  friend class Grepper<AsciiGrepper>;

public:
  // clang warns too aggressively if the names of these arguments shadow the
  // base class member vars. hence "p" and "s"...
  AsciiGrepper(Pattern &p, AuByteSource &s)
  : Grepper<AsciiGrepper>(p, s) {}

private:
  void seekSync(size_t pos) {
    source.seek(pos);
    // see comment in JsonGrepper above...
    if (pos == 0) return;
    if (!source.scanTo("\n")) {
      AU_THROW("Failed to find record at position " << pos);
    }
    source.next();
  }

  void outputValue() {
    // this contortion with the pin will force the byte source to keep the
    // entire line buffered. then the call to read will definitely give us the
    // whole line in one callback.
    auto start = source.pos();
    source.setPin(start);
    // if we don't find a newline, then we're left at the end of the stream,
    // and ought to just print the last (unterminated) line anyway. we just need
    // to know to add the newline in that case. we can unconditionally call
    // next() either way.
    auto foundNewline = source.scanTo("\n");
    source.next();
    auto len = source.pos() - start;
    source.clearPin();
    source.seek(start);
    source.readFunc(len, [](auto line) { std::cout << line; });
    if (!foundNewline) std::cout << "\n";
  }

  bool parseValue() {
    grepHandler.initializeForValue();

    // we only bail when we really have nothing left to read. we don't
    // necessarily want to require a final newline.
    if (source.peek().isEof()) return false;

    constexpr size_t MAX_TIMESTAMP_LEN =
        sizeof("yyyy-mm-ddThh:mm:ss.mmmuuunnn") - 1;
    char buf[MAX_TIMESTAMP_LEN];
    auto len = 0u;
    while (len < sizeof(buf)) {
      if (source.peek().isEof() || source.peek() == '\n')
        break;
      buf[len++] = source.next().charValue();
    }
    auto res = parseTimestampPattern<false>(std::string_view(buf, len));
    if (res) grepHandler.onTime(0, res->start);

    source.scanTo("\n");
    source.next();
    return true;
  }
};

template <typename H>
AuGrepper(Pattern &, AuByteSource &, H &) -> AuGrepper<H>;
template <typename H>
JsonGrepper(Pattern &, AuByteSource &, H &) -> JsonGrepper<H>;

}

}
