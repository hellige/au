#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace au {

class AuByteSource {
public:
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

  virtual ~AuByteSource() {}

  virtual std::string name() const = 0;

  /// The current position of the byte source. [0..sourceLen] (i.e. up to 1 past
  /// the end of the source - when at EOF).
  virtual size_t pos() const = 0;
  /// The length of the byte source (the position of the EOF)
  virtual size_t endPos() const = 0;
  /// The current byte
  virtual Byte peek() = 0;
  /// The next byte. Calling this on a newly created source should return the
  /// very first char in the source the 1st time it is called and then
  /// subsequent characters on each invocation.
  virtual Byte next() = 0;

  template<typename T>
  void read(T *t, size_t len) {
    char *buf = static_cast<char *>(static_cast<void *>(t));
    read(len, [&](std::string_view fragment) {
      ::memcpy(buf, fragment.data(), fragment.size());
      buf += fragment.size();
    });
  }

  using Fn = std::function<void(std::string_view)>;
  /// Call func with the next len bytes from the underlying byte source.
  virtual void read(size_t len, Fn &&func) = 0;
  virtual size_t doRead(char *buf, size_t len) = 0;

  virtual bool isSeekable() const = 0;
  virtual void seek(size_t abspos) = 0;
  virtual void doSeek(size_t abspos) = 0;

  virtual bool seekTo(std::string_view needle) = 0;

  virtual void skip(size_t len) = 0;

  /// Seek to length bytes from the end of the stream
  void tail(size_t length) {
    auto end = endPos();
    length = std::min(length, end);
    seek(end - length);
  }
};

}