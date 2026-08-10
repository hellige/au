#pragma once

#include "au/AuByteSource.h"
#include "au/AuCommon.h"
#include "au/ParseError.h"

#include <cstdint>
#include <string_view>

namespace au {

namespace {

static constexpr std::string_view AU_MAGIC = "HAU";

/// Magic bytes plus the first byte of the encoded format version.
static constexpr size_t AU_MAGIC_PREFIX_LEN = AU_MAGIC.size() + 1;

/** The version is written with AuWriter::value(), so it's either a "small int"
 * (versions 0-31) or a varint marker followed by a varint. Mirrors the
 * encodings accepted by BaseParser::parseFormatVersion(). */
static inline bool looksLikeFormatVersion(char c) {
  auto u = static_cast<uint8_t>(c);
  return (u & ~0x1fu) == marker::SmallInt::Positive
      || u == static_cast<uint8_t>(marker::Varint);
}

/** Accepts *any* format version, including ones this build can't read.
 * Deciding whether a file is au and deciding whether we can decode it are
 * separate questions: an au file of an unsupported version should be rejected
 * by parseFormatVersion(), which can say so, rather than being misdetected as
 * json here. */
static inline bool looksLikeAuHeader(std::string_view buf) {
  return buf.size() >= AU_MAGIC_PREFIX_LEN
      && buf.substr(0, AU_MAGIC.size()) == AU_MAGIC
      && looksLikeFormatVersion(buf[AU_MAGIC.size()]);
}

/// Leaves the source positioned where it found it.
static inline bool isAuFile(AuByteSource &source) {
  if (source.peek().isEof()) return false;

  char prefix[AU_MAGIC_PREFIX_LEN] = {};
  size_t len = 0;
  auto pos = source.pos();
  try {
    source.readFunc(sizeof(prefix), [&](std::string_view fragment) {
      // readFunc may deliver the bytes in several fragments
      for (auto c : fragment)
        if (len < sizeof(prefix)) prefix[len++] = c;
    });
  } catch (parse_error &) {
    // too short to be a header, but we still need to restore the position
  }
  source.seek(pos);

  return looksLikeAuHeader(std::string_view(prefix, len));
}

}

}
