#pragma once

#include "au/AuCommon.h"
#include "au/AuByteSource.h"
#include "au/Handlers.h"
#include "au/ParseError.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <cstddef>
#include <chrono>
#include <variant>
#include <sys/stat.h>

// TODO add position/expectation info to all error messages

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

  void read(size_t len, Fn &&func) override {
    size_t sz = std::min(len, bufLen_ - pos_);
    func(std::string_view(buf_ + pos_, sz));
    pos_ += sz;
  }

  ssize_t doRead(char *buf, size_t len) override {
    if (pos_ < bufLen_) {
      size_t sz = std::min(bufLen_ - pos_, len);
      ::memcpy(buf, buf_ + pos_, sz);
      return static_cast<ssize_t>(sz);
    }
    return 0;
  }

  bool isSeekable() const override { return true; }

  void seek(size_t abspos) override {
    doSeek(abspos);
  }
  void doSeek(size_t abspos) override {
    if (abspos >= bufLen_) { // TODO: Should we allow seek to EOF?
      THROW_RT("failed to seek to desired location: " << abspos);
    }
    pos_ = abspos;
  }

  void skip(size_t len) override { seek(pos_ + len); }

  bool seekTo(std::string_view needle) override {
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

class FileByteSource : public AuByteSource { // TODO rename this and FileByteSourceImpl
protected:
  const size_t BUFFER_SIZE;

  std::string name_; // TODO use this in errors, etc...
  char *buf_;   //< Working buffer
  size_t pos_;  //< Current position in the underlying data stream
  char *cur_;   //< Current position in the working buffer
  char *limit_; //< End of the current working buffer

  bool waitForData_;

public:
  explicit FileByteSource(const std::string &fname, bool waitForData,
                          size_t bufferSizeInK = 256)
      : BUFFER_SIZE(bufferSizeInK * 1024),
        name_(fname == "-" ? "<stdin>" : fname),
        buf_(new char[BUFFER_SIZE]),
        pos_(0), cur_(buf_), limit_(buf_), waitForData_(waitForData) {}

  FileByteSource(const FileByteSource &) = delete;
  FileByteSource(FileByteSource &&) = delete;
  FileByteSource &operator=(const FileByteSource &) = delete;
  FileByteSource &operator=(FileByteSource &&) = delete;

  ~FileByteSource() override {
    delete[] buf_;
  }

  std::string name() const override {
    return name_;
  }

  /// Position in the underlying data stream
  size_t pos() const override { return pos_; }

  Byte next() override {
    while (cur_ == limit_) if (!read()) return Byte::Eof();
    pos_++;
    return Byte(*cur_++);
  }

  Byte peek() override {
    while (cur_ == limit_) if (!read()) return Byte::Eof();
    return Byte(*cur_);
  }

  // Add the base class's read() to the overload resolution list of this class.
  using AuByteSource::read;

  void read(size_t len, Fn &&func) override {
    while (len) {
      while (cur_ == limit_)
        if (!read())
          AU_THROW("reached eof while trying to read " << len << " bytes");
      // limit_ > cur_, so cast to size_t is fine...
      auto first = std::min(len, static_cast<size_t>(limit_ - cur_));
      func(std::string_view(cur_, first));
      pos_ += first;
      cur_ += first;
      len -= first;
    }
  }

  void skip(size_t len) override {
    seek(pos_ + len);
  }

  void seek(size_t abspos) override {
    if (abspos < pos_ && pos_ - abspos <= static_cast<size_t>(cur_ - buf_)) {
      auto relseek = pos_ - abspos;
      cur_ -= relseek;
      pos_ -= relseek;
    } else {
      doSeek(abspos);
      cur_ = limit_ = buf_;
      pos_ = abspos;
      if (!read())
        THROW_RT("failed to read from new location");
    }
  }

  bool seekTo(std::string_view needle) override {
    while (true) {
      while (buffAvail() < needle.length()) {
        // we might have just done a seek that left us with a very small
        // amount of buffered data. alternatively, we might have already
        // attempted to find 'needle' and failed. then we've done a seek to
        // very near the end of the buffer, leaving just len(needle)-1 bytes.
        // the seek automatically clears the buffer and re-reads, but the
        // UNDERLYING source might just return the same few bytes again, in
        // which case we'll fail on the next iteration. the contract is that
        // the underlying source can return any non-zero number of bytes on a
        // read(), but it won't return 0 unless it really actually has no more
        // bytes to give us. therefore...

        // we attempt to keep reading until we either have enough buffer
        // to scan, or until we really can't get anything more.
        // this is a crappy way of doing this, but given the
        // current design, it's the simplest solution. this whole thing should
        // be refactored...
        if (!read())
          return false;
      }
      auto found = static_cast<char *>(memmem(cur_, buffAvail(), needle.data(),
        needle.length()));
      if (found) {
        assert(found >= cur_);
        size_t offset = static_cast<size_t>(found - cur_);
        pos_ += offset;
        cur_ += offset;
        return true;
      } else {
        // TODO for zipped files where seeking may expensive, it'll be better
        // perhaps to copy the last few bytes to the start of buffer and just
        // read again, since they're contiguous reads and we're not really seeking.
        // but the zipped file source may hide that anyway using a context like
        // zindex does
        skip(buffAvail()-(needle.length()-1));
      }
    }
  }

private:
  /// Free space in the buffer
  size_t buffFree() const {
    return BUFFER_SIZE - static_cast<size_t>(limit_ - buf_);
  }

  /// Available to be consumed
  size_t buffAvail() const {
    return static_cast<size_t>(limit_ - cur_);
  }

protected:
  /// @return true if some data was read, false of 0 bytes were read.
  bool read() {
    return read(BUFFER_SIZE / 16);
  }

private:
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
      bytesRead = doRead(limit_, buffFree());
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

class FileByteSourceImpl : public FileByteSource {
  int fd_;

public:
  explicit FileByteSourceImpl(const std::string &fname, bool waitForData,
                              size_t bufferSizeInK = 256)
      : FileByteSource(fname, waitForData, bufferSizeInK) {
    if (fname == "-") {
      fd_ = fileno(stdin);
    } else {
      fd_ = ::open(fname.c_str(), O_RDONLY);
    }
    if (fd_ == -1)
      THROW_RT("open: " << strerror(errno) << " (" << fname << ")");
#ifndef __APPLE__
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);  // TODO report error?
#endif
  }

  ~FileByteSourceImpl() override {
    close(fd_); // TODO report error?
  }

  ssize_t doRead(char *buf, size_t len) override {
    return ::read(fd_, buf, len);
  }

  size_t endPos() const override {
    struct stat stat;
    if (auto res = fstat(fd_, &stat); res < 0)
      THROW_RT("failed to stat file: " << strerror(errno));
    if (stat.st_size < 0)
      THROW_RT("file size was negative!");
    return static_cast<size_t>(stat.st_size);
  }

  bool isSeekable() const override {
    return lseek(fd_, 0, SEEK_CUR) == 0;
  }

  void doSeek(size_t abspos) override {
    auto pos = lseek(fd_, static_cast<off_t>(abspos), SEEK_SET);
    if (pos < 0) {
      THROW_RT("failed to seek to desired location: " << strerror(errno));
    }
  }
};

class StringBuilder {
  std::string str_;
  size_t maxLen_;

public:
  StringBuilder(size_t maxLen) : maxLen_(maxLen) {}

  void onStringStart(size_t, size_t len) {
    if (len > maxLen_)
      throw std::length_error("String too long");
    str_.reserve(len);
  }
  void onStringFragment(std::string_view frag) {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
  void onStringEnd() {}

  const std::string &str() const { return str_; }
};

class BaseParser {
protected:
  static constexpr int AU_FORMAT_VERSION = FormatVersion1::AU_FORMAT_VERSION;

  AuByteSource &source_;

  explicit BaseParser(AuByteSource &source)
      : source_(source) {}

  void expect(char e) const {
    auto c = source_.next();
    if (c == e) return;
    AU_THROW("Unexpected character: " << c);
  }

  uint32_t readBackref() const {
    uint32_t val;
    source_.read<uint32_t>(&val, sizeof(val));
    return val;
  }

  double readDouble() const {
    double val;
    static_assert(sizeof(val) == 8, "sizeof(double) must be 8");
    source_.read(&val, sizeof(val));
    return val;
  }

  time_point readTime() const {
    uint64_t nanos;
    source_.read(&nanos, sizeof(nanos));
    std::chrono::nanoseconds n(nanos);
    return time_point() + n;
  }

  uint64_t readVarint() const {
    auto shift = 0u;
    uint64_t result = 0;
    while (true) {
      if (shift >= 64u)
        AU_THROW("Bad varint encoding");
      auto next = source_.next();
      if (next.isEof())
        AU_THROW("Unexpected end of file");
      auto i = next.byteValue();
      const auto valueMask = std::byte(0x7f);
      const auto moreMask = std::byte(0x80);
      result |= static_cast<uint64_t>(i & valueMask) << shift;
      shift += 7;
      if ((i & moreMask) != moreMask) break;
    }
    return result;
  }

  uint64_t parseFormatVersion() const {
    uint64_t version;
    auto c = source_.next();
    if ((c.charValue() & ~0x1f) == marker::SmallInt::Positive) {
      version = c.charValue() & 0x1f;
    } else if (c == marker::Varint) {
      version = readVarint();
    } else {
      AU_THROW("Expected version number");
    }

    // note: this would be one possible place to check that the format is one
    // of multiple supported versions, return the version number, and then
    // dispatch to one of several value parsers. i think that would currently
    // do the right thing for tail as well as for other use sites.

    if (version != AU_FORMAT_VERSION) {
      AU_THROW("Bad format version: expected " << AU_FORMAT_VERSION
                                            << ", got " << version);
    }
    return version;
  }

  template <typename Handler>
  void parseFullString(Handler &handler) const {
    size_t sov = source_.pos();
    auto c = source_.next();
    if ((c.uint8Value() & ~0x1fu) == 0x20) {
      parseString(sov, c.uint8Value() & 0x1fu, handler);
    } else if (c == marker::String) {
      parseString(sov, handler);
    } else {
      AU_THROW("Expected a string");
    }
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

struct TooDeeplyNested : std::runtime_error {
  TooDeeplyNested() : runtime_error("File too deeply nested") {}
};

template<typename Handler>
class ValueParser : BaseParser {
  Handler &handler_;
  /** A positive value that when multiplied by -1 represents the most negative
  number we support (std::numeric_limits<int64_t>::min() * -1). */
  static constexpr uint64_t NEG_INT_LIMIT =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;

  // TODO was 8192, reduced to avoid stack overflow with clang asan. it's known
  // that clang asan has ~3x stack overhead, so if this is ok, probably best
  // to put it back to 8192 by default and reduce it only for asan builds...
  static inline constexpr size_t MaxDepth = 2048;
  mutable size_t depth{0};
  struct DepthRaii {
    const ValueParser &parent;
    explicit DepthRaii(const ValueParser &vp) : parent(vp) {
      parent.depth++;
      if (parent.depth > MaxDepth)
        throw TooDeeplyNested();
    }
    ~DepthRaii() { parent.depth--; }
  };

public:
  ValueParser(AuByteSource &source, Handler &handler)
      : BaseParser(source), handler_(handler) {}

  void value() const {
    size_t sov = source_.pos();
    auto c = source_.next();
    if (c.isEof())
      AU_THROW("Unexpected EOF at start of value");
    if (c.charValue() & 0x80) {
      handler_.onDictRef(sov, c.uint8Value() & ~0x80u);
      return;
    }
    {
      auto val = c.uint8Value() & ~0xe0u;
      if (c.charValue() & marker::SmallInt::Negative) {
        if (c.charValue() & 0x20)
          handler_.onUint(sov, val);
        else
          handler_.onInt(sov, -static_cast<int>(val));
        return;
      }
      if (c.uint8Value() & 0x20) {
        parseString(sov, val, handler_);
        return;
      }
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
          AU_THROW("Signed int overflows int64_t: (-)" << i << " 0x"
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
          AU_THROW("Signed int overflows int64_t: (-)" << val << " 0x"
                << std::setfill('0') << std::setw(16) << std::hex << val);
        }
        handler_.onInt(sov, -static_cast<int64_t>(val));
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
        AU_THROW("Unexpected character at start of value: " << c);
    }
  }

private:
  void key() const {
    size_t sov = source_.pos();
    auto c = source_.next();
    if (c.isEof())
      AU_THROW("Unexpected EOF at start of key");
    if (c.charValue() & 0x80) {
      handler_.onDictRef(sov, c.uint8Value() & ~0x80u);
      return;
    }
    auto val = c.uint8Value() & ~0xe0u;
    if ((c.uint8Value() & ~0x1f) == 0x20) {
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
        AU_THROW("Unexpected character at start of key: " << c);
    }
  }

  void parseArray() const {
    DepthRaii raii(*this);
    handler_.onArrayStart();
    while (source_.peek() != marker::ArrayEnd) value();
    expect(marker::ArrayEnd);
    handler_.onArrayEnd();
  }

  void parseObject() const {
    DepthRaii raii(*this);
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
  Handler &handler_;

public:
  RecordParser(AuByteSource &source, Handler &handler)
      : BaseParser(source), handler_(handler) {}

  void parseStream(bool expectHeader = true) const {
    if (expectHeader) checkHeader();
    while (source_.peek() != EOF) record();
  }

  bool parseUntilValue() {
    while (source_.peek() != EOF)
      if (record()) return true;
    return false;
  }

  bool record() const {
    auto c = source_.next();
    if (c.isEof()) AU_THROW("Unexpected EOF at start of record");
    handler_.onRecordStart(source_.pos() - 1);
    switch (c.charValue()) {
      case 'H': {   // Header / metadata
        expect('A');
        expect('U');
        auto version = parseFormatVersion();
        StringBuilder sb(FormatVersion1::MAX_METADATA_SIZE);
        parseFullString(sb);
        handler_.onHeader(version, sb.str());
        term();
        break;
      }
      case 'C':     // Clear dictionary
        parseFormatVersion();
        term();
        handler_.onDictClear();
        break;
      case 'A': {   // Add dictionary entry
        auto backref = readBackref();
        handler_.onDictAddStart(backref);
        while (source_.peek() != marker::RecordEnd)
          parseFullString(handler_);
        term();
        break;
      }
      case 'V': {   // Add value
        auto backref = readBackref();
        auto len = readVarint();
        auto startOfValue = source_.pos();
        handler_.onValue(backref, len - 2, source_);
        term();
        if (source_.pos() - startOfValue != len)
          AU_THROW("could be a parse error, or internal error: value handler "
                "didn't skip value!");
        return true;
      }
      default:
        AU_THROW("Unexpected character at start of record: " << c);
    }
    return false;
  }

private:
  struct HeaderHandler : NoopRecordHandler {
    bool headerSeen = false;
    void onHeader(uint64_t, const std::string &) override {
      headerSeen = true;
    }
  };

  void checkHeader() const {
    // this is a special case. empty files should be considered ok, even
    // though they don't have a header/magic bytes, etc...
    if (source_.peek() == EOF) return;
    HeaderHandler hh;
    try {
      RecordParser<HeaderHandler>(source_, hh).record();
    } catch (const au::parse_error &) {
      // don't care what it was...
    }
    if (!hh.headerSeen)
      AU_THROW("This file doesn't appear to start with an au header record");
  }
};

class AuDecoder {
  std::string filename_;

public:
  AuDecoder(const std::string &filename)
      : filename_(filename) {}

  template<typename H>
  void decode(H &handler, bool waitForData) const {
    FileByteSourceImpl source(filename_, waitForData);
    try {
      RecordParser<H>(source, handler).parseStream();
    } catch (parse_error &e) {
      std::cerr << e.what() << std::endl;
    }
  }
};

template<typename Handler>
ValueParser(AuByteSource &source, Handler &handler) -> ValueParser<Handler>;
template<typename Handler>
RecordParser(AuByteSource &source, Handler &handler) -> RecordParser<Handler>;

}
