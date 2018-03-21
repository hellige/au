#pragma once

#include "au/ParseError.h"
#include "au/AuCommon.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <cstddef>
#include <chrono>

// TODO add explicit 4-byte float support
// TODO add small int and small dict-ref support
// TODO add short string-length support (roll into 'S')
// TODO add position tracking and length validation

class FileByteSource {
protected:
  const size_t BUFFER_SIZE;

  char *buf_;   //< Working buffer
  size_t pos_;  //< Current position in the underlying data stream
  char *cur_;   //< Current position in the working buffer
  char *limit_; //< End of the current working buffer
  int fd_;      //< Underlying data stream

  bool waitForData_;

public:
  explicit FileByteSource(const std::string &fname, bool waitForData,
                          size_t bufferSizeInK = 256)
      : BUFFER_SIZE(bufferSizeInK * 1024), buf_(new char[BUFFER_SIZE]),
        pos_(0), cur_(buf_), limit_(buf_), waitForData_(waitForData)
  {
    if (fname == "-") {
      fd_ = fileno(stdin);
    } else {
      fd_ = ::open(fname.c_str(), O_RDONLY);
    }
    if (fd_ == -1)
      THROW_RT("open: " << strerror(errno) << " (" << fname << ")");
#ifndef __APPLE__
    ::posix_fadvise(fd_, 0, 0, 1);  // FDADVICE_SEQUENTIAL TODO report error?
#endif
  }

  FileByteSource(const FileByteSource &) = delete;
  FileByteSource(FileByteSource &&) = delete;
  FileByteSource &operator=(const FileByteSource &) = delete;
  FileByteSource &operator=(FileByteSource &&) = delete;

  ~FileByteSource() {
    close(fd_); // TODO report error?
    delete[] buf_;
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

protected:
  /// Free space in the buffer
  size_t buffFree() const {
    return BUFFER_SIZE - (limit_ - buf_);
  }

  /// @return true if some data was read, false of 0 bytes were read.
  bool read() {
    return read(BUFFER_SIZE / 16);
  }
  bool read(size_t minHistSz) {
    // Keep a minimum amount of consumed data in the buffer so we can seek back
    // even in non-seekable data streams.
    //const auto minHistSz = (BUFFER_SIZE / 16);
    if (cur_ > buf_ + minHistSz) {
      auto startOfHistory = cur_ - minHistSz;
      memmove(buf_, startOfHistory,
              static_cast<size_t>(limit_ - startOfHistory));
      auto shift = startOfHistory - buf_;
      cur_ -= shift;
      limit_ -= shift;
    }

    ssize_t bytesRead = 0;
    do {
      bytesRead = ::read(fd_, limit_, buffFree());
      if (bytesRead < 0) // TODO: && errno != EAGAIN ?
        THROW_RT("Error reading file: " << strerror(errno));
      if (bytesRead == 0 && waitForData_)
        sleep(1);
    } while (!bytesRead && waitForData_);

    if (!bytesRead) return false;
    limit_ += bytesRead;
    return true;
  }
};

namespace {

class BaseParser {
protected:
  FileByteSource &source_;

  explicit BaseParser(FileByteSource &source)
      : source_(source) {}

  void expect(char e) const {
    auto c = source_.next();
    if (c == e) return;
    THROW("Unexpected character: " << c);
  }

  uint32_t readBackref() const {
    uint32_t val;
    source_.read(&val, sizeof(val));
    return val;
  }

  double readDouble() const {
    double val;
    static_assert(sizeof(val) == 8, "sizeof(double) must be 8");
    source_.read(&val, sizeof(val));
    return val;
  }

  std::chrono::nanoseconds readTime() const {
    uint64_t nanos;
    source_.read(&nanos, sizeof(nanos));
    std::chrono::nanoseconds n(nanos);
    return n;
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
  void parseString(size_t pos, size_t len, Handler &handler) const {
    handler.onStringStart(pos, len);
    source_.read(len, [&](std::string_view fragment) {
      handler.onStringFragment(fragment);
    });
    handler.onStringEnd();
  }

  template<typename Handler>
  void parseString(size_t pos, Handler &handler) const {
    auto len = readVarint();
    parseString(pos, len, handler);
  }

  void term() const {
    expect(marker::RecordEnd);
    expect('\n');
  }
};

template<typename Handler>
class ValueParser : BaseParser {
  Handler &handler_;
  /** A positive value that when multiplied by -1 represents the most negative
  number we support (std::numeric_limits<int64_t>::min() * -1). */
  static constexpr uint64_t NEG_INT_LIMIT =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;

public:
  ValueParser(FileByteSource &source, Handler &handler)
      : BaseParser(source), handler_(handler) {}

  void value() const {
    size_t sov = source_.pos();
    auto c = source_.next();
    if (c.isEof())
      THROW("Unexpected EOF at start of value");
    if (c.charValue() & 0x80) {
      handler_.onDictRef(sov, (uint8_t)c.charValue() & ~0x80);
      return;
    }
    int val = (uint8_t)c.charValue() & ~0xe0;
    if (c.charValue() & 0x40) {
      if (c.charValue() & 0x20)
        handler_.onUint(sov, val);
      else
        handler_.onInt(sov, -val);
      return;
    }
    if (c.charValue() & 0x20) {
      parseString(sov, val, handler_);
      return;
    }
    switch (c.charValue()) {
      case marker::True:
        handler_.onBool(sov, true);
        break;
      case marker::False:
        handler_.onBool(sov, false);
        break;
      case marker::Null:
        handler_.onNull(sov);
        break;
      case marker::Varint:
        handler_.onUint(sov, readVarint());
        break;
      case marker::NegVarint: {
        auto i = readVarint();
        if (i > NEG_INT_LIMIT) {
          THROW("Signed int overflows int64_t: (-)" << i << " 0x"
                << std::setfill('0') << std::setw(16) << std::hex << i);
        }
        handler_.onInt(sov, -static_cast<int64_t>(i));
        break;
      }
      case marker::PosInt64: {
        uint64_t val;
        source_.read(&val, sizeof(val));
        handler_.onUint(sov, val);
        break;
      }
      case marker::NegInt64: {
        uint64_t val;
        source_.read(&val, sizeof(val));
        if (val > NEG_INT_LIMIT) {
          THROW("Signed int overflows int64_t: (-)" << val << " 0x"
                << std::setfill('0') << std::setw(16) << std::hex << val);
        }
        handler_.onUint(sov, -static_cast<int64_t>(val));
        break;
      }
      case marker::Double:
        handler_.onDouble(sov, readDouble());
        break;
      case marker::Timestamp:
        handler_.onTime(sov, readTime());
        break;
      case marker::DictRef:
        handler_.onDictRef(sov, readVarint());
        break;
      case marker::String:
        parseString(sov, handler_);
        break;
      case marker::ArrayStart:
        parseArray();
        break;
      case marker::ObjectStart:
        parseObject();
        break;
      default:
        THROW("Unexpected character at start of value: " << c);
    }
  }

private:
  void key() const {
      // TODO clean up... this is ugly and redundant. also see the dict-add case
    size_t sov = source_.pos();
    auto c = source_.next();
    if (c.isEof())
      THROW("Unexpected EOF at start of key");
    if (c.charValue() & 0x80) {
      handler_.onDictRef(sov, (uint8_t)c.charValue() & ~0x80);
      return;
    }
    int val = (uint8_t)c.charValue() & ~0xe0;
    if ((c.charValue() & ~0x1f) == 0x20) {
      parseString(sov, val, handler_);
      return;
    }
    switch (c.charValue()) {
      case marker::DictRef:
        handler_.onDictRef(sov, readVarint());
        break;
      case marker::String:
        parseString(sov, handler_);
        break;
      default:
        THROW("Unexpected character at start of key: " << c);
    }
  }

  void parseArray() const {
    handler_.onArrayStart();
    while (source_.peek() != marker::ArrayEnd) value();
    expect(marker::ArrayEnd);
    handler_.onArrayEnd();
  }

  void parseObject() const {
    handler_.onObjectStart();
    while (source_.peek() != marker::ObjectEnd) {
      key();
      value();
    }
    expect(marker::ObjectEnd);
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
      case 'H': {
        uint64_t version;
        c = source_.next();
        if ((c.charValue() & ~0x1f) == 0x60) {
          version = c.charValue() & 0x1f;
        } else if (c == marker::Varint) {
          version = readVarint();
        } else {
          THROW("Expected version number"); // TODO
        }
        if (version != AU_FORMAT_VERSION) {
          THROW("Bad format version: expected " << AU_FORMAT_VERSION
                                                << ", got " << version);
        }
        handler_.onHeader(version);
        term();
        break;
      }
      case 'C':
        term();
        handler_.onDictClear();
        break;
      case 'A': {
        auto backref = readBackref();
        handler_.onDictAddStart(backref);
        while (source_.peek() != marker::RecordEnd) {
          size_t sov = source_.pos();
          c = source_.next();
          if (((uint8_t)c.charValue() & ~0x1fu) == 0x20) {
            parseString(sov, (uint8_t)c.charValue() & 0x1fu, handler_);
          } else if (c == marker::String) {
            parseString(sov, handler_);
          } else {
            THROW("Expected a string"); // TODO
          }
        }
        term();
        break;
      }
      case 'V': {
        auto backref = readBackref();
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
};

}

struct NoopValueHandler {
  virtual ~NoopValueHandler() = default;

  virtual void onObjectStart() {}
  virtual void onObjectEnd() {}
  virtual void onArrayStart() {}
  virtual void onArrayEnd() {}
  virtual void onNull([[maybe_unused]] size_t pos) {}
  virtual void onBool([[maybe_unused]] size_t pos, bool) {}
  virtual void onInt([[maybe_unused]] size_t pos, int64_t) {}
  virtual void onUint([[maybe_unused]] size_t pos, uint64_t) {}
  virtual void onDouble([[maybe_unused]] size_t pos, double) {}
  virtual void onTime([[maybe_unused]] size_t pos,
                      [[maybe_unused]] std::chrono::nanoseconds nanos) {}
  virtual void onDictRef([[maybe_unused]] size_t pos,
                         [[maybe_unused]] size_t dictIdx) {}
  virtual void onStringStart([[maybe_unused]] size_t sov,
                             [[maybe_unused]] size_t length) {}
  virtual void onStringEnd() {}
  virtual void onStringFragment([[maybe_unused]] std::string_view fragment) {}
};

struct NoopRecordHandler {
  virtual ~NoopRecordHandler() = default;

  virtual void onRecordStart([[maybe_unused]] size_t absPos) {}
  virtual void onValue([[maybe_unused]]size_t relDictPos, size_t len,
                       FileByteSource &source) {
    source.skip(len);
  }
  virtual void onHeader([[maybe_unused]] uint64_t version) {}
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
  void decode(H &handler, bool waitForData) const {
    FileByteSource source(filename_, waitForData);
    try {
      RecordParser<H>(source, handler).parseStream();
      handler.onParseEnd();
    } catch (parse_error &e) {
      std::cerr << e.what() << std::endl;
    }
  }
};
