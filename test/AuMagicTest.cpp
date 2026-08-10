#include "AuMagic.h"
#include "au/AuDecoder.h"
#include "au/BufferByteSource.h"
#include "au/Handlers.h"

#include <gmock/gmock.h>

#include <string>
#include <string_view>

namespace au {

/// magic, smallint version 1, empty metadata string, record terminator
constexpr std::string_view V1_HEADER = "HAU\x61\x20\x0f\n";

static std::string headerWithVersionByte(char version) {
  std::string s(V1_HEADER);
  s[3] = version;
  return s;
}

TEST(AuMagic, AcceptsSupportedVersion) {
  EXPECT_TRUE(looksLikeAuHeader("HAU\x61"));
}

TEST(AuMagic, AcceptsVersionsWeDoNotSupport) {
  EXPECT_TRUE(looksLikeAuHeader("HAU\x62"));   // version 2
  EXPECT_TRUE(looksLikeAuHeader("HAU\x60"));   // version 0
  EXPECT_TRUE(looksLikeAuHeader("HAU\x7f"));   // version 31, largest smallint
  EXPECT_TRUE(looksLikeAuHeader("HAU\x06"));   // varint-encoded version
}

TEST(AuMagic, IgnoresTrailingBytes) {
  EXPECT_TRUE(looksLikeAuHeader(V1_HEADER));
  EXPECT_TRUE(looksLikeAuHeader("HAU\x61 and then some other stuff"));
}

TEST(AuMagic, RejectsShortInput) {
  EXPECT_FALSE(looksLikeAuHeader(""));
  EXPECT_FALSE(looksLikeAuHeader("H"));
  EXPECT_FALSE(looksLikeAuHeader("HA"));
  EXPECT_FALSE(looksLikeAuHeader("HAU"));  // magic but no version byte
}

TEST(AuMagic, RejectsWrongMagic) {
  EXPECT_FALSE(looksLikeAuHeader("hau\x61"));
  EXPECT_FALSE(looksLikeAuHeader("XAU\x61"));
  EXPECT_FALSE(looksLikeAuHeader("HAX\x61"));
  EXPECT_FALSE(looksLikeAuHeader("{\"a\":1}"));
}

TEST(AuMagic, RejectsImplausibleVersionByte) {
  EXPECT_FALSE(looksLikeAuHeader(std::string_view("HAU\x00", 4)));
  EXPECT_FALSE(looksLikeAuHeader("HAUZ"));    // 0x5a, a small *negative* int
  EXPECT_FALSE(looksLikeAuHeader("HAU\x05")); // string marker, not a version
  EXPECT_FALSE(looksLikeAuHeader("HAU\x80")); // dict ref, and high bit set
  EXPECT_FALSE(looksLikeAuHeader("HAU\xff"));
}

TEST(AuMagicSource, DetectsHeaderAndRestoresPosition) {
  BufferByteSource source(V1_HEADER);
  EXPECT_TRUE(isAuFile(source));
  EXPECT_EQ(0u, source.pos()) << "isAuFile must not consume the header";
  // and it must be repeatable
  EXPECT_TRUE(isAuFile(source));
  EXPECT_EQ(0u, source.pos());
}

TEST(AuMagicSource, DetectsUnsupportedVersion) {
  auto v2 = headerWithVersionByte('\x62');
  BufferByteSource source(v2);
  EXPECT_TRUE(isAuFile(source));
}

TEST(AuMagicSource, RejectsJson) {
  std::string_view json = "{\"a\":1,\"b\":2.5}\n";
  BufferByteSource source(json);
  EXPECT_FALSE(isAuFile(source));
  EXPECT_EQ(0u, source.pos());
}

TEST(AuMagicSource, RejectsSourceTooShortToHoldAHeader) {
  for (size_t len = 1; len < AU_MAGIC_PREFIX_LEN; ++len) {
    std::string truncated(V1_HEADER.substr(0, len));
    BufferByteSource source(truncated);
    EXPECT_FALSE(isAuFile(source)) << "for length " << len;
  }
}

TEST(AuMagicSource, RejectsEmptySource) {
  BufferByteSource source(std::string_view{});
  EXPECT_FALSE(isAuFile(source));
}

TEST(AuMagicSource, ReportsUnsupportedVersionRatherThanDenyingItIsAuAtAll) {
  auto v2 = headerWithVersionByte('\x62');
  BufferByteSource source(v2);
  NoopRecordHandler handler;
  try {
    RecordParser(source, handler).parseStream();
    FAIL() << "expected an unsupported version to be rejected";
  } catch (const bad_version &e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("got 2"));
  }
}

TEST(AuMagicSource, StillComplainsGenericallyAboutNonAuInput) {
  std::string_view json = "{\"a\":1}\n";
  BufferByteSource source(json);
  NoopRecordHandler handler;
  try {
    RecordParser(source, handler).parseStream();
    FAIL() << "expected non-au input to be rejected";
  } catch (const bad_version &) {
    FAIL() << "non-au input should not be reported as a version problem";
  } catch (const parse_error &e) {
    EXPECT_NE(std::string::npos,
              std::string(e.what()).find("doesn't appear to start with an au"));
  }
}

}
