#pragma once

#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

class Au;

class AuStringIntern {

  class UsageTracker {
    using InOrder = std::list<std::string>;
    InOrder inOrder;

    using DictVal = std::pair<size_t, InOrder::iterator>;
    using Dict = std::unordered_map<std::string_view, DictVal>;
    Dict dict;

    void pop(Dict::iterator it) {
      if (it != dict.end()) {
        auto listIt = it->second.second;
        dict.erase(it);
        inOrder.erase(listIt);
      }
    }

  public:
    const size_t INTERN_THRESH;
    const size_t INTERN_CACHE_SIZE;

    UsageTracker(size_t internThresh, size_t internCacheSize)
        : INTERN_THRESH(internThresh), INTERN_CACHE_SIZE(internCacheSize) {}

    bool shouldIntern(const std::string &str) {
      auto it = dict.find(std::string_view(str.c_str(), str.length()));
      if (it != dict.end()) {
        if (it->second.first >= INTERN_THRESH) {
          pop(it);
          return true;
        } else {
          it->second.first++;
          return false;
        }
      } else {
        if (inOrder.size() >= INTERN_CACHE_SIZE) {
          const auto &s = inOrder.front();
          pop(dict.find(std::string_view(s.c_str(), s.length())));
        }
        inOrder.emplace_back(str);
        const auto &s = inOrder.back();
        std::string_view sv(s.c_str(), s.length());
        dict[sv] = {size_t(1), --(inOrder.end())};
        return false;
      }
    }

    void clear() {
      dict.clear();
      inOrder.clear();
    }

    size_t size() const {
      return dict.size();
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

template<typename Buffer>
class AuFormatter {
  Buffer &msgBuf_;
  AuStringIntern &stringIntern;

  void encodeString(const std::string_view sv) {
    msgBuf_.put('S');
    valueInt(sv.length());
    msgBuf_.write(sv.data(), static_cast<std::streamsize>(sv.length()));
  }

  void encodeStringIntern(const std::string_view sv,
                          std::optional<bool> intern) {
    auto idx = stringIntern.idx(sv, intern);
    if (!idx) {
      encodeString(sv);
    } else {
      msgBuf_.put('X');
      valueInt(*idx);
    }
  }

  template<typename O>
  class HasOperatorApply {
    template<typename OO>
    static auto test(int)
    -> decltype(&OO::operator(), std::true_type());

    template<typename>
    static auto test(...) -> std::false_type;

  public:
    static constexpr bool value = decltype(test<O>(0))
    ::value;
  };

public:
  AuFormatter(Buffer &buf, AuStringIntern &stringIntern)
      : msgBuf_(buf), stringIntern(stringIntern) {}

  class KeyValSink {
    AuFormatter &formatter_;
    KeyValSink(AuFormatter &formatter) : formatter_(formatter) {}
    friend AuFormatter;
  public:
    template<typename V>
    void operator()(std::string_view key, V &&val) {
      formatter_.kvs(key, std::forward<V>(val));
    }
  };

  template<typename... Args>
  AuFormatter &map(Args &&... args) {
    msgBuf_.put('{');
    kvs(std::forward<Args>(args)...);
    msgBuf_.put('}');
    return *this;
  }

  template<typename... Args>
  AuFormatter &array(Args &&... args) {
    msgBuf_.put('[');
    vals(std::forward<Args>(args)...);
    msgBuf_.put(']');
    return *this;
  }

  template<typename F>
  auto mapVals(F &&f) {
    return [this, f] {
      KeyValSink sink(*this);
      msgBuf_.put('{');
      f(sink);
      msgBuf_.put('}');
    };
  }

  template<typename F>
  auto arrayVals(F &&f) {
    return [this, f] {
      msgBuf_.put('[');
      f();
      msgBuf_.put(']');
    };
  }

  // Interface to support SAX handlers
  AuFormatter &startMap() {
    msgBuf_.put('{');
    return *this;
  }
  AuFormatter &endMap() {
    msgBuf_.put('}');
    return *this;
  }
  AuFormatter &startArray() {
    msgBuf_.put('[');
    return *this;
  }
  AuFormatter &endArray() {
    msgBuf_.put(']');
    return *this;
  }

  template<class T>
  AuFormatter &
  value(T i, typename std::enable_if<std::is_integral<T>::value>::type * = nullptr) {
    if constexpr (std::is_signed_v<T>) {
      msgBuf_.put(i < 0 ? 'J' : 'I');
      if (i < 0) { i *= -1; }
      valueInt(static_cast<typename std::make_unsigned<T>::type>(i));
    } else {
      msgBuf_.put('I');
      valueInt(i);
    }
    return *this;
  }

  template<class T>
  AuFormatter &value(T f,
                     typename std::enable_if<std::is_floating_point<T>::value>::type * = nullptr) {
    double d = static_cast<double>(f); // TODO should we support a float format?
    static_assert(sizeof(d) == 8);
    msgBuf_.put('D');
    auto *dPtr = reinterpret_cast<char *>(&d);
    msgBuf_.write(dPtr, sizeof(d));
    return *this;
  }

  /**
   * @param sv
   * @param intern If uninitialized, it will intern (or not) based on frequency
   * of the string. If true, it will force interning (subject to tiny string
   * limits). If false, it will force in-lining.
   * @return
   */
  AuFormatter &value(const std::string_view sv,
                     std::optional<bool> intern = std::optional < bool > ()) {
    if (intern.has_value() && !intern.value()) {
      encodeString(sv);
    } else {
      encodeStringIntern(sv, intern);
    }
    return *this;
  }

  AuFormatter &value(bool b) {
    msgBuf_.put(b ? 'T' : 'F');
    return *this;
  }

  AuFormatter &value(const char *s) { return value(std::string_view(s)); }
  AuFormatter &value(const std::string &s) {
    return value(std::string_view(s.c_str(), s.length()));
  }
  AuFormatter &null() {
    msgBuf_.put('N');
    return *this;
  }
  AuFormatter &value(std::nullptr_t) { return null(); }

  template<typename T>
  AuFormatter &value(const T *t) {
    if (t) value(*t);
    else null();
    return *this;
  }

  template<typename T>
  AuFormatter &value(const std::unique_ptr<T> &val) { return value(val.get()); }

  template<typename T>
  AuFormatter &value(const std::shared_ptr<T> &val) { return value(val.get()); }

  template<typename T>
  AuFormatter &value(T val,
                     typename std::enable_if<std::is_enum<T>::value, void>::type * = 0) {
    return value(name(val));
  }

  template<typename F>
  typename std::enable_if<HasOperatorApply<F>::value, void>::type
  value(F &&func) { func(); }

protected:
  friend Au;
  void raw(char c) {
    msgBuf_.put(c);
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
    msgBuf_.put('E');
    msgBuf_.put('\n');
  }

private:
  void kvs() {}
  template<typename V, typename... Args>
  void kvs(std::string_view key, V &&val, Args &&... args) {
    encodeStringIntern(key, true);
    value(std::forward<V>(val));
    kvs(std::forward<Args>(args)...);
  }

  void vals() {}
  template<typename V, typename... Args>
  void vals(V &&val, Args &&... args) {
    value(std::forward<V>(val));
    vals(std::forward<Args>(args)...);
  }
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


class Au {
  static constexpr unsigned FORMAT_VERSION = 1;
  std::ostream &output_;
  OutputTracker outputTracker_;
  AuStringIntern stringIntern_;
  size_t pos_;
  size_t lastDictLoc_;
  size_t lastDictSize_;
  size_t records_;
  size_t purgeInterval_;
  size_t purgeThreshold_;
  size_t clearThreshold_;

  size_t tell() const {
    return pos_;
  }

  void exportDict() {
    auto dict = stringIntern_.dict();
    if (dict.size() > lastDictSize_) {
      OutputTracker tracker(output_);
      AuFormatter af(output_, stringIntern_);
      auto newDictLoc = tell();
      af.raw('A');
      af.valueInt(newDictLoc - lastDictLoc_);
      lastDictLoc_ = newDictLoc;
      for (size_t i = lastDictSize_; i < dict.size(); ++i) {
        auto &s = dict[i];
        af.value(std::string_view(s.c_str(), s.length()), false);
      }
      af.term();
      pos_ += tracker.count();
      lastDictSize_ = dict.size();
    }
  }

  void write(const std::string &msg) {
    write(std::string_view(msg.c_str(), msg.size()));
  }

  void write(const std::string_view &msg) {
    exportDict();
    OutputTracker tracker(output_);
    AuFormatter af(output_, stringIntern_);
    auto thisLoc = tell();
    af.raw('V');
    af.valueInt(thisLoc - lastDictLoc_);
    af.valueInt(msg.length());
    output_ << msg;
    records_++;

    pos_ += tracker.count();

    if (records_ % purgeInterval_ == 0) {
      purgeDictionary(purgeThreshold_);
    }

    if (lastDictSize_ > clearThreshold_) {
      clearDictionary(true);
    }
  }

public:

  // Mimics std::ostringstream interface (except for str() and clear())
  class VectorBuffer {
    std::vector<char> v;
  public:
    VectorBuffer(size_t size = 1024) {
      v.reserve(size);
    }
    void put(char c) {
      v.push_back(c);
    }
    void write(const char *data, std::streamsize size) {
      v.insert(v.end(), data, data + size);
    }
    size_t tellp() {
      return v.size();
    }
    std::string_view str() {
      return std::string_view(v.data(), v.size());
    }
    void clear() {
      v.clear();
    }
  };

  /**
   * @param output
   * @param purgeInterval The dictionary will be purged after this many records
   * @param purgeThreshold Entries with a count less than this will be purged
   * @param clearThreshold When the dictionary grows beyond this size, it will
   * be cleared. Large dictionaries slow down encoding.
   */
  Au(std::ostream &output, size_t purgeInterval = 250'000,
     size_t purgeThreshold = 50, size_t clearThreshold = 1400)
      : output_(output), outputTracker_(output),
        pos_(0), lastDictSize_(0), records_(0),
        purgeInterval_(purgeInterval), purgeThreshold_(purgeThreshold),
        clearThreshold_(clearThreshold)
  {
    OutputTracker outputTracker(output_);
    AuFormatter af(output_, stringIntern_);
    af.raw('H');
    af.value(FORMAT_VERSION);
    af.term();
    pos_ += outputTracker.count();
    clearDictionary();
  }

  template<typename F>
  void encode(F &&f) {
    static VectorBuffer buf;
    AuFormatter formatter(buf, stringIntern_);
    f(formatter);
    if (buf.tellp() != 0) {
      formatter.term();
      write(buf.str());
    }
    buf.clear();
  }

  void clearDictionary(bool clearUsageTracker = false) {
    stringIntern_.clear(clearUsageTracker);
    lastDictSize_ = 0;
    lastDictLoc_ = tell();
    OutputTracker tracker(output_);
    AuFormatter af(output_, stringIntern_);
    af.raw('C');
    af.term();
    pos_ += tracker.count();
  }

  /// Removes strings that are used less than "threshold" times from the hash
  void purgeDictionary(size_t threshold) {
    stringIntern_.purge(threshold);
  }

  auto getStats() const {
    auto stats = stringIntern_.getStats();
    stats["Records"] = static_cast<int>(records_);
    return stats;
  }
};
