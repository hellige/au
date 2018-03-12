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
  static const auto BUFFER_SIZE = 16*1024;

  char buf_[BUFFER_SIZE]; //< Working buffer
  int fd_;      //< Underlying data stream
  size_t pos_;  //< Current position in the underlying data stream
  char *cur_;   //< Current position in the working buffer
  char *limit_; //< End of the current working buffer

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

  int next() {
	while (cur_ == limit_) if (!read()) return EOF;
    pos_++;
    return *cur_++;
  }

  int peek() {
    while (cur_ == limit_) if (!read()) return EOF;
    return *cur_;
  }

  template <typename T>
  void read(T *t, size_t len) {
	char *buf = static_cast<char *>(static_cast<void *>(t));
    read(len, [&](std::string_view fragment) {
  	  ::memcpy(buf, fragment.data(), fragment.size());
	  buf += fragment.size();
    });
  }

  template <typename F>
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
      auto pos = lseek(fd_, abspos, SEEK_SET);
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
    const auto minHistSz = (BUFFER_SIZE / 4);
    if (cur_ > buf_ + minHistSz) {
      auto startOfHistory = cur_ - minHistSz;
      memmove(buf_, startOfHistory, limit_ - startOfHistory);
      auto shift = startOfHistory - buf_;
      cur_ -= shift;
      limit_ -= shift;
    }

    const auto freeSpace = BUFFER_SIZE - (limit_ - buf_);
    size_t bytes_read = ::read(fd_, limit_, freeSpace);
    if (bytes_read == (size_t)-1)
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
    int c = source_.next();
    if (c == e) return;
    THROW("Unexpected character: '"
          << (char)c << "' (0x" << std::hex << c << ")");
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
    while (true) { // TODO check that you're not reading more than 9 bytes
      uint64_t i = source_.next();// TODO what if the source is at the end?
      result |= (i & 0x7f) << shift;
      shift += 7;
      if (!(i & 0x80)) break;
    }
    return result;
  }

  template <typename Handler>
  void parseString(Handler &handler) const {
    auto len = readVarint();
    handler.onStringStart(len);
    source_.read(len, [&](std::string_view fragment) {
      handler.onStringFragment(fragment);
    });
    handler.onStringEnd();
  }
};

template <typename Handler>
class ValueParser : BaseParser {
  Handler &handler_;

public:
  ValueParser(FileByteSource &source, Handler &handler)
  : BaseParser(source), handler_(handler) {}

  void value() const {
	int c = source_.next();
	switch (c) {
	case 'T': handler_.onBool(true); break;
	case 'F': handler_.onBool(false); break;
	case 'N': handler_.onNull(); break;
	case 'I': handler_.onUint(readVarint()); break;
      case 'J': handler_.onInt(-readVarint()); break; // TODO this should check that readVarint() is not greater than signed int64 max
	case 'D': handler_.onDouble(readDouble()); break;
	case 'X': handler_.onDictRef(readVarint()); break;
      case 'S': parseString(handler_); break;
	case '[': parseArray(); break;
	case '{': parseObject(); break;
	default:
        THROW("Unexpected character at start of value: '"
              << (char)c << "' (0x" << std::hex << c << ")");
	}
  }

private:
  void parseArray() const {
    handler_.onArrayStart();
    while (source_.peek() != ']') value();
    expect(']');
    handler_.onArrayEnd();
  }

  void parseObject() const {
    handler_.onObjectStart();
    while (source_.peek() != '}') {
      value();
      value();
    }
    expect('}');
    handler_.onObjectEnd();
  }
};

template <typename Handler>
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
    int c = source_.next();
    handler_.onRecordStart(source_.pos()-1);
    switch (c) {
      case 'H':
        if (source_.next() == 'I') {
          int64_t version = readVarint();
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
          THROW("could be a parse error, or internal error: value handler didn't skip value!");
        break;
      }
      default:
        THROW("Unexpected character at start of record: '"
              << (char)c << "' (0x" << std::hex << c << ")");
    }
  }

  void term() const {
    expect('E'); expect('\n');
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

  template <typename H>
  void decode(H &handler) const {
    FileByteSource source(filename_);
    try {
      RecordParser(source, handler).parseStream();
      handler.onParseEnd();
    } catch (parse_error &e) {
      std::cout << e.what() << std::endl;
    }
  }
};
