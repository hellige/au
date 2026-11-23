#pragma once

#include "au/AuByteSource.h"
#include "au/ParseError.h"

#include <cassert>

namespace au {

/// A byte source over an in-memory buffer. The whole buffer is the readable
/// window, so underflow() only ever happens at eof. (The buffer is never
/// written through, despite cur_ and limit_ being non-const in the base.)
class BufferByteSource : public AuByteSource {
  const char *buf_; //< Underlying source buffer
  size_t bufLen_;   //< Underlying source buffer length

  void setPos(size_t p) {
    cur_ = const_cast<char *>(buf_) + p;
    pos_ = p;
  }

protected:
  bool underflow() override { return false; }

public:
  BufferByteSource(const char *buf, size_t len) : buf_(buf), bufLen_(len) {
    setPos(0);
    limit_ = const_cast<char *>(buf_) + bufLen_;
  }

  BufferByteSource(std::string_view buf)
  : BufferByteSource(buf.data(), buf.length())
  {}

  std::string name() const override {
    return "<buffer>";
  }

  size_t endPos() const override { return bufLen_; }

  void readFunc(size_t len, Fn &&func) override {
    size_t sz = std::min(len, bufLen_ - pos_);
    func(std::string_view(buf_ + pos_, sz));
    setPos(pos_ + sz);
  }

  // ignored, the whole buffer is always available...
  void setPin(size_t abspos) override final {
    assert(abspos <= bufLen_);
    (void)abspos;
  }
  void clearPin() override final {}

  bool isSeekable() const override { return true; }

  void seek(size_t abspos) override {
    // i think we could allow a seek to eof (abspos == bufLen_), but in practice
    // nothing has ever tried to do that, and it isn't needed for now.
    if (abspos >= bufLen_) {
      THROW_RT("failed to seek to desired location: " << abspos);
    }
    setPos(abspos);
  }

  void skip(size_t len) override { seek(pos_ + len); }

  bool scanTo(std::string_view needle) override {
    auto found = static_cast<const char *>(memmem(buf_ + pos_, bufLen_ - pos_,
                        needle.data(), needle.length()));
    if (found) {
      assert(found >= buf_);
      setPos(static_cast<size_t>(found - buf_));
      return true;
    }
    return false;
  }
};

}
