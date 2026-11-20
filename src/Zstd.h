#pragma once

#include "au/FileByteSource.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace au {

/// Magic number at the start of every (non-skippable) zstd frame.
inline constexpr std::string_view ZSTD_MAGIC = "\x28\xb5\x2f\xfd";

/// True if the 4-byte prefix is a zstd frame or a zstd skippable frame
/// (0x184D2A50..0x184D2A5F, little-endian).
inline bool looksLikeZstd(std::string_view prefix) {
  if (prefix.size() < 4) return false;
  if (prefix.substr(0, 4) == ZSTD_MAGIC) return true;
  return (static_cast<uint8_t>(prefix[0]) & 0xf0) == 0x50
      && prefix.substr(1, 3) == "\x2a\x4d\x18";
}

/// Reads zstd-compressed data. If the file is in the zstd seekable format
/// (https://github.com/facebook/zstd/blob/dev/contrib/seekable_format/), the
/// source is seekable, using the seek table embedded at the end of the file.
/// Otherwise (including on stdin) it's a plain streaming decompressor that
/// handles any valid zstd stream, including multiple concatenated frames.
class ZstdByteSource : public FileByteSource {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  /// Takes ownership of the underlying FILE from `source`, along with any data
  /// it has already buffered.
  explicit ZstdByteSource(FileByteSourceImpl &source);
  ~ZstdByteSource() override;

  bool isSeekable() const override;
  size_t endPos() const override;

private:
  size_t doRead(char *buf, size_t len) override;
  void doSeek(size_t abspos) override;
};

struct ZstdCompressOptions {
  int level = 3;
  unsigned threads = 1;
  size_t frameSize = 16 * 1024 * 1024;
  bool checksum = true; // per-frame content checksums
  int longWindowLog = 0; // if non-zero, enable long distance matching
  bool verbose = false;
};

/// Compresses `inFile` ("-" for stdin) to `outFile` ("-" for stdout) in the
/// zstd seekable format. Frames are compressed independently, in parallel if
/// opts.threads > 1. The output is a valid zstd stream readable by any zstd
/// decompressor.
int zstdCompressFile(const std::string &inFile, const std::string &outFile,
                     const ZstdCompressOptions &opts);

}
