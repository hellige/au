#pragma once

#include "au/AuCommon.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <stdio.h>
#include <string>

namespace au {

class AuEncoder;

enum class AuIntern {
  ByFrequency,
  ForceIntern,
  ForceExplicit
};

class AuStringIntern {
  /** Frequently encountered strings should be interned. The UsageTracker keeps
   * track of how many times we've seen a string. The INTERN_CACHE_SIZE most
   * recent unique strings are cached and once one of those strings has been
   * seen INTERN_THRESH times, it is removed from the tracker and reported as
   * internable.
   */
  class UsageTracker {
    /** Keeps track of strings encountered. Most recent string is at the end of
     * the list. Once this list reaches INTERN_CACHE_SIZE, we start discarding
     * the oldest entry from the front before adding a new one at the end.
     */
    using InOrder = std::list<std::string>;
    std::list<std::string> freeList_;
    InOrder inOrder_;

    /** DictVal.first == How many times the string pointed to by DictVal.second
     * has been seen.
     */
    using DictVal = std::pair<size_t, InOrder::iterator>;
    using Dict = std::unordered_map<std::string_view, DictVal>;
    Dict dict_;

    void pop(Dict::iterator it) {
      auto listIt = it->second.second;
      dict_.erase(it);
      freeList_.splice(freeList_.begin(), inOrder_, listIt);
    }

  public:
    /// Strings that have been encountered this many times will be interned.
    const size_t INTERN_THRESH;
    /// We track this many unique most recent strings.
    const size_t INTERN_CACHE_SIZE;

    UsageTracker(size_t internThresh, size_t internCacheSize)
        : freeList_(internCacheSize + 1),
          INTERN_THRESH(internThresh),
          INTERN_CACHE_SIZE(internCacheSize)
    {}

    bool shouldIntern(std::string_view sv) {
      auto it = dict_.find(sv);
      if (it != dict_.end()) {
        if (it->second.first >= INTERN_THRESH) {
          pop(it);
          return true;
        } else {
          it->second.first++;
          return false;
        }
      } else {
        if (inOrder_.size() >= INTERN_CACHE_SIZE) {
          pop(dict_.find(inOrder_.front()));
        }
        if (freeList_.begin() == freeList_.end())
          throw std::bad_alloc();
        // insert at end
        inOrder_.splice(inOrder_.end(), freeList_, freeList_.begin());
        auto &s = inOrder_.back();
        s.assign(sv);
        dict_.emplace(s, DictVal{size_t(1), --(inOrder_.end())});
        return false;
      }
    }

    void clear() {
      dict_.clear();
      freeList_.splice(freeList_.begin(), inOrder_);
    }

    size_t size() const {
      return dict_.size();
    }
  };

  struct InternEntry {
    size_t internIndex;
    size_t occurences;
  };

  std::vector<std::string> dictInOrder_;
  /// The string and its intern index
  std::unordered_map<std::string_view, InternEntry> dictionary_;
  const size_t tinyStringSize_;
  UsageTracker internCache_;

public:
  struct Config {
    size_t tinyStr = 4;
    size_t internThresh = 10;
    size_t internCacheSize = 1000;
    size_t clearThreshold = 1400;
  };

  explicit AuStringIntern() : AuStringIntern(Config{}) {}

  explicit AuStringIntern(Config config)
      : tinyStringSize_(config.tinyStr),
        internCache_(config.internThresh, config.internCacheSize) {
    const auto reserveSize = static_cast<size_t>(
        static_cast<double>(config.clearThreshold) * 1.2);
    dictInOrder_.reserve(reserveSize);
    dictionary_.reserve(reserveSize);
  }

  std::optional<size_t> idx(std::string_view sv, AuIntern intern) {
    if (sv.length() <= tinyStringSize_) return {std::nullopt};
    if (intern == AuIntern::ForceExplicit) return {std::nullopt};

    auto it = dictionary_.find(sv);
    if (it != dictionary_.end()) {
      it->second.occurences++;
      return it->second.internIndex;
    }

    if (intern == AuIntern::ForceIntern || internCache_.shouldIntern(sv)) {
      auto nextEntry = dictInOrder_.size();
      if (dictInOrder_.size() == dictInOrder_.capacity()) {
        // about to resize... need to reconstruct dictionary_ since string_view
        // might point to a SSO std::string. might as well reIndex. If we
        // `clear` about where we are told in Config, this should very rarely
        // happen
        dictInOrder_.reserve(dictInOrder_.size() * 2);
        doReIndex();
      }
      const auto &s = dictInOrder_.emplace_back(std::string(sv));
      dictionary_.emplace(s, InternEntry{nextEntry, 1});
      return nextEntry;
    }
    return {std::nullopt};
  }

  const std::vector<std::string> &dict() const { return dictInOrder_; }

  void clear(bool clearUsageTracker) {
    dictionary_.clear();
    dictInOrder_.clear();
    if (clearUsageTracker) internCache_.clear();
  }

  /// Removes strings that are used less than "threshold" times from the hash
  size_t purge(size_t threshold) {
    // Note: We can't modify dictInOrder_ or else the internIndex will no longer
    // match.
    size_t purged = 0;
    for (auto it = dictionary_.begin(); it != dictionary_.end();) {
      if (it->second.occurences < threshold) {
        it = dictionary_.erase(it);
        purged++;
      } else {
        ++it;
      }
    }
    return purged;
  }

  /// Purges the dictionary and re-indexes the remaining entries so the more
  /// frequent ones are at the beginning (and have smaller indices).
  size_t reIndex(size_t threshold) {
    size_t purged = purge(threshold);
    doReIndex();
    return purged;
  }

  void doReIndex() {
    std::vector<std::pair<std::size_t,std::string>> tmpDict;
    tmpDict.reserve(dictionary_.size());
    for (auto &[_, entry] : dictionary_) {
      (void) _;
      tmpDict.emplace_back(
          entry.occurences,
          std::move(dictInOrder_[entry.internIndex]));
    }

    std::sort(tmpDict.begin(), tmpDict.end(),
              [] (const auto &a, const auto &b) { return a > b; });

    dictInOrder_.clear();
    dictionary_.clear();
    std::size_t idx = 0;
    for (const auto &[occurrences, str] : tmpDict) {
      const auto &s = dictInOrder_.emplace_back(std::move(str));
      dictionary_.emplace(s, InternEntry{idx++, occurrences});
    }
  }

  // For debug/profiling
  auto getStats() const {
    return std::unordered_map<std::string, int> {
        {"HashBucketCount", dictionary_.bucket_count()},
        {"HashLoadFactor",  dictionary_.load_factor()},
        {"MaxLoadFactor",   dictionary_.max_load_factor()},
        {"HashSize",        dictionary_.size()},
        {"DictSize",        dictInOrder_.size()},
        {"CacheSize",       internCache_.size()}
    };
  }
};

class AuVectorBuffer {
  std::vector<char> v;
  std::size_t idx{};
public:
  AuVectorBuffer(size_t size = 1024 * 1024) {
    v.resize(size);
  }
  void put(char c) {
    if (__builtin_expect(idx == v.capacity(), 0))
      v.resize(v.size() * 2);
    v[idx++] = c;
  }
  char *raw(size_t size) {
    if (__builtin_expect(idx + size > v.capacity(), 0))
      v.resize(std::max(v.size() * 2, idx + size));
    auto front = idx;
    idx += size;
    return v.data() + front;
  }
  void write(const char *data, size_t size) {
    memcpy(raw(size), data, size);
  }
  size_t tellp() const {
    return idx;
  }
  const std::string_view str() const {
    return std::string_view(v.data(), idx);
  }
  void clear() {
    idx = 0;
  }
};

class AuWriter {
  AuVectorBuffer &msgBuf_;
  AuStringIntern &stringIntern_;

  void encodeString(const std::string_view sv) {
    static constexpr size_t MaxInlineStringSize = 31;
    if (sv.length() <= MaxInlineStringSize) {
      msgBuf_.put(0x20 | static_cast<char>(sv.length()));
    } else {
      msgBuf_.put(marker::String);
      valueInt(sv.length());
    }
    msgBuf_.write(sv.data(), sv.length());
  }

  void encodeStringIntern(const std::string_view sv, AuIntern intern) {
    auto idx = stringIntern_.idx(sv, intern);
    if (!idx) {
      encodeString(sv);
    } else if (*idx < 0x80) {
      msgBuf_.put(static_cast<char>(0x80 | *idx));
    } else {
      msgBuf_.put(marker::DictRef);
      valueInt(*idx);
    }
  }

  template<typename O>
  class HasWriteAu {
    template<typename OO>
    static auto test(int)
    -> decltype(&OO::writeAu, std::true_type());

    template<typename>
    static auto test(...) -> std::false_type;

  public:
    static constexpr bool value = decltype(test<O>(0))::value;
  };

  template<typename O>
  class HasOperatorApply {
    template<typename OO>
    static auto test(int)
    -> decltype(&OO::operator(), std::true_type());

    template<typename>
    static auto test(...) -> std::false_type;

  public:
    static constexpr bool value = decltype(test<O>(0))::value;
  };

public:
  AuWriter(AuVectorBuffer &buf, AuStringIntern &stringIntern)
      : msgBuf_(buf), stringIntern_(stringIntern) {}
  virtual ~AuWriter() = default;

  class KeyValSink {
    AuWriter &writer_;
    KeyValSink(AuWriter &writer) : writer_(writer) {}
    friend AuWriter;
  public:
    template<typename V>
    void operator()(std::string_view key, V &&val) {
      writer_.kvs(key, std::forward<V>(val));
    }
  };

  /**
   * <code>
   * writer.map(
   *     "key1", "string value",
   *     "key2", 123,
   *     "key3", writer.arrayVals([&]() {
   *        writer.val("a");
   *        writer.val("b");
   *     })
   * );
   * </code>
   * @tparam Args
   * @param args
   * @return
   */
  template<typename... Args>
  AuWriter &map(Args &&... args) {
    msgBuf_.put(marker::ObjectStart);
    kvs(std::forward<Args>(args)...);
    msgBuf_.put(marker::ObjectEnd);
    return *this;
  }

  template<typename... Args>
  AuWriter &array(Args &&... args) {
    msgBuf_.put(marker::ArrayStart);
    vals(std::forward<Args>(args)...);
    msgBuf_.put(marker::ArrayEnd);
    return *this;
  }

  template<typename F>
  auto mapVals(F &&f) {
    return [this, f] {
      KeyValSink sink(*this);
      msgBuf_.put(marker::ObjectStart);
      f(sink);
      msgBuf_.put(marker::ObjectEnd);
    };
  }

  template<typename F>
  auto arrayVals(F &&f) {
    return [this, f] {
      msgBuf_.put(marker::ArrayStart);
      f();
      msgBuf_.put(marker::ArrayEnd);
    };
  }

  // Interface to support SAX handlers
  AuWriter &startMap() {
    msgBuf_.put(marker::ObjectStart);
    return *this;
  }
  AuWriter &endMap() {
    msgBuf_.put(marker::ObjectEnd);
    return *this;
  }
  AuWriter &startArray() {
    msgBuf_.put(marker::ArrayStart);
    return *this;
  }
  AuWriter &endArray() {
    msgBuf_.put(marker::ArrayEnd);
    return *this;
  }
  void key(std::string_view key) {
    encodeStringIntern(key, AuIntern::ForceIntern);
  }

  AuWriter &null() {
    msgBuf_.put(marker::Null);
    return *this;
  }
  AuWriter &value(std::nullptr_t) { return null(); }

  template<typename T>
  AuWriter &value(const T *t) {
    if (t) value(*t);
    else null();
    return *this;
  }

  template<typename T>
  AuWriter &value(const std::unique_ptr<T> &val) { return value(val.get()); }

  template<typename T>
  AuWriter &value(const std::shared_ptr<T> &val) { return value(val.get()); }

  AuWriter &value(const char *s) { return value(std::string_view(s)); }
  /**
   * @param sv
   * @param intern If uninitialized, it will intern (or not) based on frequency
   * of the string. If true, it will force interning (subject to tiny string
   * limits). If false, it will force in-lining.
   * @return
   */
  AuWriter &value(const std::string_view sv,
                  std::optional<bool> intern = std::nullopt) {
    AuIntern internEnum{};
    if (intern.has_value()) {
      internEnum = intern.value()
        ? AuIntern::ForceIntern : AuIntern::ForceExplicit;
    }
    if (internEnum == AuIntern::ForceExplicit) {
      encodeString(sv);
    } else {
      encodeStringIntern(sv, internEnum);
    }
    return *this;
  }

  AuWriter &value(const std::string &s) {
    return value(std::string_view(s.c_str(), s.length()));
  }
  AuWriter &value(bool b) {
    msgBuf_.put(b ? marker::True : marker::False);
    return *this;
  }

  AuWriter &value(int i)                { return IntSigned(i); }
  AuWriter &value(unsigned int i)       { return IntUnsigned(i); }
  AuWriter &value(long int i)           { return IntSigned(i); }
  AuWriter &value(long unsigned i)      { return IntUnsigned(i); }
  AuWriter &value(long long int i)      { return IntSigned(i); }
  AuWriter &value(long long unsigned i) { return IntUnsigned(i); }

  template<class T>
  AuWriter &value(T f,
                  typename std::enable_if<std::is_floating_point<T>::value>::type * = nullptr) {
    double d = static_cast<double>(f);
    static_assert(sizeof(d) == 8);
    msgBuf_.put(marker::Double);
    auto *dPtr = reinterpret_cast<char *>(&d);
    msgBuf_.write(dPtr, sizeof(d));
    return *this;
  }

  AuWriter &nanos(uint64_t n) {
    msgBuf_.put(marker::Timestamp);
    auto *dPtr = reinterpret_cast<char *>(&n);
    msgBuf_.write(dPtr, sizeof(n));
    return *this;
  }

  /** Time points are converted to nanos since UNIX epoch. */
  template <class Clock, class Duration>
  AuWriter &value(const std::chrono::time_point<Clock, Duration> &tp) {
    using namespace std::chrono;

    // Note: system_clock will be UNIX epoch based in C++20
    std::chrono::time_point<system_clock, Duration> unixT0;
    auto nanoDuration = duration_cast<nanoseconds>(tp - unixT0);
    return nanos(static_cast<uint64_t>(nanoDuration.count()));
  }

  template<typename T>
  AuWriter &value(T val,
                     typename std::enable_if<std::is_enum<T>::value, void>::type * = 0) {
    return value(name(val));
  }

  template<typename T>
  typename std::enable_if<HasWriteAu<T>::value, void>::type
  value(const T &val) { val.writeAu(*this); }

  template<typename F>
  typename std::enable_if<HasOperatorApply<F>::value, void>::type
  value(F &&func) { func(); }

protected:
  friend AuEncoder;
  void raw(char c) {
    msgBuf_.put(c);
  }

  void backref(uint32_t val) {
    auto *iPtr = reinterpret_cast<char *>(&val);
    msgBuf_.write(iPtr, sizeof(val));
  }

  void valueInt(uint64_t i) {
    if (i < (1ul << 7)) {
      msgBuf_.put(static_cast<char>(i));
    } else if (i < (1ul << 14)) {
      auto ptr = msgBuf_.raw(2);
      ptr[0] = static_cast<char>((i & 0x7fu) | 0x80u);
      ptr[1] = static_cast<char>(i >> 7);
    } else if (i < (1ul << 21)) {
      auto ptr = msgBuf_.raw(3);
      ptr[0] = static_cast<char>((i & 0x7fu) | 0x80u);
      ptr[1] = static_cast<char>(((i >> 7) & 0x7fu) | 0x80u);
      ptr[2] = static_cast<char>(i >> 14);
    } else if (i < (1ul << 28)) {
      auto ptr = msgBuf_.raw(4);
      ptr[0] = static_cast<char>((i & 0x7fu) | 0x80u);
      ptr[1] = static_cast<char>(((i >> 7) & 0x7fu) | 0x80u);
      ptr[2] = static_cast<char>(((i >> 14) & 0x7fu) | 0x80u);
      ptr[3] = static_cast<char>(i >> 21);
    } else if (i < (1ul << 35)) {
      auto ptr = msgBuf_.raw(5);
      ptr[0] = static_cast<char>((i & 0x7fu) | 0x80u);
      ptr[1] = static_cast<char>(((i >> 7) & 0x7fu) | 0x80u);
      ptr[2] = static_cast<char>(((i >> 14) & 0x7fu) | 0x80u);
      ptr[3] = static_cast<char>(((i >> 21) & 0x7fu) | 0x80u);
      ptr[4] = static_cast<char>(i >> 28);
    } else if (i < (1ul << 42)) {
      auto ptr = msgBuf_.raw(6);
      ptr[0] = static_cast<char>((i & 0x7fu) | 0x80u);
      ptr[1] = static_cast<char>(((i >> 7) & 0x7fu) | 0x80u);
      ptr[2] = static_cast<char>(((i >> 14) & 0x7fu) | 0x80u);
      ptr[3] = static_cast<char>(((i >> 21) & 0x7fu) | 0x80u);
      ptr[4] = static_cast<char>(((i >> 28) & 0x7fu) | 0x80u);
      ptr[5] = static_cast<char>(i >> 35);
    } else if (i < (1ul << 49)) {
      auto ptr = msgBuf_.raw(7);
      ptr[0] = static_cast<char>((i & 0x7fu) | 0x80u);
      ptr[1] = static_cast<char>(((i >> 7) & 0x7fu) | 0x80u);
      ptr[2] = static_cast<char>(((i >> 14) & 0x7fu) | 0x80u);
      ptr[3] = static_cast<char>(((i >> 21) & 0x7fu) | 0x80u);
      ptr[4] = static_cast<char>(((i >> 28) & 0x7fu) | 0x80u);
      ptr[5] = static_cast<char>(((i >> 35) & 0x7fu) | 0x80u);
      ptr[6] = static_cast<char>(i >> 42);
    } else {
      // above this we need at least 8 bytes. should not have called this
      // function but we use the general formula...
      while (true) {
        char toWrite = static_cast<char>(i & 0x7fu);
        i >>= 7;
        if (i) {
          msgBuf_.put((toWrite | static_cast<char>(0x80u)));
        } else {
          msgBuf_.put(toWrite);
          break;
        }
      }
    }
  }

  void term() {
    msgBuf_.put(marker::RecordEnd);
    msgBuf_.put('\n');
  }

private:
  void kvs() {}
  template<typename V, typename... Args>
  void kvs(std::string_view key, V &&val, Args &&... args) {
    this->key(key);
    value(std::forward<V>(val));
    kvs(std::forward<Args>(args)...);
  }

  void vals() {}
  template<typename V, typename... Args>
  void vals(V &&val, Args &&... args) {
    value(std::forward<V>(val));
    vals(std::forward<Args>(args)...);
  }

  // TODO: Split into IntUnsigned/Signed
  template<class T>
  AuWriter &
  auInt(T i, typename std::enable_if<std::is_integral<T>::value>::type * = nullptr) {
    if constexpr (std::is_signed_v<T>) {
      if (i >= 0 && i < 32) {
        msgBuf_.put(static_cast<char>(marker::SmallInt::Positive | i));
        return *this;
      }
      if (i < 0 && i > -32) {
        msgBuf_.put(static_cast<char>(marker::SmallInt::Negative | -i));
        return *this;
      }
      bool neg = false;
      uint64_t val = static_cast<uint64_t>(i);
      if (i < 0) {
        val = static_cast<uint64_t>(-i);
        neg = true;
      }
      if (val >= 1ull << 48) {
        msgBuf_.put(neg ? marker::NegInt64 : marker::PosInt64);
        msgBuf_.write(reinterpret_cast<char *>(&val), sizeof(val));
        return *this;
      }
      msgBuf_.put(neg ? marker::NegVarint : marker::Varint);
      valueInt(static_cast<typename std::make_unsigned<T>::type>(val));
    } else {
      if (i < 32) {
        msgBuf_.put(static_cast<char>(marker::SmallInt::Positive | i));
      } else if (i >= 1ull << 48) {
        msgBuf_.put(marker::PosInt64);
        uint64_t val = i;
        msgBuf_.write(reinterpret_cast<char *>(&val), sizeof(val));
      } else {
        msgBuf_.put(marker::Varint);
        valueInt(i);
      }
    }
    return *this;
  }

  AuWriter &IntSigned(int64_t i) { return auInt(i); }
  AuWriter &IntUnsigned(uint64_t i) { return auInt(i); }
};

class AuEncoder {
  static constexpr uint32_t AU_FORMAT_VERSION
      = FormatVersion1::AU_FORMAT_VERSION;
  AuStringIntern stringIntern_;
  AuVectorBuffer dictBuf_;
  AuVectorBuffer buf_;
  size_t backref_;
  size_t lastDictSize_;
  size_t records_;
  size_t purgeInterval_;
  size_t purgeThreshold_;
  size_t reindexInterval_;
  size_t clearThreshold_;

  void exportDict() {
    auto &dict = stringIntern_.dict();
    if (dict.size() > lastDictSize_) {
      auto sor = dictBuf_.tellp();
      AuWriter af(dictBuf_, stringIntern_);
      af.raw('A');
      af.backref(static_cast<uint32_t>(backref_)); // TODO do we guarantee elsewhere that this is never allowed to exceed 32 bits?
      for (size_t i = lastDictSize_; i < dict.size(); ++i) {
        auto &s = dict[i];
        af.value(std::string_view(s.c_str(), s.length()), false);
      }
      af.term();
      backref_ = dictBuf_.tellp() - sor;
      lastDictSize_ = dict.size();
    }
  }

  template <typename F>
  ssize_t finalizeAndWrite(F &&write) {
    exportDict();
    auto sor = dictBuf_.tellp();
    AuWriter af(dictBuf_, stringIntern_);
    af.raw('V');
    af.backref(static_cast<uint32_t>(backref_));  // TODO as above, do we guarantee elsewhere that this is never allowed to exceed 32 bits?
    af.valueInt(buf_.tellp());
    backref_ += dictBuf_.tellp() - sor;

    auto result = write(dictBuf_.str(), buf_.str());

    records_++;
    backref_ += buf_.tellp();

    buf_.clear();
    dictBuf_.clear();

    if (reindexInterval_ && (records_ % reindexInterval_ == 0)) {
      reIndexDictionary(purgeThreshold_);
    }

    if (purgeInterval_ && (records_ % purgeInterval_ == 0) && lastDictSize_) {
      purgeDictionary(purgeThreshold_);
    }

    if (lastDictSize_ > clearThreshold_) {
      clearDictionary(true);
    }

    return static_cast<ssize_t>(result);
  }

public:

  /**
   * @param metadata Metadata string to write in the header record. Values
   * longer than 16k will be truncated.
   * @param purgeInterval The dictionary will be purged after this many records.
   * A value of 0 means "never".
   * @param purgeThreshold Entries with a count less than this will be purged
   * when a purge or reindex is done.
   * @param reindexInterval The dictionary will be reindexed after this many
   * records. A value of 0 means "never". A re-index involves a purge.
   * @param clearThreshold When the dictionary grows beyond this size, it will
   * be cleared. Large dictionaries slow down encoding.
   */
  AuEncoder(std::string metadata = std::string{},
            size_t purgeInterval = 250'000,
            size_t purgeThreshold = 50,
            size_t reindexInterval = 500'000)
      : AuEncoder(
          std::move(metadata),
          purgeInterval,
          purgeThreshold,
          reindexInterval,
          AuStringIntern::Config{})
  {}
  AuEncoder(std::string metadata,
            size_t purgeInterval,
            size_t purgeThreshold,
            size_t reindexInterval,
            AuStringIntern::Config stringInternConfig)
      : stringIntern_(stringInternConfig),
        backref_(0), lastDictSize_(0), records_(0),
        purgeInterval_(purgeInterval),
        purgeThreshold_(purgeThreshold),
        reindexInterval_(reindexInterval),
        clearThreshold_(stringInternConfig.clearThreshold)
  {
    if (metadata.size() > FormatVersion1::MAX_METADATA_SIZE)
      metadata.resize(FormatVersion1::MAX_METADATA_SIZE);
    AuWriter af(dictBuf_, stringIntern_);
    af.raw('H');
    af.raw('A');
    af.raw('U');
    af.value(AU_FORMAT_VERSION);
    af.value(metadata, false);
    af.term();
    clearDictionary();
  }

  /**
   * Encodes a single JSON object-like record (scalar, string, vector, or
   * map/object). Call this function multiple times to encode more than one
   * record.
   *
   *
   * @tparam F A function that takes a AuWriter reference
   * @tparam W A function that takes 2 string_view args
   * @param f This function should use the AuWriter to write/encode a record.
   * This is the main interface for writing Au data. You are given an AuWriter
   * that you can use as needed to create a (single) record. In other words, you
   * must only call one of the writer's functions at the top level of this
   * function.
   * @param write This function should persist the 2 string_view's provided (in
   * order).
   * @return
   */
  template<typename F, typename W>
  ssize_t encode(F &&f, W &&write) {
    ssize_t result = 0;
    AuWriter writer(buf_, stringIntern_);
    f(writer);
    if (buf_.tellp() != 0) {
      writer.term();
      result = finalizeAndWrite(write);
    }
    return result;
  }

  void clearDictionary(bool clearUsageTracker = false) {
    stringIntern_.clear(clearUsageTracker);
    emitDictClear();
  }

  /// Removes strings that are used less than "threshold" times from the hash
  void purgeDictionary(size_t threshold) {
    stringIntern_.purge(threshold);
  }

  /// Purges the dictionary and re-indexes the remaining entries so the more
  /// frequent ones are at the beginning (and have smaller indices).
  void reIndexDictionary(size_t threshold) {
    stringIntern_.reIndex(threshold);
    emitDictClear();
  }

  auto getStats() const {
    auto stats = stringIntern_.getStats();
    stats["Records"] = static_cast<int>(records_);
    return stats;
  }

private:
  void emitDictClear() {
    lastDictSize_ = 0;
    auto sor = dictBuf_.tellp();
    AuWriter af(dictBuf_, stringIntern_);
    af.raw('C');
    af.value(AU_FORMAT_VERSION);
    af.term();
    backref_ = dictBuf_.tellp() - sor;
  }
};

}
