#include "au/AuEncoder.h"
#include "au/ParseError.h"
#include "DocumentParser.h"
#include "Zindex.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

// this file contains code adapted from https://github.com/mattgodbolt/zindex

namespace au {

namespace {

constexpr size_t DefaultIndexEvery = 8 * 1024 * 1024u;
constexpr size_t WindowSize = 32768u;
constexpr size_t ChunkSize = 16384u;
constexpr auto Version = 1u;

std::string getRealPath(const std::string &relPath) {
  char realPathBuf[PATH_MAX];
  auto result = realpath(relPath.c_str(), realPathBuf);
  if (result == nullptr) return relPath;
  return std::string(relPath);
}

std::string getBaseName(const std::string &path) {
  std::vector<char> chars(path.begin(), path.end());
  chars.push_back(0);
  return std::string(basename(&chars[0]));
}

struct ZlibError : std::runtime_error {
  ZlibError(int result)
  : std::runtime_error(std::string("Error from zlib : ") + zError(result)) {}
};

void X(int zlibErr) {
  if (zlibErr != Z_OK) throw ZlibError(zlibErr);
}

size_t makeWindow(uint8_t *out, size_t outSize, const uint8_t *in,
                  uint64_t left) {
  uint8_t temp[WindowSize];
  // Could compress directly into out if I (mgodbolt, that is) wasn't so lazy.
  if (left)
    memcpy(temp, in + WindowSize - left, left);
  if (left < WindowSize)
    memcpy(temp + left, in, WindowSize - left);
  uLongf destLen = outSize;
  X(compress2(out, &destLen, temp, WindowSize, 9));
  return destLen;
}

void uncompressWindow(const std::vector<uint8_t> &compressed, uint8_t *to,
                      size_t len) {
    uLongf destLen = len;
    X(::uncompress(to, &len, &compressed[0], compressed.size()));
    if (destLen != len)
        THROW_RT("Unable to decompress a full window");
}

struct ZStream {
  z_stream stream;
  enum class Type : int {
    ZlibOrGzip = 47, Raw = -15,
  };
  Type type;

  explicit ZStream(Type type) : type(type) {
    memset(&stream, 0, sizeof(stream));
    X(inflateInit2(&stream, static_cast<int>(type)));
  }

  void reset() {
    X(inflateReset(&stream));
  }

  ~ZStream() {
    (void)inflateEnd(&stream);
  }

  ZStream(ZStream &) = delete;
  ZStream &operator=(ZStream &) = delete;
};

struct Closer {
    void operator()(FILE *f) {
        if (f) ::fclose(f);
    }
};

// A File is a self-closing FILE *.
using File = std::unique_ptr<FILE, Closer>;

struct CachedContext {
  ZStream zs_;
  size_t pos_ = 0; // current absolute position in stream
  size_t cur_ = 0; // current offset into ZipByteSource::output_
  size_t limit_ = 0; // number of valid bytes in ZipByteSource::output_
  bool eof_ = false;

  explicit CachedContext()
      : zs_(ZStream::Type::ZlibOrGzip) {}

  explicit CachedContext(size_t uncompressedOffset)
      : zs_(ZStream::Type::Raw),
        pos_(uncompressedOffset) {}
};

std::string getIndexFilename(const std::string &filename,
                          const std::optional<std::string> &indexFilename) {
  if (indexFilename) return *indexFilename;
  return getRealPath(filename) + ".auzx";
}

}

int zindexFile(const std::string &fileName,
               const std::optional<std::string> &indexFilename) {
  size_t indexEvery = DefaultIndexEvery; // TODO extract

  auto ifn = getIndexFilename(fileName, indexFilename);
  std::cout << "Indexing " << fileName << " to " << ifn << "...\n";

  // open gzipped file, or fail...
  File from(fopen(fileName.c_str(), "rb"));
  if (from.get() == nullptr) {
      std::cerr << "Could not open " << fileName << " for reading\n";
      return 1;
  }
  struct stat compressedStat;
  if (fstat(fileno(from.get()), &compressedStat) != 0)
    throw ZlibError(Z_DATA_ERROR);

  // open index file, or fail...
  // TODO fail if file exists...
  if (unlink(ifn.c_str()) == 0)
    std::cout << "Rebuilding existing index " << ifn << std::endl;
  std::ofstream out;
  out.open(ifn, std::ios_base::binary);
  if (!out) {
    std::cerr << "Unable to open output " << ifn << std::endl; // TODO strerror, etc
    return 1;
  }

  AuEncoder idx(AU_STR("Index of " << fileName << ", written by au"));
  auto emit = [&](auto &&f) {
    idx.encode(f, [&](std::string_view dict, std::string_view val) {
      out << dict << val; // TODO error if write fails
      return dict.size() + val.size();
    });
  };

  emit([&](AuWriter &au) {
    au.map(
      "fileType", "zindex",
      "version", Version,
      "compressedFile", getBaseName(fileName),
      "compressedSize", static_cast<uint64_t>(compressedStat.st_size),
      "compressedModTime", static_cast<uint64_t>(compressedStat.st_mtime)
    );
  });

  // actually build the index...
  ZStream zs(ZStream::Type::ZlibOrGzip);
  uint8_t input[ChunkSize];
  uint8_t window[WindowSize];

  int ret = 0;
  uint64_t totalIn = 0;
  uint64_t totalOut = 0;
  uint64_t last = 0;
  bool emitInitialAccessPoint = true;

  do {
    if (zs.stream.avail_in == 0) {
      zs.stream.avail_in =
        static_cast<uInt>(fread(input, 1, ChunkSize, from.get()));
      if (ferror(from.get()))
        throw ZlibError(Z_ERRNO);
      if (zs.stream.avail_in == 0)
        throw ZlibError(Z_DATA_ERROR);
      zs.stream.next_in = input;
    }
    do {
      if (zs.stream.avail_out == 0) {
        zs.stream.avail_out = WindowSize;
        zs.stream.next_out = window;
      }
      totalIn += zs.stream.avail_in;
      totalOut += zs.stream.avail_out;
      ret = inflate(&zs.stream, Z_BLOCK);
      totalIn -= zs.stream.avail_in;
      totalOut -= zs.stream.avail_out;
      if (ret == Z_NEED_DICT)
        throw ZlibError(Z_DATA_ERROR);
      if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
        throw ZlibError(ret);
      if (ret == Z_STREAM_END)
        break;
      auto sinceLast = totalOut - last;
      bool needsIndex = sinceLast > indexEvery
                        || emitInitialAccessPoint;
      bool endOfBlock = zs.stream.data_type & 0x80;
      bool lastBlockInStream = zs.stream.data_type & 0x40;
      if (endOfBlock && !lastBlockInStream && needsIndex) {
        std::cout << "Creating checkpoint at " << totalOut <<
          " (compressed offset " << totalIn << ")\n";
        uint8_t apWindow[compressBound(WindowSize)];
        auto size = makeWindow(apWindow, sizeof(apWindow), window,
            zs.stream.avail_out);
        auto windowStr =
            std::string_view(reinterpret_cast<char *>(apWindow), size);
        emit([&](AuWriter &au) {
          au.map(
            "uncompressedOffset", totalOut,
            "compressedOffset", totalIn,
            "bitOffset", zs.stream.data_type & 0x7,
            "window", windowStr
          );
        });

        last = totalOut;
        emitInitialAccessPoint = false;
      }
      //progress.update<PrettyBytes>(totalIn, compressedStat.st_size); TODO?
    } while (zs.stream.avail_in);
  } while (ret != Z_STREAM_END);

  if (zs.stream.avail_in || !feof(from.get())) {
    std::cout << "\n"
              << "WARNING: this file appears to contain multiple gzip blocks.\n"
              << "This tool does not currently support such files!\n"
              << "Data beyond the first block will not be indexed.\n\n";
  }

  // TODO find a better way to record the total uncompressed size...
  std::cout << "Writing final entry...\n";
  emit([&](AuWriter &au) {
    au.map(
        "uncompressedOffset", totalOut,
        "compressedOffset", totalIn,
        "bitOffset", zs.stream.data_type & 0x7,
        "window", ""
    );
  });

  std::cout << "Index complete.\n";
  return 0;
}

struct Zindex {
  struct IndexEntry {
    size_t compressedOffset = 0;
    size_t uncompressedOffset = 0;
    int bitOffset = 0;
    std::vector<uint8_t> window;
  };

  std::vector<IndexEntry> index_;
  std::string compressedFilename;
  size_t compressedSize = 0;
  size_t compressedModTime = 0;

  Zindex(const std::string &filename) {
    FileByteSourceImpl source(filename, false);
    Dictionary dictionary;

    DocumentParser metadataParser;
    metadataParser.parse(source, dictionary);
    auto &meta = metadataParser.document();
    if (!meta.IsObject())
      THROW_RT("First record in index file is not a json object!");
    auto fileType = std::string_view(meta["fileType"].GetString(),
                                     meta["fileType"].GetStringLength());
    if (fileType != "zindex")
      THROW_RT("Wrong fileType in index, expected 'zindex'");
    if (!meta["version"].IsInt() || meta["version"].GetInt() != 1)
      THROW_RT("Wrong version index, expected version 1");
    compressedFilename =
        std::string_view(meta["compressedFile"].GetString(),
                         meta["compressedFile"].GetStringLength());
    compressedSize = meta["compressedSize"].GetUint64();
    compressedModTime = meta["compressedModTime"].GetUint64();

    while (source.peek() != AuByteSource::Byte::Eof()) {
      DocumentParser entryParser;
      entryParser.parse(source, dictionary);
      auto &entry = entryParser.document();
      auto compressedOffset = entry["compressedOffset"].GetUint64();
      auto uncompressedStartOffset = entry["uncompressedOffset"].GetUint64();
      auto bitOffset = entry["bitOffset"].GetInt();
      auto window = std::string_view(entry["window"].GetString(),
          entry["window"].GetStringLength());
      index_.emplace_back(IndexEntry {
        compressedOffset,
        uncompressedStartOffset,
        bitOffset,
        std::vector<uint8_t>(window.begin(), window.end())
      });
    }

    if (index_.empty())
      THROW_RT("Index should contain at least one entry!");

    const auto &last = index_.back();
    if (!last.window.empty()) {
      THROW_RT("Index appears to be incomplete: Final entry has"
               " non-empty compression window data.");
    }

    if (last.compressedOffset != compressedSize) {
      THROW_RT("Index appears to be incomplete: Final entry has"
               " compressed offset " << last.compressedOffset
               << " but metadata shows compressed size " << compressedSize);
    }
  }

  size_t numEntries() const { return index_.size(); }

  size_t uncompressedSize() const {
    // total stream size is "start" of dummy final entry
    return index_.back().uncompressedOffset;
  }

  IndexEntry &find(size_t abspos) {
    auto it = std::upper_bound(
        index_.begin(), index_.end(), abspos,
        [](size_t pos, const IndexEntry &entry) {
          return pos < entry.uncompressedOffset;
        });
    if (it == index_.begin())
      THROW_RT("Couldn't find index entry containing " << abspos);
    --it;
    return *it;
  }
};

struct ZipByteSource::Impl {
  File compressed_;
  Zindex index_;
  // based on the average block size, used to determine whether to seek forward
  // by decompressing or seeking to a new block. also used to determine the size
  // of the decompression lookback buffer to use after each seek
  size_t blockSize_;
  std::unique_ptr<CachedContext> context_;
  uint8_t input_[ChunkSize];
  uint8_t *output_;

  Impl(const std::string &fname,
       const std::optional<std::string> &indexFname)
      : compressed_(fopen(fname.c_str(), "rb")),
        index_(getIndexFilename(fname, indexFname)),
        blockSize_(2 * index_.uncompressedSize() / index_.numEntries()),
        context_(new CachedContext()),
        output_(new uint8_t[blockSize_]) {
    if (compressed_.get() == nullptr)
      THROW_RT("Could not open " << fname << " for reading");

    if (index_.compressedFilename != getBaseName(fname))
      THROW_RT("Wrong compressed filename in index: '"
                   << index_.compressedFilename << "', expected '"
                   << getBaseName(fname) << "'");

    struct stat stats;
    if (fstat(fileno(compressed_.get()), &stats) != 0)
      THROW_RT("Unable to get file stats"); // TODO errno
    if (stats.st_size != static_cast<int64_t>(index_.compressedSize))
      THROW_RT("Compressed size changed since index was built");
    if (index_.compressedModTime != static_cast<uint64_t>(stats.st_mtime))
      THROW_RT("Compressed file has been modified since index was built");

    context_->zs_.stream.avail_in = 0;
  }

  ~Impl() {
    delete[] output_;
  }

  ssize_t doRead(char *buf, size_t len) {
    auto &c = *context_;
    if (c.cur_ == c.limit_) gzread();
    auto n = std::min(c.limit_ - c.cur_, len);
    ::memcpy(buf, output_+c.cur_, n);
    c.cur_ += n;
    c.pos_ += n;
    return static_cast<ssize_t>(n);
  }

  size_t endPos() const {
    return index_.uncompressedSize();
  }

  bool isSeekable() const {
    // for now, all ZipByteSources are indexed and thus should be seekable.
    // this may not be true forever, if we add support for sequential reading
    // of non-indexed gz files...
    return true;
  }

  void doSeek(size_t abspos) {
    auto &c = *context_;

    if (abspos < c.pos_ && c.pos_ - abspos <= c.cur_) {
      // we're seeking backward within current buffer
      auto relseek = c.pos_ - abspos;
      c.cur_ -= relseek;
      c.pos_ -= relseek;
      return;
    }

    size_t bufRemaining = c.limit_ - c.cur_;
    if (abspos > c.pos_ && abspos - c.pos_ <= bufRemaining) {
      // we're seeking forward within current buffer
      auto relseek = abspos - c.pos_;
      c.cur_ += relseek;
      c.pos_ += relseek;
      return;
    }

    // now we're either seeking backward, or else forward beyond end of
    // buffer. do we really need to seek? or can we just skip ahead some?
    if (abspos < c.pos_ || abspos > blockSize_ + bufRemaining) {
      // we're either seeking backward beyond the start of output_,
      // or far enough forward that it's better to jump instead of scan.
      Zindex::IndexEntry &indexEntry = index_.find(abspos);
      auto compressedOffset = indexEntry.compressedOffset;
      auto uncompressedOffset = indexEntry.uncompressedOffset;
      auto bitOffset = indexEntry.bitOffset;
      //log_.debug("Creating new context at offset ", compressedOffset); TODO
      context_.reset(new CachedContext(uncompressedOffset));
      uint8_t window[WindowSize];
      uncompressWindow(indexEntry.window, window, WindowSize);

      size_t seekPos = bitOffset ? compressedOffset - 1 : compressedOffset;
      auto err = ::fseek(compressed_.get(), static_cast<long>(seekPos),
        SEEK_SET);
      if (err != 0)
        THROW_RT("Error seeking in file"); // todo errno
      context_->zs_.stream.avail_in = 0;
      if (bitOffset) {
        auto ch = fgetc(compressed_.get());
        if (ch == -1)
          throw ZlibError(ferror(compressed_.get()) ?
              Z_ERRNO : Z_DATA_ERROR);
        X(inflatePrime(&context_->zs_.stream, bitOffset,
          ch >> (8 - bitOffset)));
      }
      X(inflateSetDictionary(&context_->zs_.stream, &window[0], WindowSize));
    }

    if (abspos < context_->pos_)
      THROW_RT("Invariant abspos >= context_->pos_ doesn't hold: abspos = "
                   << abspos << ", context_->pos_ = " << context_->pos_);
    char discardBuffer[WindowSize];
    auto numToSkip = abspos - context_->pos_;
    while (numToSkip) {
      auto skipNow = std::min(WindowSize, numToSkip);
      auto numRead = doRead(discardBuffer, skipNow);
      if (numRead <= 0) THROW_RT("Unable to skip any bytes!");
      numToSkip -= static_cast<size_t>(numRead);
    }
  }

  size_t gzread() {
    if (context_->eof_) return 0;

    if (context_->cur_ != context_->limit_)
      THROW_RT("Shouldn't call gzread() unless cur_ == limit_!");

    if (context_->cur_ == blockSize_) {
      // buffer is full. we're only called when we're supposed to read
      // something, so just clear it and continue...
      context_->cur_ = context_->limit_ = 0;
    }

    auto &zs = context_->zs_;
    zs.stream.next_out = output_+context_->limit_;
    zs.stream.avail_out = static_cast<uInt>(
        std::min(blockSize_ - context_->limit_, ChunkSize)); // TODO ChunkSize or whatever...
    size_t total = 0;
    do {
      if (zs.stream.avail_in == 0) {
        zs.stream.avail_in = static_cast<uInt>(
          ::fread(input_, 1, sizeof(input_), compressed_.get()));
        if (ferror(compressed_.get())) throw ZlibError(Z_ERRNO);
        zs.stream.next_in = input_;
      }
      auto availBefore = zs.stream.avail_out;
      auto ret = inflate(&zs.stream, Z_NO_FLUSH);
      if (ret == Z_NEED_DICT) throw ZlibError(Z_DATA_ERROR);
      if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
        throw ZlibError(ret);
      auto numUncompressed = availBefore - zs.stream.avail_out;
      context_->limit_ += numUncompressed;
      total += numUncompressed;
      if (ret == Z_STREAM_END) {
        // this is the end of the first gzip block. we don't currently
        // support multiple blocks... it's hard for us to detect here whether
        // there are multiple blocks or not, because in the presence of the
        // gzip framing data, the underlying file might not actually be at
        // eof. so we don't bother to detect or warn. they will have gotten
        // the warning when the built the index, at least.
        context_->eof_ = true;
        break;
      }
    } while (zs.stream.avail_out);
    return total;
  }
};

ZipByteSource::ZipByteSource(const std::string &fname,
                             const std::optional<std::string> &indexFilename)
: FileByteSource(fname, false),
  impl_(std::make_unique<Impl>(fname, indexFilename)) {}

ZipByteSource::~ZipByteSource() {}

bool ZipByteSource::isSeekable() const {
  return impl_->isSeekable();
}

ssize_t ZipByteSource::doRead(char *buf, size_t len) {
  return impl_->doRead(buf, len);
}

size_t ZipByteSource::endPos() const {
  return impl_->endPos();
}

void ZipByteSource::doSeek(size_t abspos) {
  return impl_->doSeek(abspos);
}

}
