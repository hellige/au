#pragma once

#include "au/AuCommon.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <stdio.h>

class AuEncoder;

class AuStringIntern {

  class UsageTracker {
    using InOrder = std::list<std::string>;
    InOrder inOrder_;

    using DictVal = std::pair<size_t, InOrder::iterator>;
    using Dict = std::unordered_map<std::string_view, DictVal>;
    Dict dict_;

    void pop(Dict::iterator it) {
      if (it != dict_.end()) {
        auto listIt = it->second.second;
        dict_.erase(it);
        inOrder_.erase(listIt);
      }
    }

  public:
    const size_t INTERN_THRESH;
    const size_t INTERN_CACHE_SIZE;

    UsageTracker(size_t internThresh, size_t internCacheSize)
        : INTERN_THRESH(internThresh), INTERN_CACHE_SIZE(internCacheSize) {}

    bool shouldIntern(const std::string &str) {
      auto it = dict_.find(std::string_view(str.c_str(), str.length()));
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
          const auto &s = inOrder_.front();
          pop(dict_.find(std::string_view(s.c_str(), s.length())));
        }
        inOrder_.emplace_back(str);
        const auto &s = inOrder_.back();
        std::string_view sv(s.c_str(), s.length());
        dict_[sv] = {size_t(1), --(inOrder_.end())};
        return false;
      }
    }

    void clear() {
      dict_.clear();
      inOrder_.clear();
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
  std::unordered_map<std::string, InternEntry> dictionary_;
  size_t nextEntry_;
  const size_t TINY_STR;
  UsageTracker internCache_;

public:

  explicit AuStringIntern(size_t tinyStr = 4, size_t internThresh = 10,
                          size_t internCacheSize = 1000)
      : nextEntry_(0), TINY_STR(tinyStr),
        internCache_(internThresh, internCacheSize) {}

  std::optional<size_t> idx(std::string s, std::optional<bool> intern) {
    if (s.length() <= TINY_STR) return std::optional < size_t > ();
    if (intern.has_value() && !intern.value())
      return std::optional < size_t > ();

    auto it = dictionary_.find(s);
    if (it != dictionary_.end()) {
      it->second.occurences++;
      return it->second.internIndex;
    }

    bool forceIntern = intern.has_value() && intern.value();
    if (forceIntern || internCache_.shouldIntern(s)) {
      dictionary_[s] = {nextEntry_, 1};
      dictInOrder_.emplace_back(s);
      return nextEntry_++;
    } else {
      return std::optional < size_t > ();
    }
  }

  auto idx(std::string_view sv, std::optional<bool> intern) {
    return idx(std::string(sv), intern);
  }

  const std::vector<std::string> &dict() const { return dictInOrder_; }

  void clear(bool clearUsageTracker) {
    dictionary_.clear();
    dictInOrder_.clear();
    nextEntry_ = 0;
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

  /// Purges the dictionary and re-idexes the remaining entries so the more
  /// frequent ones are at the beginning (and have smaller indices).
  size_t reIndex(size_t threshold) {
    size_t purged = purge(threshold);

    dictInOrder_.clear();
    dictInOrder_.reserve(dictionary_.size());
    for (auto &pr : dictionary_) {
      dictInOrder_.emplace_back(pr.first);
    }

    std::sort(dictInOrder_.begin(), dictInOrder_.end(),
              [this](const auto &a, const auto &b) {
                // Invert comparison b/c we want frequent strings first
                return dictionary_[a].occurences > dictionary_[b].occurences;
              });
    size_t idx = 0;
    for (auto &v : dictInOrder_) {
      dictionary_[v].internIndex = idx++;
    }

    nextEntry_ = dictInOrder_.size();
    return purged;
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

// Mimics std::ostringstream interface (except for str() and clear())
class AuBuffer {
public:
  virtual ~AuBuffer() = default;

  virtual void put(char c) = 0;
  virtual void write(const char *data, size_t size) = 0;
  virtual size_t tellp() = 0;
  virtual std::string_view str() = 0;
  virtual void clear() {};
};

class AuWriter {
  AuBuffer &msgBuf_;
  AuStringIntern &stringIntern_;

  void encodeString(const std::string_view sv) {
    if (sv.length() < 32) {
      msgBuf_.put(0x20 | sv.length());
    } else {
      msgBuf_.put(marker::String);
      valueInt(sv.length());
    }
    msgBuf_.write(sv.data(), static_cast<std::streamsize>(sv.length()));
  }

  void encodeStringIntern(const std::string_view sv,
                          std::optional<bool> intern) {
    auto idx = stringIntern_.idx(sv, intern);
    if (!idx) {
      encodeString(sv);
    } else if (*idx < 0x80) {
      msgBuf_.put(0x80 | *idx);
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
  AuWriter(AuBuffer &buf, AuStringIntern &stringIntern)
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
    encodeStringIntern(key, true);
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
                  std::optional<bool> intern = std::optional < bool > ()) {
    if (intern.has_value() && !intern.value()) {
      encodeString(sv);
    } else {
      encodeStringIntern(sv, intern);
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

  AuWriter &value(int i)          { return IntSigned(i); }
  AuWriter &value(unsigned int i) { return IntUnsigned(i); }
  AuWriter &value(int64_t i)      { return IntSigned(i); }
  AuWriter &value(uint64_t i)     { return IntUnsigned(i); }
  AuWriter &value(long long i)    { return IntSigned(i); }

  template<class T>
  AuWriter &value(T f,
                  typename std::enable_if<std::is_floating_point<T>::value>::type * = nullptr) {
    double d = static_cast<double>(f); // TODO should we support a float format?
    static_assert(sizeof(d) == 8);
    msgBuf_.put(marker::Double);
    auto *dPtr = reinterpret_cast<char *>(&d);
    msgBuf_.write(dPtr, sizeof(d));
    return *this;
  }

  template <class Rep, class Period>
  AuWriter &value(const std::chrono::duration<Rep, Period> &time) {
    using namespace std::chrono;
    auto nanoDuration = duration_cast<nanoseconds>(time);
    uint64_t nanos = static_cast<uint64_t>(nanoDuration.count());

    msgBuf_.put(marker::Timestamp);
    auto *dPtr = reinterpret_cast<char *>(&nanos);
    msgBuf_.write(dPtr, sizeof(nanos));
    return *this;
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
        msgBuf_.put(0x60 | i);
        return *this;
      }
      if (i < 0 && i > -32) {
        msgBuf_.put(0x40 | -i);
        return *this;
      }
      bool neg = false;
      uint64_t val = i;
      if (i < 0) {
        val = -i;
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
        msgBuf_.put(0x60 | i);
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


/// Need to keep track of bytes written to both files and cout
class OutputTracker : public std::streambuf {
  std::streambuf *dest_;
  std::ostream *owner_;
  size_t count_;
  using int_type = typename std::streambuf::int_type;
  using char_type = typename std::streambuf::char_type;
  using traits_type = typename std::streambuf::traits_type;

public:
  explicit OutputTracker(std::streambuf *dest)
      : dest_(dest), owner_(nullptr), count_(0)
  {}

  explicit OutputTracker(std::ostream &dest)
      : dest_(dest.rdbuf()), owner_(&dest), count_(0)
  {
    owner_->rdbuf(this);
  }

  ~OutputTracker() override {
    if (owner_ != nullptr) {
      owner_->rdbuf(dest_);
    }
  }

  size_t count() const {
    return count_;
  }

  void reset() {
    count_ = 0;
  }

protected:
  virtual int_type overflow(int_type ch = traits_type::eof()) {
    count_++;
    return dest_->sputc(ch);
  }
};

class AuStreamBuffer : public AuBuffer {
  std::ostream &output_;
  OutputTracker tracker_;
public:
  AuStreamBuffer(std::ostream &output) : output_(output), tracker_(output) {}
  void put(char c) override { output_.put(c); }
  void write(const char *data, size_t size) override {
    output_.write(data, size);
  }
  size_t tellp() override { return output_.tellp(); }
  std::string_view str() override { throw std::runtime_error("Not supported"); }
  void clear() override { throw std::runtime_error("Not supported"); }

  OutputTracker &tracker() { return tracker_; }
};

class AuVectorBuffer : public AuBuffer {
  std::vector<char> v;
public:
  AuVectorBuffer(size_t size = 1024) {
    v.reserve(size);
  }
  void put(char c) override {
    v.push_back(c);
  }
  void write(const char *data, size_t size) override {
    v.insert(v.end(), data, data + size);
  }
  size_t tellp() override {
    return v.size();
  }
  std::string_view str() override {
    return std::string_view(v.data(), v.size());
  }
  void clear() override {
    v.clear();
  }
};


class AuEncoder {
  static constexpr unsigned FORMAT_VERSION = 1;
  std::ostream &output_;
  AuStringIntern stringIntern_;
  AuVectorBuffer buf_;
  size_t pos_;
  size_t lastDictLoc_;
  size_t lastDictSize_;
  size_t records_;
  size_t purgeInterval_;
  size_t purgeThreshold_;
  size_t reindexInterval_;
  size_t clearThreshold_;

  size_t tell() const {
    return pos_;
  }

  void exportDict() {
    auto dict = stringIntern_.dict();
    if (dict.size() > lastDictSize_) {
      AuStreamBuffer formatterOutput(output_);
      AuWriter af(formatterOutput, stringIntern_);
      auto newDictLoc = tell();
      af.raw('A');
      af.backref(newDictLoc - lastDictLoc_);
      lastDictLoc_ = newDictLoc;
      for (size_t i = lastDictSize_; i < dict.size(); ++i) {
        auto &s = dict[i];
        af.value(std::string_view(s.c_str(), s.length()), false);
      }
      af.term();
      pos_ += formatterOutput.tracker().count();
      lastDictSize_ = dict.size();
    }
  }

  void write(const std::string &msg) {
    write(std::string_view(msg.c_str(), msg.size()));
  }

  void write(const std::string_view &msg) {
    exportDict();
    AuStreamBuffer formatterOutput(output_);
    AuWriter af(formatterOutput, stringIntern_);
    auto thisLoc = tell();
    af.raw('V');
    af.backref(thisLoc - lastDictLoc_);
    af.valueInt(msg.length());
    output_ << msg;
    records_++;

    pos_ += formatterOutput.tracker().count();

    if (reindexInterval_ && (records_ % reindexInterval_ == 0)) {
      reIndexDictionary(purgeThreshold_);
    }

    if (purgeInterval_ && (records_ % purgeInterval_ == 0) && lastDictSize_) {
      purgeDictionary(purgeThreshold_);
    }

    if (lastDictSize_ > clearThreshold_) {
      clearDictionary(true);
    }
  }

public:

  /**
   * @param output
   * @param purgeInterval The dictionary will be purged after this many records.
   * A value of 0 means "never".
   * @param purgeThreshold Entries with a count less than this will be purged
   * when a purge or reindex is done.
   * @param reindexInterval The dictionary will be reindexed after this many
   * records. A value of 0 means "never". A re-index involves a purge.
   * @param clearThreshold When the dictionary grows beyond this size, it will
   * be cleared. Large dictionaries slow down encoding.
   */
  AuEncoder(std::ostream &output, size_t purgeInterval = 250'000,
     size_t purgeThreshold = 50, size_t reindexInterval = 500'000,
     size_t clearThreshold = 1400)
      : output_(output),
        pos_(0), lastDictSize_(0), records_(0),
        purgeInterval_(purgeInterval), purgeThreshold_(purgeThreshold),
        reindexInterval_(reindexInterval), clearThreshold_(clearThreshold)
  {
    AuStreamBuffer formatterOutput(output_);
    AuWriter af(formatterOutput, stringIntern_);
    af.raw('H');
    af.value(FORMAT_VERSION);
    af.term();
    pos_ += formatterOutput.tracker().count();
    clearDictionary();
  }

  template<typename F>
  ssize_t encode(F &&f) {
    AuWriter writer(buf_, stringIntern_);
    f(writer);
    if (buf_.tellp() != 0) {
      writer.term();
      write(buf_.str());
    }
    buf_.clear();
    return 0; // TODO: Return bytes written or negative error code
  }

  AuWriter writer() {
    return AuWriter(buf_, stringIntern_);
  }

  // TODO: Maybe change to AuWriter.commit()
  ssize_t commit(AuWriter &writer) {
    if (buf_.tellp() != 0) {
      writer.term();
      write(buf_.str());
    }
    buf_.clear();
    return 0; // TODO: Return bytes written or negative error code
  }

  void clearDictionary(bool clearUsageTracker = false) {
    stringIntern_.clear(clearUsageTracker);
    lastDictSize_ = 0;
    lastDictLoc_ = tell();
    AuStreamBuffer formatterOutput(output_);
    AuWriter af(formatterOutput, stringIntern_);
    af.raw('C');
    af.term();
    pos_ += formatterOutput.tracker().count();
  }

  /// Removes strings that are used less than "threshold" times from the hash
  void purgeDictionary(size_t threshold) {
    stringIntern_.purge(threshold);
  }

  /// Purges the dictionary and re-idexes the remaining entries so the more
  /// frequent ones are at the beginning (and have smaller indices).
  void reIndexDictionary(size_t threshold) {
    stringIntern_.reIndex(threshold);
    lastDictSize_ = 0;
    lastDictLoc_ = tell();
    AuStreamBuffer formatterOutput(output_);
    AuWriter af(formatterOutput, stringIntern_);
    af.raw('C');
    af.term();
    pos_ += formatterOutput.tracker().count();
  }

  auto getStats() const {
    auto stats = stringIntern_.getStats();
    stats["Records"] = static_cast<int>(records_);
    return stats;
  }
};
