#include "Zstd.h"

#include "au/AuCommon.h"
#include "au/ParseError.h"

#define ZSTD_STATIC_LINKING_ONLY // for ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX
#include <zstd.h>
#include <zstd_errors.h>
#include <zstd_seekable.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <vector>

namespace au {

namespace {

size_t check(size_t ret, const char *what) {
  if (ZSTD_isError(ret))
    THROW_RT(what << ": " << ZSTD_getErrorName(ret));
  return ret;
}

struct DCtxDeleter {
  void operator()(ZSTD_DCtx *d) { ZSTD_freeDCtx(d); }
};

struct CCtxDeleter {
  void operator()(ZSTD_CCtx *c) { ZSTD_freeCCtx(c); }
};

struct SeekableDeleter {
  void operator()(ZSTD_seekable *s) { ZSTD_seekable_free(s); }
};

struct FrameLogDeleter {
  void operator()(ZSTD_frameLog *f) { ZSTD_seekable_freeFrameLog(f); }
};

}

struct ZstdByteSource::Impl {
  File file_;
  std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx_;
  size_t pos_ = 0; // current decompressed position

  // Streaming mode (no seek table): input buffering state.
  std::vector<char> inBuf_;
  ZSTD_inBuffer in_{nullptr, 0, 0};
  bool inputEof_ = false;
  // last return value of ZSTD_decompressStream: 0 iff we're at a frame
  // boundary, which is the only valid place for the input to end.
  size_t lastRet_ = 0;

  // Seekable mode: the seek table, plus a small cache of recently used
  // frames. Entry i is the start of frame i; the final entry is a sentinel for
  // the end of the last frame (so size() == numFrames + 1).
  std::vector<uint64_t> cOffsets_;
  std::vector<uint64_t> dOffsets_;

  /// A byte buffer that grows without initializing its contents.
  class Buffer {
    std::unique_ptr<char[]> buf_;
    size_t size_ = 0, capacity_ = 0;
  public:
    void resize(size_t n) {
      if (n > capacity_) {
        buf_.reset(new char[n]); // default-init: not zeroed
        capacity_ = n;
      }
      size_ = n;
    }
    char *data() { return buf_.get(); }
    const char *data() const { return buf_.get(); }
    size_t size() const { return size_; }
  };

  // A frame being (lazily) decompressed. We only decompress as far into the
  // frame as we've been asked to read, so a seek costs on average half a
  // frame of decompression rather than a whole one.
  struct Frame {
    std::optional<size_t> index;
    std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx{ZSTD_createDCtx()};
    // buffers are reused across frames and never zero-filled: a probe
    // touches only a small prefix of a frame, so zeroing (or even faulting
    // in) the whole thing would dominate the cost.
    Buffer compressed;            // sized to the full compressed frame
    size_t compressedRead = 0;    // bytes of compressed that are valid
    Buffer data;                  // sized to the full decompressed frame
    ZSTD_inBuffer in{nullptr, 0, 0};
    size_t decoded = 0;           // bytes of data that are valid
    uint64_t lastUse = 0;
  };
  // compressed data is read lazily too, growing geometrically from this size.
  // a probe at a frame start (to inspect the first record or two) then only
  // costs a small read, which matters a lot on high-latency filesystems.
  static constexpr size_t InitialCompressedRead = 64 * 1024;
  // binary search over an au file bounces between a probe point and the
  // dictionary preceding it, which is often in an earlier frame, so we keep a
  // few frames around.
  static constexpr size_t FrameCacheSize = 4;
  // when we need more of a frame, decompress at least this much more, to
  // amortize per-call overhead
  static constexpr size_t MinDecodeChunk = 64 * 1024;
  std::vector<Frame> frames_;
  uint64_t useCounter_ = 0;

  Impl(File file, std::string_view buffered, const std::string &name)
      : file_(std::move(file)),
        dctx_(ZSTD_createDCtx()) {
    if (!dctx_) THROW_RT("Unable to create zstd decompression context");
    // Be permissive: accept frames written with any window size (e.g., with
    // --long). The window can never exceed the frame size anyway.
    check(ZSTD_DCtx_setParameter(dctx_.get(), ZSTD_d_windowLogMax,
                                 ZSTD_WINDOWLOG_MAX),
          "Setting zstd windowLogMax");

    loadSeekTable(name);

    if (!isSeekable()) {
      // start with whatever the original source had already buffered
      inBuf_.resize(std::max(ZSTD_DStreamInSize(), buffered.size()));
      ::memcpy(inBuf_.data(), buffered.data(), buffered.size());
      in_ = ZSTD_inBuffer{inBuf_.data(), buffered.size(), 0};
    }
  }

  void loadSeekTable(const std::string &name) {
    auto *f = file_.get();
    auto here = ::ftello(f);
    if (here < 0) return; // not seekable (e.g., stdin)

    std::unique_ptr<ZSTD_seekable, SeekableDeleter> zs(ZSTD_seekable_create());
    if (!zs) THROW_RT("Unable to create zstd seekable context");
    auto ret = ZSTD_seekable_initFile(zs.get(), f);
    if (ZSTD_isError(ret)) {
      // prefix_unknown: no seek table footer. seekableIO: file too short.
      auto code = ZSTD_getErrorCode(ret);
      if (code != ZSTD_error_prefix_unknown && code != ZSTD_error_seekableIO)
        std::cerr << "Warning: ignoring invalid zstd seek table in " << name
                  << " (" << ZSTD_getErrorName(ret) << ")\n";
    } else {
      auto n = ZSTD_seekable_getNumFrames(zs.get());
      cOffsets_.reserve(n + 1);
      dOffsets_.reserve(n + 1);
      uint64_t cEnd = 0;
      uint64_t dEnd = 0;
      for (unsigned i = 0; i < n; i++) {
        cOffsets_.push_back(ZSTD_seekable_getFrameCompressedOffset(zs.get(), i));
        dOffsets_.push_back(ZSTD_seekable_getFrameDecompressedOffset(zs.get(), i));
        cEnd = cOffsets_.back() + ZSTD_seekable_getFrameCompressedSize(zs.get(), i);
        dEnd = dOffsets_.back() + ZSTD_seekable_getFrameDecompressedSize(zs.get(), i);
      }
      cOffsets_.push_back(cEnd);
      dOffsets_.push_back(dEnd);
    }

    if (::fseeko(f, here, SEEK_SET) != 0)
      THROW_RT("Unable to restore file position: " << strerror(errno));
  }

  bool isSeekable() const { return !dOffsets_.empty(); }

  size_t chunkStartFor(size_t pos) const {
    if (!isSeekable() || pos >= dOffsets_.back()) return 0;
    return static_cast<size_t>(dOffsets_[frameFor(pos)]);
  }

  std::optional<size_t> chunkBoundaryWithin(size_t lo, size_t hi) const {
    if (dOffsets_.size() < 3 || hi <= lo + 1) return std::nullopt;
    // frame starts strictly within (lo, hi). the sentinel (end of file) is
    // excluded, as is frame 0 (position 0 is never > lo).
    auto first = std::upper_bound(dOffsets_.begin(), dOffsets_.end() - 1, lo);
    auto last = std::lower_bound(first, dOffsets_.end() - 1, hi);
    if (first == last) return std::nullopt;
    auto mid = lo + (hi - lo) / 2;
    // the boundary closest to the middle
    auto it = std::lower_bound(first, last, mid);
    if (it == last) --it;
    else if (it != first && mid - *(it - 1) < *it - mid) --it;
    return static_cast<size_t>(*it);
  }

  size_t endPos() const {
    if (!isSeekable())
      THROW_RT("Can't determine the uncompressed size of a zstd file without"
               " a seek table. Use 'au zstd' to create a seekable file.");
    return dOffsets_.back();
  }

  size_t doRead(char *buf, size_t len) {
    return isSeekable() ? readFrames(buf, len) : readStreaming(buf, len);
  }

  size_t readStreaming(char *buf, size_t len) {
    ZSTD_outBuffer out{buf, len, 0};
    while (true) {
      if (in_.pos == in_.size && !inputEof_) {
        auto n = ::fread(inBuf_.data(), 1, inBuf_.size(), file_.get());
        if (::ferror(file_.get()))
          THROW_RT("Error reading zstd input: " << strerror(errno));
        if (n == 0) inputEof_ = true;
        in_ = ZSTD_inBuffer{inBuf_.data(), n, 0};
      }
      bool noInput = inputEof_ && in_.pos == in_.size;
      // at a frame boundary with no more input: clean eof
      if (noInput && lastRet_ == 0) break;
      lastRet_ = check(ZSTD_decompressStream(dctx_.get(), &out, &in_),
                       "zstd decompression failed");
      if (out.pos) break;
      // no input and no output, but the decoder isn't at a frame boundary
      if (noInput) THROW_RT("Truncated zstd input");
    }
    pos_ += out.pos;
    return out.pos;
  }

  /// Index of the frame containing abspos, or the number of frames if abspos
  /// is at (or past) the end.
  size_t frameFor(size_t abspos) const {
    return static_cast<size_t>(
        std::upper_bound(dOffsets_.begin(), dOffsets_.end(), abspos)
        - dOffsets_.begin() - 1);
  }

  Frame &getFrame(size_t index) {
    if (frames_.empty()) frames_.resize(FrameCacheSize);
    Frame *victim = &frames_[0];
    for (auto &f : frames_) {
      if (f.index == index) {
        f.lastUse = ++useCounter_;
        return f;
      }
      if (f.lastUse < victim->lastUse) victim = &f;
    }

    auto &f = *victim;
    f.index.reset();
    if (!f.dctx) THROW_RT("Unable to create zstd decompression context");
    check(ZSTD_DCtx_reset(f.dctx.get(), ZSTD_reset_session_only),
          "Resetting zstd context");
    check(ZSTD_DCtx_setParameter(f.dctx.get(), ZSTD_d_windowLogMax,
                                 ZSTD_WINDOWLOG_MAX),
          "Setting zstd windowLogMax");
    auto cSize = static_cast<size_t>(cOffsets_[index + 1] - cOffsets_[index]);
    auto dSize = static_cast<size_t>(dOffsets_[index + 1] - dOffsets_[index]);
    f.compressed.resize(cSize);
    f.compressedRead = 0;
    f.data.resize(dSize);
    f.in = ZSTD_inBuffer{f.compressed.data(), 0, 0};
    f.decoded = 0;
    f.index = index;
    f.lastUse = ++useCounter_;
    return f;
  }

  /// Read more of the frame's compressed data. Returns false if it's all
  /// been read already.
  bool readMoreCompressed(Frame &f) {
    auto total = f.compressed.size();
    if (f.compressedRead == total) return false;
    auto want = std::max(InitialCompressedRead, f.compressedRead);
    want = std::min(want, total - f.compressedRead);
    auto off = cOffsets_[*f.index] + f.compressedRead;
    if (::fseeko(file_.get(), static_cast<off_t>(off), SEEK_SET) != 0)
      THROW_RT("Error seeking in zstd file: " << strerror(errno));
    if (::fread(f.compressed.data() + f.compressedRead, 1, want, file_.get())
        != want)
      THROW_RT("Error reading zstd frame " << *f.index << ": "
               << (::ferror(file_.get()) ? strerror(errno) : "truncated file"));
    f.compressedRead += want;
    f.in.size = f.compressedRead;
    return true;
  }

  /// Ensure at least `upTo` bytes of the frame are decompressed.
  void decodeTo(Frame &f, size_t upTo) {
    if (f.decoded >= upTo) return;
    auto target = std::min(f.data.size(), std::max(upTo, f.decoded + MinDecodeChunk));
    ZSTD_outBuffer out{f.data.data(), target, f.decoded};
    while (out.pos < target) {
      if (f.in.pos == f.in.size && !readMoreCompressed(f)) break;
      auto ret = check(ZSTD_decompressStream(f.dctx.get(), &out, &f.in),
                       "zstd decompression failed");
      if (ret == 0) break;
    }
    f.decoded = out.pos;
    if (f.decoded < upTo)
      THROW_RT("zstd frame " << *f.index << " decompressed to fewer bytes ("
               << f.decoded << ") than the seek table says ("
               << f.data.size() << ")");
  }

  size_t readFrames(char *buf, size_t len) {
    if (pos_ >= dOffsets_.back()) return 0;
    auto index = frameFor(pos_);
    auto &f = getFrame(index);
    auto offset = static_cast<size_t>(pos_ - dOffsets_[index]);
    auto n = std::min(len, f.data.size() - offset);
    // only decode as much as needed to return *something*: the caller asks
    // for its whole free buffer, but a probe might only look at one record.
    // anything already decoded is returned too.
    decodeTo(f, offset + std::min(n, MinDecodeChunk));
    n = std::min(n, f.decoded - offset);
    ::memcpy(buf, f.data.data() + offset, n);
    pos_ += n;
    return n;
  }

  void doSeek(size_t abspos) {
    if (!isSeekable())
      THROW_RT("Can't seek in a zstd file without a seek table");
    if (abspos > dOffsets_.back())
      THROW_RT("Seek to " << abspos << " is past end of file ("
               << dOffsets_.back() << ")");
    // frames are loaded lazily on the next read
    pos_ = abspos;
  }
};

ZstdByteSource::ZstdByteSource(FileByteSourceImpl &source)
    : FileByteSource(source.name()) {
  // we can only take over the buffered data if nothing has been consumed yet
  // (detection peeks and then seeks back to the start).
  if (source.pos() != source.bufferOffset())
    THROW_RT("ZstdByteSource must be created from a source at position 0");
  auto buffered = source.bufferedFromStart();
  impl_ = std::make_unique<Impl>(std::move(source.file_), buffered,
                                 source.name());
}

ZstdByteSource::~ZstdByteSource() = default;

bool ZstdByteSource::isSeekable() const { return impl_->isSeekable(); }
size_t ZstdByteSource::endPos() const { return impl_->endPos(); }
size_t ZstdByteSource::doRead(char *buf, size_t len) {
  return impl_->doRead(buf, len);
}
void ZstdByteSource::doSeek(size_t abspos) { impl_->doSeek(abspos); }
size_t ZstdByteSource::chunkStartFor(size_t pos) const {
  return impl_->chunkStartFor(pos);
}
std::optional<size_t> ZstdByteSource::chunkBoundaryWithin(size_t lo,
                                                          size_t hi) const {
  return impl_->chunkBoundaryWithin(lo, hi);
}

////////////////////////////////////////////////////////////////////////////////
// Compression

namespace {

struct CompressedFrame {
  std::vector<char> data;
  size_t srcSize;
};

CompressedFrame compressFrame(std::vector<char> src,
                              const ZstdCompressOptions &opts) {
  std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
  if (!cctx) THROW_RT("Unable to create zstd compression context");
  check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, opts.level),
        "Setting compression level");
  check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_checksumFlag,
                               opts.checksum ? 1 : 0),
        "Setting checksum flag");
  if (opts.longWindowLog) {
    check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_enableLongDistanceMatching,
                                 ZSTD_ps_enable),
          "Enabling long distance matching");
    check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_windowLog,
                                 opts.longWindowLog),
          "Setting window log");
  }
  CompressedFrame result;
  result.srcSize = src.size();
  result.data.resize(ZSTD_compressBound(src.size()));
  auto n = check(ZSTD_compress2(cctx.get(), result.data.data(),
                                result.data.size(), src.data(), src.size()),
                 "zstd compression failed");
  result.data.resize(n);
  return result;
}

void writeAll(FILE *f, const void *data, size_t len) {
  if (len && ::fwrite(data, 1, len, f) != len)
    THROW_RT("Error writing output: " << strerror(errno));
}

size_t readFully(FILE *f, char *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    auto n = ::fread(buf + total, 1, len - total, f);
    if (n == 0) {
      if (::ferror(f)) THROW_RT("Error reading input: " << strerror(errno));
      break;
    }
    total += n;
  }
  return total;
}

/// Compresses frames in parallel (up to opts.threads in flight) and writes
/// them in order, followed by a seek table on finish().
class FrameSink {
  FILE *out_;
  const ZstdCompressOptions &opts_;
  std::unique_ptr<ZSTD_frameLog, FrameLogDeleter> frameLog_;
  std::deque<std::future<CompressedFrame>> inFlight_;

public:
  size_t totalIn = 0;
  size_t totalOut = 0;
  size_t numFrames = 0;

  FrameSink(FILE *out, const ZstdCompressOptions &opts)
      : out_(out), opts_(opts),
        frameLog_(ZSTD_seekable_createFrameLog(0)) {
    if (!frameLog_) THROW_RT("Unable to create zstd seekable frame log");
  }

  void add(std::vector<char> frame) {
    if (frame.empty()) return;
    if (frame.size() > ZSTD_SEEKABLE_MAX_FRAME_DECOMPRESSED_SIZE)
      THROW_RT("Frame of " << frame.size() << " bytes exceeds the seekable"
               " format's maximum of "
               << ZSTD_SEEKABLE_MAX_FRAME_DECOMPRESSED_SIZE);
    if (inFlight_.size() >= std::max(1u, opts_.threads)) writeOldest();
    inFlight_.push_back(std::async(std::launch::async, compressFrame,
                                   std::move(frame), std::cref(opts_)));
  }

  void finish() {
    while (!inFlight_.empty()) writeOldest();
    std::vector<char> tableBuf(64 * 1024);
    while (true) {
      ZSTD_outBuffer tableOut{tableBuf.data(), tableBuf.size(), 0};
      auto remaining = check(
          ZSTD_seekable_writeSeekTable(frameLog_.get(), &tableOut),
          "Writing seek table");
      writeAll(out_, tableBuf.data(), tableOut.pos);
      totalOut += tableOut.pos;
      if (remaining == 0) break;
    }
    if (::fflush(out_) != 0)
      THROW_RT("Error writing output: " << strerror(errno));
  }

private:
  void writeOldest() {
    auto frame = inFlight_.front().get();
    inFlight_.pop_front();
    writeAll(out_, frame.data.data(), frame.data.size());
    check(ZSTD_seekable_logFrame(frameLog_.get(),
                                 static_cast<unsigned>(frame.data.size()),
                                 static_cast<unsigned>(frame.srcSize), 0),
          "Logging seekable frame");
    totalIn += frame.srcSize;
    totalOut += frame.data.size();
    numFrames++;
    if (opts_.verbose)
      std::cerr << "\rframes: " << numFrames << ", in: " << totalIn
                << ", out: " << totalOut << std::flush;
  }
};

/// Buffered reader over the raw input, used to split an au stream into
/// records without decoding values.
class RecordReader {
  FILE *in_;
  std::vector<char> buf_;
  size_t start_ = 0; // start of unconsumed data in buf_
  size_t end_ = 0;   // end of valid data in buf_
  bool eof_ = false;

public:
  explicit RecordReader(FILE *in) : in_(in), buf_(1 << 20) {}

  /// Make at least n bytes available past the start. False if we hit eof.
  bool ensure(size_t n) {
    while (end_ - start_ < n) {
      if (eof_) return false;
      if (start_ > 0) {
        ::memmove(buf_.data(), buf_.data() + start_, end_ - start_);
        end_ -= start_;
        start_ = 0;
      }
      if (buf_.size() < n) buf_.resize(std::max(n, buf_.size() * 2));
      auto got = ::fread(buf_.data() + end_, 1, buf_.size() - end_, in_);
      if (got == 0) {
        if (::ferror(in_)) THROW_RT("Error reading input: " << strerror(errno));
        eof_ = true;
      }
      end_ += got;
    }
    return true;
  }

  size_t available() const { return end_ - start_; }
  const char *data() const { return buf_.data() + start_; }
  uint8_t at(size_t i) const { return static_cast<uint8_t>(buf_[start_ + i]); }
  void consume(size_t n) { start_ += n; }
};

/// Thrown when the input ends partway through a record.
struct TruncatedRecord {};

/// Helpers to measure the encoded length of au fields at an offset into the
/// reader's buffer (relative to its current start).
struct AuScanner {
  RecordReader &r;

  uint8_t byte(size_t off) {
    if (!r.ensure(off + 1)) throw TruncatedRecord{};
    return r.at(off);
  }

  /// Returns the offset just past a varint starting at off.
  size_t varint(size_t off, uint64_t *value = nullptr) {
    uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
      auto b = byte(off++);
      if (shift >= 64) THROW_RT("Bad varint in au input");
      result |= static_cast<uint64_t>(b & 0x7f) << shift;
      shift += 7;
      if (!(b & 0x80)) break;
    }
    if (value) *value = result;
    return off;
  }

  size_t formatVersion(size_t off) {
    auto c = byte(off);
    if ((c & ~0x1fu) == marker::SmallInt::Positive) return off + 1;
    if (c == marker::Varint) return varint(off + 1);
    THROW_RT("Expected au format version");
  }

  size_t string(size_t off) {
    auto c = byte(off);
    uint64_t len;
    if ((c & ~0x1fu) == 0x20) {
      len = c & 0x1fu;
      off += 1;
    } else if (c == marker::String) {
      off = varint(off + 1, &len);
    } else {
      THROW_RT("Expected a string in au dictionary record");
    }
    if (!r.ensure(off + len)) throw TruncatedRecord{};
    return off + len;
  }

  size_t term(size_t off) {
    if (byte(off) != marker::RecordEnd || byte(off + 1) != '\n')
      THROW_RT("Expected end of au record");
    return off + 2;
  }

  uint32_t backref(size_t off) {
    if (!r.ensure(off + 4)) throw TruncatedRecord{};
    uint32_t v;
    ::memcpy(&v, r.data() + off, 4);
    return v;
  }
};

template <typename Buf>
void appendBackref(Buf &out, size_t backref) {
  if (backref > std::numeric_limits<uint32_t>::max())
    THROW_RT("au backref " << backref << " doesn't fit in 32 bits");
  auto v = static_cast<uint32_t>(backref);
  const char *p = reinterpret_cast<const char *>(&v);
  out.insert(out.end(), p, p + 4);
}

/// Splits an au stream into frames such that every frame can be decoded
/// independently: each frame after the first begins with a dictionary clear
/// followed by the complete current dictionary (preserving entry order, hence
/// indices). Backrefs are rewritten to match. The decoded records are
/// unchanged, but the bytes are not identical to the input.
void auFrames(FILE *in, FrameSink &sink, const ZstdCompressOptions &opts) {
  RecordReader r(in);
  AuScanner sc{r};

  if (!r.ensure(1)) return; // empty input: empty output
  if (!r.ensure(3) || ::memcmp(r.data(), "HAU", 3) != 0)
    THROW_RT("Input doesn't look like an au file (no au header). Use au zstd"
             " without --au to compress other files.");

  std::vector<char> frame;
  frame.reserve(opts.frameSize + opts.frameSize / 8);
  size_t frameStartOut = 0; // output position of the start of `frame`
  auto outPos = [&] { return frameStartOut + frame.size(); };

  size_t inPos = 0;
  std::optional<size_t> inLastDict, outLastDict;
  // raw encoded strings of the current dictionary, in order, and the
  // version bytes of the most recent clear (to replicate it)
  std::string dictBytes;
  std::string clearVersion;
  size_t records = 0, checkpoints = 0;

  auto emitFrame = [&] {
    frameStartOut += frame.size();
    sink.add(std::move(frame));
    frame = std::vector<char>();
    frame.reserve(opts.frameSize + opts.frameSize / 8);
  };

  auto checkBackref = [&](uint32_t backref, char type) {
    if (!inLastDict)
      THROW_RT("au '" << type << "' record at " << inPos
               << " precedes any dictionary record");
    if (backref != inPos - *inLastDict)
      THROW_RT("Unexpected backref in au '" << type << "' record at " << inPos
               << ": " << backref << " (expected " << inPos - *inLastDict
               << "). This au layout isn't supported by --au.");
  };

  while (r.ensure(1)) {
    auto type = static_cast<char>(r.at(0));
    size_t len = 0;
    try {
      switch (type) {
        case 'H': {
          if (!r.ensure(3) || ::memcmp(r.data(), "HAU", 3) != 0)
            THROW_RT("Bad au header at " << inPos);
          len = sc.term(sc.string(sc.formatVersion(3)));
          break;
        }
        case 'C':
          len = sc.term(sc.formatVersion(1));
          break;
        case 'A': {
          size_t off = 5;
          sc.backref(1);
          while (sc.byte(off) != marker::RecordEnd) off = sc.string(off);
          len = sc.term(off);
          break;
        }
        case 'V': {
          sc.backref(1);
          uint64_t valueLen;
          auto off = sc.varint(5, &valueLen);
          len = off + valueLen;
          if (!r.ensure(len)) throw TruncatedRecord{};
          break;
        }
        default:
          THROW_RT("Unexpected au record type 0x" << std::hex
                   << static_cast<int>(static_cast<uint8_t>(type)) << std::dec
                   << " at input position " << inPos);
      }
    } catch (TruncatedRecord &) {
      std::cerr << "Warning: input ends with a partial au record at position "
                << inPos << "; copying the remaining " << r.available()
                << " bytes as-is\n";
      frame.insert(frame.end(), r.data(), r.data() + r.available());
      r.consume(r.available());
      break;
    }

    // cut a new frame before this record if the current one is full. a 'C'
    // or 'H' record needs no preceding dictionary, so only checkpoint before
    // 'A' and 'V'.
    if (frame.size() >= opts.frameSize) {
      emitFrame();
      if (type == 'A' || type == 'V') {
        // re-establish the current dictionary at the start of the frame
        checkpoints++;
        std::string cp;
        cp.reserve(dictBytes.size() + clearVersion.size() + 16);
        cp += 'C';
        cp += clearVersion;
        cp += static_cast<char>(marker::RecordEnd);
        cp += '\n';
        outLastDict = outPos();
        if (!dictBytes.empty()) {
          auto addPos = outPos() + cp.size();
          cp += 'A';
          appendBackref(cp, addPos - *outLastDict);
          cp += dictBytes;
          cp += static_cast<char>(marker::RecordEnd);
          cp += '\n';
          outLastDict = addPos;
        }
        frame.insert(frame.end(), cp.begin(), cp.end());
      }
    }

    const char *rec = r.data();
    switch (type) {
      case 'H':
        frame.insert(frame.end(), rec, rec + len);
        break;
      case 'C':
        clearVersion.assign(rec + 1, len - 3);
        dictBytes.clear();
        inLastDict = inPos;
        outLastDict = outPos();
        frame.insert(frame.end(), rec, rec + len);
        break;
      case 'A':
      case 'V': {
        uint32_t backref;
        ::memcpy(&backref, rec + 1, 4);
        checkBackref(backref, type);
        auto here = outPos();
        frame.push_back(type);
        appendBackref(frame, here - *outLastDict);
        frame.insert(frame.end(), rec + 5, rec + len);
        if (type == 'A') {
          dictBytes.append(rec + 5, len - 7); // strings, without the term
          inLastDict = inPos;
          outLastDict = here;
        } else {
          records++;
        }
        break;
      }
      default: break; // unreachable
    }
    inPos += len;
    r.consume(len);
  }
  emitFrame();

  if (opts.verbose)
    std::cerr << "\nau: " << records << " value records, " << checkpoints
              << " dictionary checkpoints\n";
}

void plainFrames(FILE *in, FrameSink &sink, const ZstdCompressOptions &opts) {
  while (true) {
    std::vector<char> buf(opts.frameSize);
    auto n = readFully(in, buf.data(), buf.size());
    if (n == 0) break;
    buf.resize(n);
    sink.add(std::move(buf));
    if (n < opts.frameSize) break;
  }
}

}

int zstdCompressFile(const std::string &inFile, const std::string &outFile,
                     const ZstdCompressOptions &opts) {
  if (opts.frameSize == 0
      || opts.frameSize > ZSTD_SEEKABLE_MAX_FRAME_DECOMPRESSED_SIZE)
    THROW_RT("Frame size must be between 1 and "
             << ZSTD_SEEKABLE_MAX_FRAME_DECOMPRESSED_SIZE);

  File inOwner;
  FILE *in = stdin;
  if (inFile != "-") {
    inOwner.reset(::fopen(inFile.c_str(), "rb"));
    if (!inOwner) THROW_RT("fopen: " << strerror(errno) << " (" << inFile << ")");
    in = inOwner.get();
  }
  File outOwner;
  FILE *out = stdout;
  if (outFile != "-") {
    outOwner.reset(::fopen(outFile.c_str(), "wb"));
    if (!outOwner)
      THROW_RT("fopen: " << strerror(errno) << " (" << outFile << ")");
    out = outOwner.get();
  }

  FrameSink sink(out, opts);
  if (opts.auFrames) auFrames(in, sink, opts);
  else plainFrames(in, sink, opts);
  sink.finish();

  if (opts.verbose) {
    std::cerr << "\r" << inFile << ": " << sink.totalIn << " -> "
              << sink.totalOut << " bytes in " << sink.numFrames << " frames";
    if (sink.totalIn)
      std::cerr << " (" << 100.0 * static_cast<double>(sink.totalOut)
                         / static_cast<double>(sink.totalIn) << "%)";
    std::cerr << "\n";
  }
  return 0;
}

}
