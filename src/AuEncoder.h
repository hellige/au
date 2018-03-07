#pragma once

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
    using Dict = std::unordered_map<std::string, DictVal>;
    Dict dict;

    void pop(Dict::iterator it) {
      if (it != dict.end()) {
        inOrder.erase(it->second.second);
        dict.erase(it);
      }
    }

  public:
    bool shouldIntern(const std::string &str) {
      auto it = dict.find(str);
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
          pop(dict.find(inOrder.front()));
        }
        inOrder.emplace_back(str);
        dict[str] = {size_t(1), --(inOrder.end())};
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

  std::vector<std::string> dictInOrder;
  /// The string and its intern index
  std::unordered_map<std::string, size_t> dictionary;
  UsageTracker internCache;
  size_t nextEntry;

public:
  static constexpr size_t TINY_STR = 4;
  static constexpr size_t INTERN_THRESH = 50;
  static constexpr size_t INTERN_CACHE_SIZE = 10000;

  AuStringIntern() : nextEntry(0) { }

  /// @return negative value means the string was not interned
  std::optional<size_t> idx(std::string s, std::optional<bool> intern) {
    if (s.length() <= TINY_STR) return std::optional<size_t>();
    if (intern.has_value() && !intern.value()) return std::optional<size_t>();

    auto it = dictionary.find(s);
    if (it != dictionary.end()) {
      return it->second;
    }

    bool forceIntern = intern.has_value() && intern.value();
    if (forceIntern || internCache.shouldIntern(s)) {
      dictionary[s] = nextEntry;
      dictInOrder.emplace_back(s);
      return nextEntry++;
    } else {
      return std::optional<size_t>();
    }
  }

  /// @return negative value means the string was not interned
  auto idx(std::string_view sv, std::optional<bool> intern) {
    return idx(std::string(sv), intern);
  }

  const std::vector<std::string> &dict() const { return dictInOrder; }

  void clear(bool clearUsageTracker) {
    dictionary.clear();
    dictInOrder.clear();
    nextEntry = 0;
    if (clearUsageTracker) internCache.clear();
  }

  // For debug/profiling
  size_t cacheSize() const {
    return internCache.size();
  }
};

class AuFormatter {
  std::ostream &msgBuf_;
  AuStringIntern &stringIntern;

  void encodeString(const std::string_view sv) {
    msgBuf_.put('S');
    valueInt(sv.length());
    msgBuf_.write(sv.data(), sv.length());
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
    static constexpr bool value = decltype(test<O>(0))::value;
  };

public:
  AuFormatter(std::ostream &os, AuStringIntern &stringIntern)
    : msgBuf_(os), stringIntern(stringIntern) { }

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
  AuFormatter &map(Args&&... args) {
    msgBuf_.put('{');
    kvs(std::forward<Args>(args)...);
    msgBuf_.put('}');
    return *this;
  }

  template<typename... Args>
  AuFormatter &array(Args&&... args) {
    msgBuf_.put('[');
    vals(std::forward<Args>(args)...);
    msgBuf_.put(']');
    return *this;
  }

  template <typename F>
  auto mapVals(F &&f) {
    return [this, f] {
      KeyValSink sink(*this);
      msgBuf_.put('{');
      f(sink);
      msgBuf_.put('}');
    };
  }

  template <typename F>
  auto arrayVals(F &&f) {
    return [this, f] {
      msgBuf_.put('[');
      f();
      msgBuf_.put(']');
    };
  }

  // Interface to support SAX handlers
  AuFormatter &startMap() { msgBuf_.put('{'); return *this; }
  AuFormatter &endMap() { msgBuf_.put('}'); return *this; }
  AuFormatter &startArray() { msgBuf_.put('['); return *this; }
  AuFormatter &endArray() { msgBuf_.put(']'); return *this; }

  template<class T>
  AuFormatter &value(T i, typename std::enable_if<std::is_integral<T>::value>::type * = 0) {
    msgBuf_.put(i < 0 ? 'J' : 'I');
    if (i < 0) { i *= -1; }
    valueInt(i);
    return *this;
  };

  template<class T>
  AuFormatter &value(T f, typename std::enable_if<std::is_floating_point<T>::value>::type * = 0) {
    double d = f;
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
                     std::optional<bool> intern = std::optional<bool>()) {
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
  AuFormatter &null() { msgBuf_.put('N'); return *this; }
  AuFormatter &value(std::nullptr_t) { return null(); }

  template <typename T>
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
      typename std::enable_if<std::is_enum<T>::value, void>::type * = 0)
  {
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
      uint8_t toWrite = i & 0x7f;
      i >>= 7;
      if (i) {
        msgBuf_.put(toWrite | 0x80);
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
  void kvs(std::string_view key, V &&val, Args&&... args) {
    encodeStringIntern(key, true);
    value(std::forward<V>(val));
    kvs(std::forward<Args>(args)...);
  }

  void vals() {}
  template<typename V, typename... Args>
  void vals(V &&val, Args&&... args) {
    value(std::forward<V>(val));
    vals(std::forward<Args>(args)...);
  }
};


class Au {
  static constexpr int FORMAT_VERSION = 1;
  std::ostream &output_;
  AuStringIntern stringIntern;
  size_t lastDictLoc;
  size_t lastDictSize;

  void exportDict() {
    auto dict = stringIntern.dict();
    if (dict.size() > lastDictSize) {
      AuFormatter au(output_, stringIntern);
      size_t newDictLoc = output_.tellp();
      au.raw('A');
      au.valueInt(newDictLoc - lastDictLoc);
      lastDictLoc = newDictLoc;
      for (size_t i = lastDictSize; i < dict.size(); ++i) {
        auto &s = dict[i];
        au.value(std::string_view(s.c_str(), s.length()), false);
      }
      au.term();
      lastDictSize = dict.size();
    }
  }

  void write(const std::string &msg) {
    exportDict();
    AuFormatter au(output_, stringIntern);
    size_t thisLoc = output_.tellp();
    au.raw('V');
    au.valueInt(thisLoc - lastDictLoc);
    au.valueInt(msg.length());
    output_ << msg;
  }

public:

  Au(std::ostream &output) : output_(output), lastDictSize(0) {
    AuFormatter af(output_, stringIntern);
    af.raw('H');
    af.value(FORMAT_VERSION);
    af.term();
    clearDictionary();
  }

  template <typename F>
  void encode(F &&f) {
    std::ostringstream os;
    AuFormatter formatter(os, stringIntern);
    f(formatter);
    if (os.tellp() != 0) {
      formatter.term();
      write(os.str());
    }
  }

  void clearDictionary(bool clearUsageTracker = false) {
    stringIntern.clear(clearUsageTracker);
    lastDictSize = 0;
    lastDictLoc = output_.tellp();
    AuFormatter af(output_, stringIntern);
    af.raw('C');
    af.term();
  }

  auto getStats() const {
    return std::unordered_map<std::string, int> {
        {"DictSize", stringIntern.dict().size()},
        {"CacheSize", stringIntern.cacheSize()}
    };
  }
};
