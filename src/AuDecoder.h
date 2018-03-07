#pragma once

#include "JsonHandler.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

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


namespace {

struct parse_error {
  std::string what;

  parse_error(const std::string &what)
  : what(what) {}
};


class FileByteSource {
  static const auto BUFFER_SIZE = 16*1024;

  int fd_;
  char *cur_;
  char *limit_;
  char buf_[BUFFER_SIZE];

public:
  FileByteSource(const std::string &fname) : cur_(nullptr), limit_(nullptr) {
    if (fname == "-") {
      fd_ = fileno(stdin);
    } else {
      fd_ = ::open(fname.c_str(), O_RDONLY);
    }
    if (fd_ == -1)
      throw std::runtime_error("open");
    ::posix_fadvise(fd_, 0, 0, 1);  // FDADVICE_SEQUENTIAL TODO report error?
    read();
  }

  ~FileByteSource() {
    close(fd_); // TODO report error?
  }

  int next() {
	while (cur_ == limit_) if (!read()) return EOF;
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
	  cur_ += first;
	  len -= first;
	}
  }

private:
  bool read() {
	cur_ = limit_ = buf_;
    size_t bytes_read = ::read(fd_, buf_, BUFFER_SIZE);
    if (bytes_read == (size_t)-1)
      throw std::runtime_error("read failed");
    if (!bytes_read) return false;
    limit_ = buf_ + bytes_read;
    return true;
//        for(char *p = buf; (p = (char*) memchr(p, '\n', (buf + bytes_read) - p)); ++p)
//            ++lines;
  }
};


template <typename Handler>
class Parser {
  static constexpr int AU_FORMAT_VERSION = 1;
  FileByteSource &source_;
  Handler &handler_;

public:
  Parser(FileByteSource &source, Handler &handler)
  : source_(source), handler_(handler) {}

  void parseStream() const {
    while (source_.peek() != EOF) record();
  }

private:
  void record() const {
    int c = source_.next();
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
      (void)backref; // TODO
      handler_.onDictAddStart();
      while (source_.peek() != 'E') {
    	  if (source_.next() != 'S')
    		  THROW("Expected a string"); // TODO
    	  parseString();
      }
      handler_.onDictAddEnd();
      term();
      break;
    }
    case 'V': {
      auto backref = readVarint();
      (void)backref; // TODO
      auto len = readVarint();
      (void)len; // TODO
      value();
      term();
      break;
    }
    default:
      THROW("Unexpected character at start of record: '" << (char)c
    		  << "' (0x" << std::hex << c << ")");
    }
    handler_.onRecordEnd();
  }

  void expect(char e) const {
    int c = source_.next();
    if (c == e) return;
    THROW("Unexpected character: '" << (char)c
  		  << "' (0x" << std::hex << c << ")");
  }

  void term() const {
    expect('E'); expect('\n');
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
      uint64_t i = source_.next();
      result |= (i & 0x7f) << shift;
      shift += 7;
      if (!(i & 0x80)) break;
    }
    return result;
  }

  void value() const {
	int c = source_.next();
	switch (c) {
	case 'T': handler_.onBool(true); break;
	case 'F': handler_.onBool(false); break;
	case 'N': handler_.onNull(); break;
	case 'I': handler_.onUint(readVarint()); break;
	case 'J': handler_.onInt(-readVarint()); break;
	case 'D': handler_.onDouble(readDouble()); break;
	case 'X': handler_.onDictRef(readVarint()); break;
	case 'S': parseString(); break;
	case '[': parseArray(); break;
	case '{': parseObject(); break;
	default:
      THROW("Unexpected character at start of value: '" << (char)c
        << "' (0x" << std::hex << c << ")");
	}
  }

  void parseString() const {
    auto len = readVarint();
    handler_.onStringStart(len);
    source_.read(len, [&](std::string_view fragment) {
      handler_.onStringFragment(fragment);
    });
    handler_.onStringEnd();
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
		value();
		value();
	}
	expect('}');
	handler_.onObjectEnd();
  }
};

}

struct NoopHandler {
  void onRecordEnd() {}
  void onObjectStart() {}
  void onObjectEnd() {}
  void onArrayStart() {}
  void onArrayEnd() {}
  void onNull() {}
  void onBool(bool) {}
  void onInt(int64_t) {}
  void onUint(uint64_t) {}
  void onDouble(double) {}
  void onDictRef(size_t) {}
  void onDictClear() {}
  void onDictAddStart() {}
  void onDictAddEnd() {}
  void onStringStart(size_t) {}
  void onStringEnd() {}
  void onStringFragment(std::string_view) {}
};

class AuDecoder {
  std::string filename_;

public:
  AuDecoder(const std::string &filename)
  : filename_(filename) {}

  void decode() const {
    FileByteSource source(filename_);
    JsonHandler handler;
    try {
      Parser(source, handler).parseStream();
    } catch (parse_error &e) {
      std::cout << e.what << std::endl;
    }
  }

  void decodeNoop() const {
    FileByteSource source(filename_);
    NoopHandler handler;
    try {
      Parser(source, handler).parseStream();
    } catch (parse_error &e) {
      std::cout << e.what << std::endl;
    }
  }

  template <typename H>
  void decode(H &handler) const {
    FileByteSource source(filename_);
    try {
      Parser(source, handler).parseStream();
    } catch (parse_error &e) {
      std::cout << e.what << std::endl;
    }
  }
};
