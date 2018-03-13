#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <cstddef>

// TODO add explicit 4-byte float support
// TODO add small int and small dict-ref support
// TODO add short string-length support (roll into 'S')
// TODO add position tracking and length validation

#define THROW(stuff) \
  do { \
    std::ostringstream _message; \
    _message << stuff; \
    throw parse_error(_message.str()); \
  } while (0)

#define THROW_RT(stuff) \
  do { \
    std::ostringstream _message; \
    _message << stuff; \
    throw std::runtime_error(_message.str()); \
  } while (0)

namespace {

struct parse_error : std::runtime_error {
  explicit parse_error(const std::string &what)
      : std::runtime_error(what) {}
};


class FileByteSource {
  static const auto BUFFER_SIZE = 256 * 1024;

  char buf_[BUFFER_SIZE]; //< Working buffer
  size_t pos_;  //< Current position in the underlying data stream
  char *cur_;   //< Current position in the working buffer
  char *limit_; //< End of the current working buffer
  int fd_;      //< Underlying data stream

public:
  explicit FileByteSource(const std::string &fname)
      : pos_(0), cur_(buf_), limit_(buf_) {
    if (fname == "-") {
      fd_ = fileno(stdin);
    } else {
      fd_ = ::open(fname.c_str(), O_RDONLY);
    }
    if (fd_ == -1)
      THROW_RT("open: " << strerror(errno) << " (" << fname << ")");
    ::posix_fadvise(fd_, 0, 0, 1);  // FDADVICE_SEQUENTIAL TODO report error?
    read();
  }

  FileByteSource(const FileByteSource &) = delete;
  FileByteSource(FileByteSource &&) = delete;
  FileByteSource &operator=(const FileByteSource &) = delete;
  FileByteSource &operator=(FileByteSource &&) = delete;

  ~FileByteSource() {
    close(fd_); // TODO report error?
  }

  /// Position in the underlying data stream
  size_t pos() const { return pos_; }

  class Byte {
    int value_;
  public:
    explicit Byte(char c) : value_(static_cast<uint8_t >(c)) {}
  private:
    Byte() : value_(-1) {}
  public:
    bool isEof() const { return value_ == -1; }
    char charValue() const {
      if (isEof()) throw std::runtime_error("Tried to get value of eof");
      return static_cast<char>(value_);
    }
    std::byte byteValue() const {
      if (isEof()) throw std::runtime_error("Tried to get value of eof");
      return static_cast<std::byte>(value_);
    }
    static Byte Eof() { return Byte(); }
    friend bool operator ==(Byte b, char c) {
      return b.value_ == c;
    }
    friend bool operator ==(Byte b, Byte c) {
      return b.value_ == c.value_;
    }
    friend bool operator !=(Byte b, char c) {
      return b.value_ != c;
    }
    friend bool operator !=(Byte b, Byte c) {
      return b.value_ != c.value_;
    }
    friend std::ostream &operator<<(std::ostream &o, Byte b) {
      if (b.isEof()) return o << "EOF";
      return o << '\'' << static_cast<char>(b.value_)
               << "' (0x" << std::hex << b.value_ << ")";
    }
  };

   Byte next() {
    while (cur_ == limit_) if (!read()) return Byte::Eof();
    pos_++;
    return Byte(*cur_++);
  }

  Byte peek() {
    while (cur_ == limit_) if (!read()) return Byte::Eof();
    return Byte(*cur_);
  }

  template<typename T>
  void read(T *t, size_t len) {
    char *buf = static_cast<char *>(static_cast<void *>(t));
    read(len, [&](std::string_view fragment) {
      ::memcpy(buf, fragment.data(), fragment.size());
      buf += fragment.size();
    });
  }

  template<typename F>
  void read(size_t len, F func) {
    while (len) {
      while (cur_ == limit_)
        if (!read())
          THROW("reached eof while trying to read " << len << " bytes");
      // limit_ > cur_, so cast to size_t is fine...
      auto first = std::min(len, static_cast<size_t>(limit_ - cur_));
      func(std::string_view(cur_, first));
      pos_ += first;
      cur_ += first;
      len -= first;
    }
  }

  void skip(size_t len) {
    read(len, [](std::string_view) {});
  }

  void seek(size_t abspos) {
    if (abspos > pos_)
      THROW_RT("seeking forward not supported");
    auto relseek = pos_ - abspos;
    if (relseek <= static_cast<size_t>(cur_ - buf_)) {
      cur_ -= relseek;
      pos_ -= relseek;
    } else {
      auto pos = lseek(fd_, static_cast<off_t>(abspos), SEEK_SET);
      if (pos < 0) {
        THROW_RT("failed to seek to desired location: " << strerror(errno));
      }
      cur_ = limit_ = buf_;
      pos_ = static_cast<size_t>(pos);
      if (!read())
        THROW_RT("failed to read from new location");
    }
  }

private:
  bool read() {
    // Keep a minimum amount of consumed data in the buffer so we can seek back
    // even in non-seekable data streams.
    const auto minHistSz = (BUFFER_SIZE / 16);
    if (cur_ > buf_ + minHistSz) {
      auto startOfHistory = cur_ - minHistSz;
      memmove(buf_, startOfHistory,
              static_cast<size_t>(limit_ - startOfHistory));
      auto shift = startOfHistory - buf_;
      cur_ -= shift;
      limit_ -= shift;
    }

    const auto freeSpace = BUFFER_SIZE - (limit_ - buf_);
    ssize_t bytes_read = ::read(fd_, limit_, static_cast<size_t>(freeSpace));
    if (bytes_read == -1)
      throw std::runtime_error("read failed");
    if (!bytes_read) return false;
    limit_ += bytes_read;
    return true;
  }
};

class BaseParser {
protected:
  FileByteSource &source_;

  explicit BaseParser(FileByteSource &source)
      : source_(source) {}

  void expect(char e) const {
    auto c = source_.next();
    if (c == e) return;
    THROW("Unexpected character: '" << c);
  }

  double readDouble() const {
    double val;
    static_assert(sizeof(val) == 8, "sizeof(double) must be 8");
    source_.read(&val, sizeof(val));
    return val;
  }

  uint64_t readVarint() const {
    auto shift = 0u;
    uint64_t result = 0;
    while (true) {
      if (shift >= 64u)
        THROW("Bad varint encoding");
      auto next = source_.next();
      if (next.isEof())
        THROW("Unexpected end of file");
      auto i = next.byteValue();
      const auto valueMask = std::byte(0x7f);
      const auto moreMask = std::byte(0x80);
      result |= static_cast<uint64_t>(i & valueMask) << shift;
      shift += 7;
      if ((i & moreMask) != moreMask) break;
    }
    return result;
  }

  template<typename Handler>
  void parseString(Handler &handler) const {
    auto len = readVarint();
    handler.onStringStart(len);
    source_.read(len, [&](std::string_view fragment) {
      handler.onStringFragment(fragment);
    });
    handler.onStringEnd();
  }
};

template<typename Handler>
class ValueParser : BaseParser {
  Handler &handler_;

public:
  ValueParser(FileByteSource &source, Handler &handler)
      : BaseParser(source), handler_(handler) {}

  void value() const {
    auto c = source_.next();
    if (c.isEof())
      THROW("Unexpected EOF at start of value");
    switch (c.charValue()) {
      case 'T':
        handler_.onBool(true);
        break;
      case 'F':
        handler_.onBool(false);
        break;
      case 'N':
        handler_.onNull();
        break;
      case 'I':
        handler_.onUint(readVarint());
        break;
      case 'J': {
        auto i = readVarint();
        if (i > std::numeric_limits<int64_t>::max() - 1)
          THROW("Signed int overflows int64_t: -" << i);
        handler_.onInt(-static_cast<int64_t>(i));
      }
        break;
      case 'D':
        handler_.onDouble(readDouble());
        break;
      case 'X':
        handler_.onDictRef(readVarint());
        break;
      case 'S':
        parseString(handler_);
        break;
      case '[':
        parseArray();
        break;
      case '{':
        parseObject();
        break;
      default:
        THROW("Unexpected character at start of value: " << c);
    }
  }

private:
  void key() const {
    auto c = source_.peek();
    if (c.isEof())
      THROW("Unexpected EOF at start of key");
    switch (c.charValue()) {
      case 'S':
      case 'X':
        value();
        break;
      default:
        THROW("Unexpected character at start of key: " << c);
    }
  }
  void parseArray() const {
    handler_.onArrayStart();
    while (source_.peek() != ']') value();
    expect(']');
    handler_.onArrayEnd();
  }

  void parseObject() const {
    handler_.onObjectStart();
    while (source_.peek() != '}') {
      key();
      value();
    }
    expect('}');
    handler_.onObjectEnd();
  }
};

template<typename Handler>
class RecordParser : BaseParser {
  static constexpr int AU_FORMAT_VERSION = 1;
  Handler &handler_;

public:
  RecordParser(FileByteSource &source, Handler &handler)
      : BaseParser(source), handler_(handler) {}

  void parseStream() const {
    while (source_.peek() != EOF) record();
  }

private:
  void record() const {
    auto c = source_.next();
    if (c.isEof()) THROW("Unexpected EOF at start of record");
    handler_.onRecordStart(source_.pos() - 1);
    switch (c.charValue()) {
      case 'H':
        if (source_.next() == 'I') {
          auto version = readVarint();
          if (version != AU_FORMAT_VERSION) {
            THROW("Bad format version: expected " << AU_FORMAT_VERSION
                                                  << ", got " << version);
          }
        } else {
          THROW("Expected version number"); // TODO
        }
        term();
        break;
      case 'C':
        term();
        handler_.onDictClear();
        break;
      case 'A': {
        auto backref = readVarint();
        handler_.onDictAddStart(backref);
        while (source_.peek() != 'E') {
          if (source_.next() != 'S')
            THROW("Expected a string"); // TODO
          parseString(handler_);
        }
        term();
        break;
      }
      case 'V': {
        auto backref = readVarint();
        auto len = readVarint();
        auto startOfValue = source_.pos();
        handler_.onValue(backref, len - 2, source_);
        term();
        if (source_.pos() - startOfValue != len)
          THROW(
              "could be a parse error, or internal error: value handler didn't skip value!");
        break;
      }
      default:
        THROW("Unexpected character at start of record: " << c);
    }
  }

  void term() const {
    expect('E');
    expect('\n');
  }
};

}

struct NoopValueHandler {
  virtual ~NoopValueHandler() = default;

  virtual void onObjectStart() {}
  virtual void onObjectEnd() {}
  virtual void onArrayStart() {}
  virtual void onArrayEnd() {}
  virtual void onNull() {}
  virtual void onBool(bool) {}
  virtual void onInt(int64_t) {}
  virtual void onUint(uint64_t) {}
  virtual void onDouble(double) {}
  virtual void onDictRef(size_t) {}
  virtual void onStringStart(size_t) {}
  virtual void onStringEnd() {}
  virtual void onStringFragment(std::string_view) {}
};

struct NoopRecordHandler {
  virtual ~NoopRecordHandler() = default;

  virtual void onRecordStart([[maybe_unused]] size_t absPos) {}
  virtual void onValue([[maybe_unused]]size_t relDictPos, size_t len,
                       FileByteSource &source) {
    source.skip(len);
  }
  virtual void onDictClear() {}
  virtual void onDictAddStart([[maybe_unused]] size_t relDictPos) {}
  virtual void onStringStart([[maybe_unused]] size_t strLen) {}
  virtual void onStringEnd() {}
  virtual void onStringFragment([[maybe_unused]] std::string_view fragment) {}
  virtual void onParseEnd() {}
};

class AuDecoder {
  std::string filename_;

public:
  AuDecoder(const std::string &filename)
      : filename_(filename) {}

  template<typename H>
  void decode(H &handler) const {
    FileByteSource source(filename_);
    try {
      RecordParser<H>(source, handler).parseStream();
      handler.onParseEnd();
    } catch (parse_error &e) {
      std::cout << e.what() << std::endl;
    }
  }
};
