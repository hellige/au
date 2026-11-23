#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace au {

/** A source of bytes for the decoder.
 *
 * next() and peek() are called for nearly every byte parsed, so they're
 * inline and non-virtual: they read directly from a window of buffered bytes
 * [cur_, limit_) that the subclass maintains, and only call the virtual
 * underflow() to refill the window when it's exhausted. (This is the same
 * approach as std::streambuf.) Everything else is virtual. */
class AuByteSource {
protected:
  char *cur_ = nullptr;   //< Next byte to be read
  char *limit_ = nullptr; //< End of the readable window
  size_t pos_ = 0;        //< Stream position of cur_

  /// Make at least one more byte available in [cur_, limit_), keeping
  /// pos_ consistent. Called only when cur_ == limit_. Returns false at eof.
  virtual bool underflow() = 0;

public:
  class Byte {
    int value_;
  public:
    explicit Byte(char c) : value_(static_cast<uint8_t >(c)) {}
  private:
    Byte() : value_(-1) {}
  public:
    bool isEof() const { return value_ == -1; }
    // these are called for nearly every byte parsed, so they must be inlined,
    // with the (never expected) throw kept out of line. otherwise gcc emits
    // them as real calls, which costs 10-25% of linear parse time.
    [[gnu::always_inline]] char charValue() const {
      if (isEof()) [[unlikely]] throwEof();
      return static_cast<char>(value_);
    }
    [[gnu::always_inline]] std::byte byteValue() const {
      if (isEof()) [[unlikely]] throwEof();
      return static_cast<std::byte>(value_);
    }
    [[gnu::always_inline]] uint8_t uint8Value() const {
      if (isEof()) [[unlikely]] throwEof();
      return static_cast<uint8_t>(value_);
    }
  private:
    [[noreturn, gnu::noinline, gnu::cold]] static void throwEof() {
      throw std::runtime_error("Tried to get value of eof");
    }
  public:
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

  virtual ~AuByteSource() = default;

  virtual std::string name() const = 0;

  /// The current position of the byte source. [0..sourceLen] (i.e. up to 1 past
  /// the end of the source - when at EOF).
  size_t pos() const { return pos_; }
  /// The length of the byte source (the position of the EOF)
  virtual size_t endPos() const = 0;
  /// The current byte
  [[gnu::always_inline]] Byte peek() {
    if (cur_ == limit_ && !underflow()) [[unlikely]] return Byte::Eof();
    return Byte(*cur_);
  }
  /// The next byte. Calling this on a newly created source should return the
  /// very first char in the source the 1st time it is called and then
  /// subsequent characters on each invocation.
  [[gnu::always_inline]] Byte next() {
    if (cur_ == limit_ && !underflow()) [[unlikely]] return Byte::Eof();
    pos_++;
    return Byte(*cur_++);
  }

  template<typename T>
  void read(T *t, size_t len) {
    char *buf = static_cast<char *>(static_cast<void *>(t));
    readFunc(len, [&](std::string_view fragment) {
      ::memcpy(buf, fragment.data(), fragment.size());
      buf += fragment.size();
    });
  }

  using Fn = std::function<void(std::string_view)>;
  /// Call func with the next len bytes from the underlying byte source.
  virtual void readFunc(size_t len, Fn &&func) = 0;

  virtual void setPin(size_t abspos) = 0;
  virtual void clearPin() = 0;
  virtual bool isSeekable() const = 0;
  virtual void seek(size_t abspos) = 0;

  virtual bool scanTo(std::string_view needle) = 0;

  virtual void skip(size_t len) = 0;

  /// For sources made of independently decodable chunks (e.g., zstd frames):
  /// returns a chunk start position strictly within (lo, hi), preferring one
  /// near the middle, or nullopt if there is none. Seeking to a chunk start
  /// is much cheaper than seeking to an arbitrary position.
  virtual std::optional<size_t> chunkBoundaryWithin(size_t, size_t) const {
    return std::nullopt;
  }

  /// The start of the chunk containing pos (0 for unchunked sources).
  virtual size_t chunkStartFor(size_t) const { return 0; }

  /// Seek to length bytes from the end of the stream
  void tail(size_t length) {
    auto end = endPos();
    if (end == 0) return;
    length = std::min(length, end);
    seek(end - length);
  }
};

}
