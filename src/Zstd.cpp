#include "Zstd.h"

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
#include <thread>
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

  // A frame being (lazily) decompressed. We only decompress as far into the
  // frame as we've been asked to read, so a seek costs on average half a
  // frame of decompression rather than a whole one.
  struct Frame {
    std::optional<size_t> index;
    std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx{ZSTD_createDCtx()};
    std::vector<char> compressed;
    std::vector<char> data; // sized to the full decompressed frame
    ZSTD_inBuffer in{nullptr, 0, 0};
    size_t decoded = 0; // bytes of data that are valid
    uint64_t lastUse = 0;
  };
  // binary search over an au file bounces between a probe point and the
  // dictionary preceding it, which is often in an earlier frame, so we keep a
  // few frames around.
  static constexpr size_t FrameCacheSize = 4;
  // when we need more of a frame, decompress at least this much more, to
  // amortize per-call overhead
  static constexpr size_t MinDecodeChunk = 1024 * 1024;
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
    f.data.resize(dSize);
    if (::fseeko(file_.get(), static_cast<off_t>(cOffsets_[index]),
                 SEEK_SET) != 0)
      THROW_RT("Error seeking in zstd file: " << strerror(errno));
    if (::fread(f.compressed.data(), 1, cSize, file_.get()) != cSize)
      THROW_RT("Error reading zstd frame " << index << ": "
               << (::ferror(file_.get()) ? strerror(errno) : "truncated file"));
    f.in = ZSTD_inBuffer{f.compressed.data(), cSize, 0};
    f.decoded = 0;
    f.index = index;
    f.lastUse = ++useCounter_;
    return f;
  }

  /// Ensure at least `upTo` bytes of the frame are decompressed.
  void decodeTo(Frame &f, size_t upTo) {
    if (f.decoded >= upTo) return;
    auto target = std::min(f.data.size(), std::max(upTo, f.decoded + MinDecodeChunk));
    ZSTD_outBuffer out{f.data.data(), target, f.decoded};
    while (out.pos < target) {
      auto before = out.pos;
      auto ret = check(ZSTD_decompressStream(f.dctx.get(), &out, &f.in),
                       "zstd decompression failed");
      if (ret == 0 || (out.pos == before && f.in.pos == f.in.size)) break;
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
    decodeTo(f, offset + n);
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
  if (source.pos() != static_cast<size_t>(source.cur_ - source.buf_))
    THROW_RT("ZstdByteSource must be created from a source at position 0");
  std::string_view buffered(
      source.buf_, static_cast<size_t>(source.limit_ - source.buf_));
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

  std::unique_ptr<ZSTD_frameLog, FrameLogDeleter> frameLog(
      ZSTD_seekable_createFrameLog(0));
  if (!frameLog) THROW_RT("Unable to create zstd seekable frame log");

  auto threads = std::max(1u, opts.threads);
  size_t totalIn = 0;
  size_t totalOut = 0;
  size_t numFrames = 0;

  auto emit = [&](CompressedFrame frame) {
    writeAll(out, frame.data.data(), frame.data.size());
    check(ZSTD_seekable_logFrame(frameLog.get(),
                                 static_cast<unsigned>(frame.data.size()),
                                 static_cast<unsigned>(frame.srcSize), 0),
          "Logging seekable frame");
    totalIn += frame.srcSize;
    totalOut += frame.data.size();
    numFrames++;
    if (opts.verbose)
      std::cerr << "\rframes: " << numFrames << ", in: " << totalIn
                << ", out: " << totalOut << std::flush;
  };

  // Read frames sequentially and compress them in parallel, keeping at most
  // `threads` frames in flight. Output is written in order.
  std::deque<std::future<CompressedFrame>> inFlight;
  while (true) {
    std::vector<char> buf(opts.frameSize);
    auto n = readFully(in, buf.data(), buf.size());
    if (n == 0) break;
    buf.resize(n);
    if (inFlight.size() >= threads) {
      emit(inFlight.front().get());
      inFlight.pop_front();
    }
    inFlight.push_back(std::async(std::launch::async, compressFrame,
                                  std::move(buf), std::cref(opts)));
    if (n < opts.frameSize) break;
  }
  while (!inFlight.empty()) {
    emit(inFlight.front().get());
    inFlight.pop_front();
  }

  std::vector<char> tableBuf(64 * 1024);
  while (true) {
    ZSTD_outBuffer tableOut{tableBuf.data(), tableBuf.size(), 0};
    auto remaining = check(ZSTD_seekable_writeSeekTable(frameLog.get(), &tableOut),
                           "Writing seek table");
    writeAll(out, tableBuf.data(), tableOut.pos);
    totalOut += tableOut.pos;
    if (remaining == 0) break;
  }

  if (::fflush(out) != 0)
    THROW_RT("Error writing output: " << strerror(errno));
  if (opts.verbose) {
    std::cerr << "\r" << inFile << ": " << totalIn << " -> " << totalOut
              << " bytes in " << numFrames << " frames";
    if (totalIn)
      std::cerr << " (" << 100.0 * static_cast<double>(totalOut)
                         / static_cast<double>(totalIn) << "%)";
    std::cerr << "\n";
  }
  return 0;
}

}
