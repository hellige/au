#pragma once

#include "Zindex.h"
#include "au/FileByteSource.h"

namespace au {

namespace {

static inline bool isAuFile(AuByteSource &source) {
  auto headerMatched = false;
  auto pos = source.pos();
  try {
    source.readFunc(4, [&](auto fragment) {
      if (fragment == "HAU\x61") {
        headerMatched = true;
      }
    });
  } catch (parse_error &) {}
  source.seek(pos);
  return headerMatched;
}

static inline bool isGzipFile(AuByteSource &source) {
  auto magicMatched = false;
  auto pos = source.pos();
  try {
    source.readFunc(2, [&](auto fragment) {
      if (fragment == "\x1f\x8b") {
        magicMatched = true;
      }
    });
  } catch (parse_error &) {}
  source.seek(pos);
  return magicMatched;
}

static inline std::unique_ptr<FileByteSource> detectSource(
    const std::string &fileName,
    const std::optional<std::string> &indexFile,
    bool compressed) {
  std::unique_ptr<FileByteSource> source;
  auto fbs = std::make_unique<FileByteSourceImpl>(fileName);
  if (compressed || isGzipFile(*fbs)) {
    auto *ptr = fbs.get();
    source.reset(new ZipByteSource(*ptr, indexFile));
  } else {
    source = std::move(fbs);
  }
  return source;
}

static inline bool checkAuFile(AuByteSource &source) {
  if (isAuFile(source)) return true;
  std::cerr << source.name() << " does not appear to be an au-encoded file"
    " (gzipped or otherwise)\n";
  return false;
}

}

}
