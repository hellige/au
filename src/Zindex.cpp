#include "au/AuDecoder.h"
#include "au/AuEncoder.h"
#include "au/ParseError.h"
#include "AuRecordHandler.h"
#include "Dictionary.h"
#include "Zindex.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <sys/stat.h>

// this file contains code adapted from https://github.com/mattgodbolt/zindex

namespace {

constexpr auto DefaultIndexEvery = 8 * 1024 * 1024u;
constexpr auto WindowSize = 32768u;
constexpr auto ChunkSize = 16384u;
constexpr auto Version = 1u;

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
    X(inflateInit2(&stream, (int)type));
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
  size_t uncompressedOffset_;
  size_t blockSize_;
  uint8_t input_[ChunkSize];
  ZStream zs_;
  bool eof_ = false;

  explicit CachedContext(size_t blockSize)
      : uncompressedOffset_(0),
        blockSize_(blockSize),
        zs_(ZStream::Type::ZlibOrGzip) {}

  explicit CachedContext(size_t uncompressedOffset, size_t blockSize)
      : uncompressedOffset_(uncompressedOffset),
        blockSize_(blockSize),
        zs_(ZStream::Type::Raw) {}

  bool offsetWithinRange(size_t offset) const {
    if (offset < uncompressedOffset_) return false; // can't seek backwards
    size_t jumpAhead = offset - uncompressedOffset_;
    return jumpAhead < blockSize_;
  }
};

std::string indexFilename(const std::string &filename) {
  return filename + ".auzx"; // TODO add command line arg, realpath stuff?
}

}

int zindexFile(const std::string &fileName) {
  size_t indexEvery = DefaultIndexEvery; // TODO extract

  auto ifn = indexFilename(fileName);

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

  AuEncoder idx(STR("Index of " << fileName << ", written by au"));
  auto emit = [&](auto &&f) {
    idx.encode(f, [&](std::string_view dict, std::string_view val) {
      out << dict << val; // TODO error if write fails
      return dict.size() + val.size();
    });
  };

  /*
   * TODO this would be much cleaner...
  emit([&](AuWriter &au) {
    au.map(
      "fileType", "zindex",
      "version", Version, // TODO what else?
      "compressedFile", fileName // TODO just filename rather than possibly full path?
    );
  });
  */
  emit([&](AuWriter &au) { au.value(Version); });
  emit([&](AuWriter &au) { au.value(compressedStat.st_size); });
  emit([&](AuWriter &au) { au.value((uint64_t)compressedStat.st_mtime); }); // TODO what cast here?

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
      zs.stream.avail_in = fread(input, 1, ChunkSize, from.get());
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
        auto window =
            std::string_view((char *)apWindow, size); // TODO what's the right way to cast this?
          /* TODO fix this...
        emit([&](AuWriter &au) {
          au.map(
            "uncompressedOffset", totalOut,
            "compressedOffset", totalIn,
            "bitOffset", zs.stream.data_type & 0x7,
            "window", window
          );
        });
           */
        emit([&](AuWriter &au) { au.value(totalIn); });
        emit([&](AuWriter &au) { au.value(totalOut); });
        emit([&](AuWriter &au) { au.value(zs.stream.data_type & 0x7); });
        emit([&](AuWriter &au) { au.value(window); });

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
  emit([&](AuWriter &au) { au.value(totalIn); });
  emit([&](AuWriter &au) { au.value(totalOut); });
  emit([&](AuWriter &au) { au.value(zs.stream.data_type & 0x7); });
  emit([&](AuWriter &au) { au.value(""); });

  std::cout << "Index complete.\n";
  return 0;
}

template <typename Base>
struct SingleValueParser : Base {
  typename Base::type parse(FileByteSource &source, Dictionary &dictionary) {
    AuRecordHandler rh(dictionary, *this);
    if (!RecordParser(source, rh).parseUntilValue())
      THROW_RT("SingleValueParser failed to parse value record!");
    auto &v = static_cast<Base*>(this)->value;
    if (v) return *v;
    THROW_RT("SingleValueParser failed to parse value!");
  }

  void onValue(FileByteSource &source, const Dictionary::Dict &) {
    ValueParser<Base> vp(source, *this);
    vp.value();
  }
};

struct ThrowParser {
  void onObjectStart() { THROW_RT("Bad value."); }
  void onObjectEnd() {}
  void onArrayStart() { THROW_RT("Bad value."); }
  void onArrayEnd() {}
  void onNull(size_t) { THROW_RT("Bad value."); }
  void onBool(size_t, bool) { THROW_RT("Bad value."); }
  void onInt(size_t, int64_t) { THROW_RT("Bad value."); }
  void onUint(size_t, uint64_t) { THROW_RT("Bad value."); }
  void onDouble(size_t, double) { THROW_RT("Bad value."); }
  void onTime(size_t, std::chrono::system_clock::time_point) { THROW_RT("Bad value."); }
  void onDictRef(size_t, size_t) { THROW_RT("Bad value."); }
  void onStringStart(size_t, size_t) { THROW_RT("Bad value."); }
  void onStringEnd() {}
  void onStringFragment(std::string_view) {}
};

struct IntParser : ThrowParser {
  using type = uint64_t;
  std::optional<type> value;
  void onUint(size_t, uint64_t val) { value = val; }
};

struct StringParser : ThrowParser {
  using type = std::string;
  std::vector<char> str;
  std::optional<type> value;
  void onStringStart(size_t, size_t len) { str.clear(); str.reserve(len); }
  void onStringEnd() { value = std::string_view(str.data(), str.size()); }
  void onStringFragment(std::string_view frag) {
    str.insert(str.end(), frag.data(), frag.data() + frag.size());
  }
};

struct Zindex {
  struct IndexEntry {
    size_t compressedOffset = 0;
    size_t uncompressedOffset = 0;
    int bitOffset = 0;
    std::vector<uint8_t> window;
  };

  std::vector<IndexEntry> index_;
  size_t compressedSize = 0;
  size_t compressedModTime = 0;

  Zindex(const std::string &filename) {
    FileByteSourceImpl source(filename, false);
    Dictionary dictionary;
    auto version = SingleValueParser<IntParser>().parse(source, dictionary);
    if (version != 1)
      THROW_RT("Wrong version index " << version << ", expected version 1");
    compressedSize = SingleValueParser<IntParser>().parse(source, dictionary);
    compressedModTime
        = SingleValueParser<IntParser>().parse(source, dictionary);

    while (source.peek() != FileByteSource::Byte::Eof()) {
      auto compressedOffset =
          SingleValueParser<IntParser>().parse(source, dictionary);
      auto uncompressedStartOffset =
          SingleValueParser<IntParser>().parse(source, dictionary);
      auto bitOffset = SingleValueParser<IntParser>().parse(source, dictionary);
      auto window = SingleValueParser<StringParser>().parse(source, dictionary);
      index_.emplace_back(IndexEntry {
        compressedOffset,
        uncompressedStartOffset,
        (int)bitOffset, // TODO cast?
        std::vector<uint8_t>(window.begin(), window.end())
      });
    }

    if (index_.empty())
      THROW_RT("Index should contain at least one entry!");
  }

  size_t numEntries() const { return index_.size(); }

  size_t uncompressedSize() const {
    return index_.back().uncompressedOffset;
  }

  IndexEntry &find(size_t abspos) {
    auto it = std::upper_bound(
        index_.begin(), index_.end(), abspos,
        [](size_t abspos, const IndexEntry &entry) {
          return abspos < entry.uncompressedOffset;
        });
    if (it == index_.begin())
      THROW_RT("Couldn't find index entry containning " << abspos);
    --it;
    return *it;
  }
};

struct ZipByteSource::Impl {
  File compressed_;
  size_t blockSize_;
  std::unique_ptr<CachedContext> cachedContext_;
  Zindex index_;

  Impl(const std::string &fname) 
  : compressed_(fopen(fname.c_str(), "rb")),
    index_(indexFilename(fname)) {
    if (compressed_.get() == nullptr)
      THROW_RT("Could not open " << fname << " for reading");

    struct stat stats;
    if (fstat(fileno(compressed_.get()), &stats) != 0)
      THROW_RT("Unable to get file stats"); // TODO errno
    if (stats.st_size != static_cast<int64_t>(index_.compressedSize))
      THROW_RT("Compressed size changed since index was built");
    if (index_.compressedModTime != (uint64_t)stats.st_mtime) // TODO what cast here?
      THROW_RT("Compressed file has been modified since index was built");

    // average block size, used to determine whether to seek forward by 
    // decompressing or seeking to a new block
    blockSize_ = index_.uncompressedSize() / index_.numEntries();

    cachedContext_.reset(new CachedContext(blockSize_));
    cachedContext_->zs_.stream.avail_in = 0;
  }

  size_t doRead(char *buf, size_t len) {
    return gzread(*cachedContext_, (uint8_t*)buf, len); // TODO what's the right cast here?
  }

  size_t endPos() const {
    return index_.uncompressedSize();
  }

  void doSeek(size_t abspos) {
    // We use and update context while in here. Only if we successfully
    // decode a line do we save it in the cachedContext_ for a subsequent
    // call.
    std::unique_ptr<CachedContext> context;
    if (cachedContext_->offsetWithinRange(abspos)) {
      // We can reuse the previous context.
      //log_.debug("Reusing previous context"); TODO
      context = std::move(cachedContext_);
    } else {
      Zindex::IndexEntry &indexEntry = index_.find(abspos);
      auto compressedOffset = indexEntry.compressedOffset;
      auto uncompressedOffset = indexEntry.uncompressedOffset;
      auto bitOffset = indexEntry.bitOffset;
      //log_.debug("Creating new context at offset ", compressedOffset); TODO
      context.reset(new CachedContext(uncompressedOffset, blockSize_));
      uint8_t window[WindowSize];
      uncompressWindow(indexEntry.window, window, WindowSize);

      auto seekPos = bitOffset ? compressedOffset - 1 : compressedOffset;
      auto err = ::fseek(compressed_.get(), seekPos, SEEK_SET);
      if (err != 0)
        THROW_RT("Error seeking in file"); // todo errno
      context->zs_.stream.avail_in = 0;
      if (bitOffset) {
        auto c = fgetc(compressed_.get());
        if (c == -1)
          throw ZlibError(ferror(compressed_.get()) ?
              Z_ERRNO : Z_DATA_ERROR);
        X(inflatePrime(&context->zs_.stream,
              bitOffset, c >> (8 - bitOffset)));
      }
      X(inflateSetDictionary(&context->zs_.stream,
            &window[0], WindowSize));
    }
    uint8_t discardBuffer[WindowSize];

    auto numToSkip = abspos - context->uncompressedOffset_;
    while (numToSkip) {
      auto skipNow = std::min(WindowSize, (uInt)numToSkip);
      numToSkip -= skipNow;
      auto numRead = gzread(*context, discardBuffer, skipNow);
      if (numRead != skipNow) {
        THROW_RT("Unable to read expected number of skipped bytes from gzipped"
                 << " stream. Wanted " << skipNow << ", got " << numRead);
      }
    }
    // we're at the right point, save the context for next time.
    cachedContext_ = std::move(context);
  }

  size_t gzread(CachedContext &context, uint8_t *outBuf, size_t len) {
    if (context.eof_) return 0;

    auto &zs = context.zs_;
    zs.stream.next_out = outBuf;
    zs.stream.avail_out = len;
    size_t total = 0;
    do {
      if (zs.stream.avail_in == 0) {
        zs.stream.avail_in = ::fread(context.input_, 1,
            sizeof(context.input_),
            compressed_.get());
        if (ferror(compressed_.get())) throw ZlibError(Z_ERRNO);
        zs.stream.next_in = context.input_;
      }
      auto availBefore = zs.stream.avail_out;
      auto ret = inflate(&zs.stream, Z_NO_FLUSH);
      if (ret == Z_NEED_DICT) throw ZlibError(Z_DATA_ERROR);
      if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
        throw ZlibError(ret);
      auto numUncompressed = availBefore - zs.stream.avail_out;
      context.uncompressedOffset_ += numUncompressed;
      total += numUncompressed;
      if (ret == Z_STREAM_END) {
        // this is the end of the first gzip block. we don't currently
        // support multiple blocks... it's hard for us to detect here whether
        // there are multiple blocks or not, because in the presence of the
        // gzip framing data, the underlying file might not actually be at
        // eof. so we don't bother to detect or warn. they will have gotten
        // the warning when the built th index, at least.
        context.eof_ = true;
        break;
      }
    } while (zs.stream.avail_out);
    return total;
  }
};

ZipByteSource::ZipByteSource(const std::string &fname)
: FileByteSource(fname, false), impl_(std::make_unique<Impl>(fname)) {}

ZipByteSource::~ZipByteSource() {}

size_t ZipByteSource::doRead(char *buf, size_t len) {
  return impl_->doRead(buf, len);
}

size_t ZipByteSource::endPos() const {
  return impl_->endPos();
}

void ZipByteSource::doSeek(size_t abspos) {
  return impl_->doSeek(abspos);
}
