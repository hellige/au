#pragma once

#include "au/AuByteSource.h"
#include "au/ParseError.h"

#include <cassert>

namespace au {

class BufferByteSource : public AuByteSource {
  const char *buf_; //< Underlying source buffer
  size_t bufLen_;   //< Underlying source buffer length
  size_t pos_ = 0;  //< Current position (this may be 1 past the end of buf_)

public:
  BufferByteSource(const char *buf, size_t len) : buf_(buf), bufLen_(len) {}

  BufferByteSource(std::string_view buf)
  : buf_(buf.data()), bufLen_(buf.length())
  {}

  std::string name() const override {
    return "<buffer>";
  }

  size_t pos() const override {
    assert(pos_ <= bufLen_);
    return pos_;
  }

  size_t endPos() const override { return bufLen_; }

  Byte peek() override {
    return pos_ < bufLen_ ? Byte(buf_[pos_]) : Byte::Eof();
  }

  Byte next() override {
    return pos_ < bufLen_ ? Byte(buf_[pos_++]) : Byte::Eof();
  }

  void readFunc(size_t len, Fn &&func) override {
    size_t sz = std::min(len, bufLen_ - pos_);
    func(std::string_view(buf_ + pos_, sz));
    pos_ += sz;
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
    pos_ = abspos;
  }

  void skip(size_t len) override { seek(pos_ + len); }

  bool scanTo(std::string_view needle) override {
    char *found = static_cast<char *>(memmem(buf_ + pos_, bufLen_ - pos_,
                        needle.data(), needle.length()));
    if (found) {
      assert(found >= buf_);
      pos_ = static_cast<size_t>(found - buf_);
      return true;
    }
    return false;
  }
};

}
