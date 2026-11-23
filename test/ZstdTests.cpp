#include "AuRecordHandler.h"
#include "Dictionary.h"
#include "GrepHandler.h"
#include "JsonOutputHandler.h"
#include "StreamDetection.h"
#include "ZstdSeekable.h"
#include "au/AuDecoder.h"
#include "au/AuEncoder.h"
#include "au/BufferByteSource.h"
#include "au/FileByteSource.h"

#include "gtest/gtest.h"
#include <zstd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace au {

namespace {

using namespace std::chrono_literals;

// Base of the synthetic timestamps; each test record is ~1ms after the last.
const time_point T0 = time_point{} + 1'700'000'000s;

/// A temp directory that's removed at the end of the test.
class TempDir {
  fs::path path_;
public:
  TempDir() {
    auto base = fs::temp_directory_path();
    std::random_device rd;
    path_ = base / ("au-zstd-test-" + std::to_string(rd()));
    fs::create_directories(path_);
  }
  ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
  std::string operator/(const std::string &name) const {
    return (path_ / name).string();
  }
};

void writeFile(const std::string &name, std::string_view data) {
  std::ofstream out(name, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string readFile(const std::string &name) {
  std::ifstream in(name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

struct GenOptions {
  size_t records = 5000;
  // fresh strings appear every so often, so the dictionary keeps growing and
  // there are lots of dict-add records interleaved with values
  size_t newStringEvery = 7;
  // force dict clears periodically (via a small clear threshold)
  size_t clearThreshold = 300;
  // every so often, write a record whose timestamp is a bit out of order
  size_t disorderEvery = 0;
  std::chrono::nanoseconds disorderBy = 0ns;
  // every so often, write a very large record
  size_t bigEvery = 0;
  size_t bigSize = 0;
};

/// Generates an au file with an increasing "ts" field, a "seq" field, and a
/// mix of interned and fresh strings.
std::string genAu(const GenOptions &opts) {
  AuStringIntern::Config cfg;
  cfg.internThresh = 2;
  cfg.clearThreshold = opts.clearThreshold;
  AuEncoder enc("zstd test", 1000, 2, 0, cfg);
  std::string out;
  auto write = [&](std::string_view dict, std::string_view val) {
    out.append(dict);
    out.append(val);
    return dict.size() + val.size();
  };
  for (size_t i = 0; i < opts.records; i++) {
    auto ts = T0 + std::chrono::milliseconds(i);
    if (opts.disorderEvery && i % opts.disorderEvery == 0 && i > 0)
      ts -= opts.disorderBy;
    std::string fresh = "fresh-string-" + std::to_string(i / opts.newStringEvery);
    std::string big;
    if (opts.bigEvery && i % opts.bigEvery == 0)
      big.assign(opts.bigSize, static_cast<char>('a' + i % 26));
    enc.encode([&](AuWriter &w) {
      w.startMap();
      w.key("ts"); w.value(ts);
      w.key("seq"); w.value(static_cast<uint64_t>(i));
      w.key("kind"); w.value(i % 3 ? "common-kind-value" : "other-kind-value");
      w.key("fresh"); w.value(fresh);
      if (!big.empty()) { w.key("big"); w.value(big, false); }
      w.endMap();
    }, write);
  }
  return out;
}

/// Decodes an au file (any supported compression) to json lines.
std::string auCat(const std::string &fname) {
  auto source = detectSource(fname, std::nullopt, false);
  std::stringstream ss;
  JsonOutputHandler handler(ss);
  Dictionary dictionary;
  AuRecordHandler recordHandler(dictionary, handler);
  RecordParser(*source, recordHandler).parseStream();
  return ss.str();
}

/// Decodes au bytes to json lines.
std::string auCatBytes(std::string_view bytes) {
  BufferByteSource source(bytes);
  std::stringstream ss;
  JsonOutputHandler handler(ss);
  Dictionary dictionary;
  AuRecordHandler recordHandler(dictionary, handler);
  RecordParser(source, recordHandler).parseStream();
  return ss.str();
}

/// Equivalent of `au grep -o ts -t <ts> -m <n>`, returning the json output.
std::string grepOrdered(const std::string &fname, const std::string &ts,
                        uint32_t matches) {
  Pattern pattern;
  pattern.keyPattern = std::string("ts");
  pattern.bisect = true;
  pattern.timestampPattern = parseFlexPattern(ts);
  EXPECT_TRUE(pattern.timestampPattern.has_value());
  pattern.numMatches = matches;
  auto source = detectSource(fname, std::nullopt, false);
  std::stringstream ss;
  JsonOutputHandler handler(ss);
  AuGrepper(pattern, *source, handler).doGrep();
  return ss.str();
}

/// Formats T0 + offset as a timestamp pattern string.
std::string tsString(std::chrono::nanoseconds offset) {
  auto t = T0 + offset;
  auto secs = std::chrono::time_point_cast<std::chrono::seconds>(t);
  auto nanos = (t - secs).count();
  auto tt = std::chrono::system_clock::to_time_t(secs);
  std::tm tm;
  gmtime_r(&tt, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  char full[80];
  std::snprintf(full, sizeof(full), "%s.%09lld", buf,
                static_cast<long long>(nanos));
  return full;
}

ZstdCompressOptions opts(size_t frameSize, bool au) {
  ZstdCompressOptions o;
  o.frameSize = frameSize;
  o.auFrames = au;
  o.threads = 2;
  return o;
}

/// Returns the frames of a seekable zstd file, each decompressed separately.
std::vector<std::string> frames(const std::string &fname) {
  std::vector<std::string> result;
  auto all = readFile(fname);
  size_t pos = 0;
  while (pos < all.size()) {
    auto csize = ZSTD_findFrameCompressedSize(all.data() + pos,
                                              all.size() - pos);
    if (ZSTD_isError(csize)) ADD_FAILURE() << ZSTD_getErrorName(csize);
    auto dsize = ZSTD_getFrameContentSize(all.data() + pos, csize);
    // skippable frames (the seek table) report 0
    if (dsize != 0 && dsize != ZSTD_CONTENTSIZE_ERROR
        && dsize != ZSTD_CONTENTSIZE_UNKNOWN) {
      std::string frame(dsize, '\0');
      auto n = ZSTD_decompress(frame.data(), dsize, all.data() + pos, csize);
      EXPECT_EQ(n, dsize);
      result.push_back(std::move(frame));
    }
    pos += csize;
  }
  return result;
}

}

////////////////////////////////////////////////////////////////////////////////
// Writer

TEST(ZstdTest, PlainRoundTripIsByteExact) {
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(4096, false));
  auto fs = frames(tmp / "out.zst");
  EXPECT_GT(fs.size(), 10u);
  std::string joined;
  for (auto &f : fs) joined += f;
  EXPECT_EQ(joined, au);
}

TEST(ZstdTest, PlainIsDeterministicAcrossThreadCounts) {
  TempDir tmp;
  writeFile(tmp / "in.au", genAu({}));
  auto o1 = opts(4096, false);
  o1.threads = 1;
  auto o8 = opts(4096, false);
  o8.threads = 8;
  zstdCompressFile(tmp / "in.au", tmp / "1.zst", o1);
  zstdCompressFile(tmp / "in.au", tmp / "8.zst", o8);
  EXPECT_EQ(readFile(tmp / "1.zst"), readFile(tmp / "8.zst"));
}

TEST(ZstdTest, AuFramesDecodeToSameRecords) {
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(4096, true));
  EXPECT_EQ(auCat(tmp / "out.zst"), auCatBytes(au));
}

TEST(ZstdTest, AuFramesAreIndependentlyDecodable) {
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(4096, true));
  auto fs = frames(tmp / "out.zst");
  ASSERT_GT(fs.size(), 10u);

  // every frame after the first starts with a dict clear, and decodes on its
  // own (prefixed with just a header). together they give the full output.
  auto header = fs[0].substr(0, fs[0].find("\x0f\n") + 2);
  ASSERT_EQ(header.substr(0, 3), "HAU");
  std::string joined = auCatBytes(fs[0]);
  for (size_t i = 1; i < fs.size(); i++) {
    SCOPED_TRACE("frame " + std::to_string(i));
    EXPECT_EQ(fs[i][0], 'C');
    joined += auCatBytes(header + fs[i]);
  }
  EXPECT_EQ(joined, auCatBytes(au));
}

TEST(ZstdTest, AuFramesWithManyClears) {
  // clear threshold small enough that clears happen within most frames
  TempDir tmp;
  GenOptions g;
  g.clearThreshold = 20;
  g.newStringEvery = 2;
  auto au = genAu(g);
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(2048, true));
  EXPECT_EQ(auCat(tmp / "out.zst"), auCatBytes(au));
}

TEST(ZstdTest, AuFramesWithRecordsLargerThanFrames) {
  TempDir tmp;
  GenOptions g;
  g.records = 500;
  g.bigEvery = 37;
  g.bigSize = 20000;
  auto au = genAu(g);
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(4096, true));
  EXPECT_EQ(auCat(tmp / "out.zst"), auCatBytes(au));
}

TEST(ZstdTest, AuFramesTruncatedInput) {
  TempDir tmp;
  auto au = genAu({});
  // cut partway through the last record
  auto truncated = au.substr(0, au.size() - 5);
  writeFile(tmp / "in.au", truncated);
  testing::internal::CaptureStderr();
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(4096, true));
  auto err = testing::internal::GetCapturedStderr();
  EXPECT_NE(err.find("partial au record"), std::string::npos);
  // all complete records survive, and the trailing partial record is kept
  // as-is (so decoding fails in the same place as the original)
  auto fs = frames(tmp / "out.zst");
  EXPECT_EQ(fs.back().substr(fs.back().size() - 10),
            truncated.substr(truncated.size() - 10));
}

TEST(ZstdTest, AuFramesEmptyInput) {
  TempDir tmp;
  writeFile(tmp / "in.au", "");
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(4096, true));
  EXPECT_EQ(auCat(tmp / "out.zst"), "");
}

TEST(ZstdTest, AuFramesRejectsNonAu) {
  TempDir tmp;
  writeFile(tmp / "in.json", "{\"a\": 1}\n");
  EXPECT_THROW(
      zstdCompressFile(tmp / "in.json", tmp / "out.zst", opts(4096, true)),
      std::runtime_error);
}

////////////////////////////////////////////////////////////////////////////////
// Reader

TEST(ZstdTest, RandomSeeksMatchRawFile) {
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(3000, false));

  auto source = detectSource(tmp / "out.zst", std::nullopt, false);
  ASSERT_TRUE(source->isSeekable());
  ASSERT_EQ(source->endPos(), au.size());

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, au.size() - 1);
  std::vector<size_t> offsets;
  for (int i = 0; i < 300; i++) offsets.push_back(dist(rng));
  // around frame boundaries in particular
  for (size_t b = 3000; b < au.size(); b += 3000)
    for (size_t d : {b - 1, b, b + 1}) offsets.push_back(d);
  for (auto off : offsets) {
    SCOPED_TRACE("offset " + std::to_string(off));
    source->seek(off);
    auto len = std::min<size_t>(5000, au.size() - off);
    std::string got;
    source->readFunc(len, [&](std::string_view f) { got.append(f); });
    ASSERT_EQ(got, au.substr(off, len));
  }
}

TEST(ZstdTest, ShortBackwardSeeks) {
  // exercises the backward buffer refill, including across frame boundaries
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(3000, false));
  for (auto name : {"in.au", "out.zst"}) {
    SCOPED_TRACE(name);
    auto source = detectSource(tmp / name, std::nullopt, false);
    size_t pos = au.size() - 10;
    while (pos > 1000) {
      source->seek(pos);
      std::string got;
      source->readFunc(10, [&](std::string_view f) { got.append(f); });
      ASSERT_EQ(got, au.substr(pos, 10)) << "at " << pos;
      pos -= 977;
    }
  }
}

TEST(ZstdTest, ChunkBoundaries) {
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(3000, false));
  auto source = detectSource(tmp / "out.zst", std::nullopt, false);
  EXPECT_EQ(source->chunkBoundaryWithin(0, au.size()).value() % 3000, 0u);
  EXPECT_EQ(source->chunkBoundaryWithin(3000, 6000), std::nullopt);
  EXPECT_EQ(source->chunkBoundaryWithin(2999, 6000), 3000u);
  EXPECT_EQ(source->chunkBoundaryWithin(3000, 6001), 6000u);
  EXPECT_EQ(source->chunkBoundaryWithin(0, 20000), 9000u);
  EXPECT_EQ(source->chunkStartFor(4500), 3000u);
  EXPECT_EQ(source->chunkStartFor(3000), 3000u);

  // raw files have no chunks
  auto raw = detectSource(tmp / "in.au", std::nullopt, false);
  EXPECT_EQ(raw->chunkBoundaryWithin(0, au.size()), std::nullopt);
}

TEST(ZstdTest, NonSeekableStreams) {
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  // a plain single-frame zstd file, and back-to-back frames with no seek table
  std::string single(ZSTD_compressBound(au.size()), '\0');
  single.resize(ZSTD_compress(single.data(), single.size(), au.data(),
                              au.size(), 3));
  writeFile(tmp / "single.zst", single);
  auto half = au.size() / 2;
  std::string a(ZSTD_compressBound(half), '\0');
  a.resize(ZSTD_compress(a.data(), a.size(), au.data(), half, 3));
  std::string b(ZSTD_compressBound(au.size() - half), '\0');
  b.resize(ZSTD_compress(b.data(), b.size(), au.data() + half,
                         au.size() - half, 3));
  writeFile(tmp / "multi.zst", a + b);

  for (auto name : {"single.zst", "multi.zst"}) {
    SCOPED_TRACE(name);
    auto source = detectSource(tmp / name, std::nullopt, false);
    EXPECT_FALSE(source->isSeekable());
    EXPECT_EQ(auCat(tmp / name), auCatBytes(au));
  }
}

TEST(ZstdTest, TruncatedAndCorruptFiles) {
  TempDir tmp;
  auto au = genAu({});
  writeFile(tmp / "in.au", au);
  zstdCompressFile(tmp / "in.au", tmp / "out.zst", opts(4096, false));
  auto good = readFile(tmp / "out.zst");

  // truncated: the seek table is gone, so we stream, and fail at the end
  writeFile(tmp / "trunc.zst", good.substr(0, good.size() / 2));
  EXPECT_THROW(auCat(tmp / "trunc.zst"), std::runtime_error);

  // corrupt frame data: checksum failure
  auto bad = good;
  bad[good.size() / 3] = static_cast<char>(bad[good.size() / 3] ^ 0xff);
  writeFile(tmp / "bad.zst", bad);
  EXPECT_THROW(auCat(tmp / "bad.zst"), std::runtime_error);
}

TEST(ZstdTest, ConcatenatedSeekableFilesFallBackToStreaming) {
  // concatenating seekable files is valid zstd, but the seek table at the end
  // only describes the last file. we must notice and read as a stream rather
  // than seeking to the wrong places.
  TempDir tmp;
  GenOptions g;
  g.records = 2000;
  auto a = genAu(g);
  auto b = genAu(g);
  writeFile(tmp / "a.au", a);
  writeFile(tmp / "b.au", b);
  for (bool au : {false, true}) {
    SCOPED_TRACE(au ? "--au" : "plain");
    zstdCompressFile(tmp / "a.au", tmp / "a.zst", opts(4096, au));
    zstdCompressFile(tmp / "b.au", tmp / "b.zst", opts(4096, au));
    writeFile(tmp / "ab.zst", readFile(tmp / "a.zst") + readFile(tmp / "b.zst"));

    testing::internal::CaptureStderr();
    auto source = detectSource(tmp / "ab.zst", std::nullopt, false);
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(source->isSeekable());
    EXPECT_NE(err.find("doesn't describe the whole file"), std::string::npos);

    testing::internal::CaptureStderr();
    auto decoded = auCat(tmp / "ab.zst");
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(decoded, auCatBytes(a) + auCatBytes(b));

    // each part on its own is still seekable
    EXPECT_TRUE(detectSource(tmp / "b.zst", std::nullopt, false)->isSeekable());
  }
}

////////////////////////////////////////////////////////////////////////////////
// Binary search

class ZstdGrepTest : public testing::Test {
protected:
  TempDir tmp;

  /// Writes the raw au plus several compressed variants, and checks that
  /// grep -o gives identical results for each, for many timestamps.
  void checkAll(const std::string &au, size_t records,
                std::vector<size_t> frameSizes = {2048, 16384},
                int randomTargets = 6) {
    auto step = 1ms;
    writeFile(tmp / "in.au", au);
    std::vector<std::string> files = {"in.au"};
    for (size_t fs : frameSizes) {
      for (bool auFrames : {false, true}) {
        auto name = std::to_string(fs) + (auFrames ? "-au" : "") + ".zst";
        zstdCompressFile(tmp / "in.au", tmp / name, opts(fs, auFrames));
        files.push_back(name);
      }
    }

    std::vector<std::chrono::nanoseconds> targets = {
        -1s, 0ns, step, step * records / 2, step * (records - 1),
        step * records + 1s};
    std::mt19937 rng(7);
    std::uniform_int_distribution<long long> dist(
        0, static_cast<long long>(records));
    for (int i = 0; i < randomTargets; i++) targets.push_back(step * dist(rng));

    for (auto t : targets) {
      auto ts = tsString(t);
      SCOPED_TRACE(ts);
      auto expected = grepOrdered(tmp / "in.au", ts, 5);
      for (auto &f : files) {
        SCOPED_TRACE(f);
        EXPECT_EQ(grepOrdered(tmp / f, ts, 5), expected);
      }
    }
  }
};

TEST_F(ZstdGrepTest, OrderedRecords) {
  checkAll(genAu({}), 5000);
}

TEST_F(ZstdGrepTest, RoughlyOrderedRecords) {
  // every 50th record is 30ms early: still within the scan-back window
  GenOptions g;
  g.disorderEvery = 50;
  g.disorderBy = 30ms;
  checkAll(genAu(g), 5000);
}

TEST_F(ZstdGrepTest, RecordsLargerThanFrames) {
  GenOptions g;
  g.records = 2000;
  g.bigEvery = 41;
  g.bigSize = 70000;
  checkAll(genAu(g), 2000);
}

TEST_F(ZstdGrepTest, FramesLargerThanFinalScanWindow) {
  // the final linear scan gives up after ~1.25MB without a match, so if
  // bisection over chunk boundaries ever bails out to a linear scan from too
  // far back, we'd miss matches. that only shows up when frames are bigger
  // than the scan window, e.g., when bisection probes a boundary just before
  // the current upper bound and finds the same first record again.
  GenOptions g;
  g.records = 250'000;
  auto au = genAu(g);
  ASSERT_GT(au.size(), 10'000'000u);
  checkAll(au, g.records, {3u << 20}, 8);
}

TEST_F(ZstdGrepTest, ManyDictionaryClears) {
  GenOptions g;
  g.clearThreshold = 20;
  g.newStringEvery = 2;
  checkAll(genAu(g), 5000);
}

}
