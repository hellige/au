#include "AuEncoder.h"

#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/reader.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdio.h>
#include <string>
#include <string.h>

using namespace rapidjson;

namespace {

class JsonSaxHandler
    : public BaseReaderHandler<UTF8<>, JsonSaxHandler> {
  AuFormatter &formatter;
  std::optional<bool> intern;
  bool toInt;
  const std::regex &timeRegExp;
  size_t &timeConversionAttempts, &timeConversionFailures;

  bool tryInt(const char *str, SizeType length) {
    // 3 extra bytes for: '-', \0 and 1 extra digit (we have no max_digits10)
    constexpr int maxBuffer = std::numeric_limits<uint64_t>::digits10 + 3;

    if (length == 0 || length > maxBuffer - 1) return false;

    // Null-terminate string for strtoull.
    char digits[maxBuffer];
    memcpy(digits, str, length);
    digits[length] = 0;

    char *endptr;
    if (str[0] == '-') {
      uint64_t u = strtoull(digits + 1, &endptr, 10);
      if (endptr - digits != length) return false;
      int64_t i = static_cast<int64_t>(u) * -1;
      formatter.value(i);
    } else {
      uint64_t u = strtoull(digits, &endptr, 10);
      if (endptr - digits != length) return false;
      formatter.value(u);
    }
    return true;
  }

  bool tryTime(const char *str, SizeType length) {
    using namespace std::chrono;

    timeConversionAttempts++;

    std::cmatch m;
    if (std::regex_match(str, str + length, m, timeRegExp)) {
      std::tm tm;
      memset(&tm, 0, sizeof(tm));

      // Note: tm_year is relative to 1900!
      tm.tm_year  = static_cast<int>(strtol(m.str(1).c_str(), nullptr, 10))
                    - 1900;
      tm.tm_mon   = static_cast<int>(strtol(m.str(2).c_str(), nullptr, 10)) - 1;
      tm.tm_mday  = static_cast<int>(strtol(m.str(3).c_str(), nullptr, 10));
      tm.tm_hour  = static_cast<int>(strtol(m.str(4).c_str(), nullptr, 10));
      tm.tm_min   = static_cast<int>(strtol(m.str(5).c_str(), nullptr, 10));
      tm.tm_sec   = static_cast<int>(strtol(m.str(6).c_str(), nullptr, 10));
      auto mics = strtol(m.str(7).c_str(), nullptr, 10);

      std::time_t tt = timegm(&tm);
      if (tt == -1) return false;

      formatter.value(seconds(tt) + microseconds(mics));

      return true;
    } else {
      timeConversionFailures++;
      return false;
    }
  }

public:
  explicit JsonSaxHandler(AuFormatter &formatter,
                          const std::regex &timeRegExp,
                          size_t &timeConversionAttempts,
                          size_t &timeConversionFailures)
      : formatter(formatter), toInt(false), timeRegExp(timeRegExp),
        timeConversionAttempts(timeConversionAttempts),
        timeConversionFailures(timeConversionFailures)
  {}

  bool Null() { formatter.null(); return true; }
  bool Bool(bool b) { formatter.value(b); return true; }
  bool Int(int i) { formatter.value(i); return true; }
  bool Uint(unsigned u) { formatter.value(u); return true; }
  bool Int64(int64_t i) { formatter.value(i); return true; }
  bool Uint64(uint64_t u) { formatter.value(u); return true; }
  bool Double(double d) { formatter.value(d); return true; }

  bool String(const char *str, SizeType length, [[maybe_unused]] bool copy) {
    if (toInt) {
      toInt = false;
      if (tryInt(str, length)) return true;
    } else if (length == sizeof("yyyy-mm-ddThh:mm:ss.mmmuuu") - 1) {
      if (tryTime(str, length)) return true;
    }
    formatter.value(std::string_view(str, length), intern);
    intern.reset();
    return true;
  }

  bool StartObject() {
    formatter.startMap();
    return true;
  }

  bool Key(const char *str, SizeType length, [[maybe_unused]] bool copy) {
    formatter.value(std::string_view(str, length), true);
    if (0 == strncmp(str, "estdEventTime", length) ||
        0 == strncmp(str, "logTime", length) ||
        0 == strncmp(str, "execId", length) ||
        0 == strncmp(str, "px", length)) {
      intern = false;
    } else if (0 == strncmp(str, "key", length) ||
               0 == strncmp(str, "signed", length) ||
               0 == strncmp(str, "origFfeKey", length)) {
      //toInt = true; // This would make the diff fail
      intern = false;
    }
    return true;
  }

  bool EndObject([[maybe_unused]] SizeType memberCount) {
    formatter.endMap();
    return true;
  }

  bool StartArray() {
    formatter.startArray();
    return true;
  }

  bool EndArray([[maybe_unused]] SizeType elementCount) {
    formatter.endArray();
    return true;
  }
};

} // namespace

int json2au(int argc, const char * const *argv) {
  argc -= 2; argv += 2;

  if (argc < 2 || argc > 3) {
    return -1;
  }

  std::string inFName("-"), outFName("-");
  size_t maxEntries = std::numeric_limits<size_t>::max();

  switch (argc) {
    case 3:
      maxEntries = strtoull(argv[2], nullptr, 0);
      [[fallthrough]];
    case 2:
      outFName = argv[1];
      [[fallthrough]];
    case 1:
      inFName = argv[0];
      [[fallthrough]];
    default:
      break;
  }

  FILE *inF;

  if (inFName == "-") {
    inF = fdopen(fileno(stdin), "rb");
  } else {
    inF = fopen(inFName.c_str(), "rb");
  }
  if (!inF) {
    std::cerr << "Unable to open input " << inFName << std::endl;
    return 1;
  }

  std::streambuf *outBuf;
  std::ofstream outFileStream;
  if (outFName == "-") {
    outBuf = std::cout.rdbuf();
  } else {
    outFileStream.open(outFName, std::ios_base::binary);
    if (!outFileStream) {
      std::cerr << "Unable to open output " << outFName << std::endl;
      return 1;
    }
    outBuf = outFileStream.rdbuf();
  }
  std::ostream out(outBuf);
  AuEncoder au(out, 250'000, 100);

  char readBuffer[65536];
  FileReadStream in(inF, readBuffer, sizeof(readBuffer));

  Reader reader;
  ParseResult res;
  size_t entriesProcessed = 0;
  size_t timeConversionAttempts = 0, timeConversionFailures = 0;
  std::regex timeRegExp(
      R"re(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})\.(\d{6})$)re");
  auto lastTime = std::chrono::steady_clock::now();
  int lastDictSize = 0;
  while (res) {
    au.encode([&](auto &f) {
      JsonSaxHandler handler(f, timeRegExp,
                             timeConversionAttempts, timeConversionFailures);
      static constexpr auto parseOpt = kParseStopWhenDoneFlag +
                                       kParseFullPrecisionFlag +
                                       kParseNanAndInfFlag;
      res = reader.Parse<parseOpt>(in, handler);
    });

    entriesProcessed++;
    if (entriesProcessed % 10'000 == 0) {
      auto stats = au.getStats();
      auto tNow = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                     (tNow - lastTime);
      std::cerr << "Processed: " << stats["Records"] / 1'000 << "k entries in "
                << elapsed.count() << "ms. DictSize: " << stats["DictSize"]
                << " DictDelta: " << stats["DictSize"] - lastDictSize
                << " HashSize: " << stats["HashSize"]
                << " HashBucketCount: " << stats["HashBucketCount"]
                << " CacheSize: " << stats["CacheSize"] << "\n";
      lastTime = tNow;
      lastDictSize = stats["DictSize"];
    }

    if (entriesProcessed > maxEntries) break;
  }
  if (timeConversionAttempts) {
    std::cerr << "Time conversion attempts: " << timeConversionAttempts
              << " failures: " << timeConversionFailures << " ("
              << (100 * timeConversionFailures / timeConversionAttempts)
              << "%)\n";
  }

  fclose(inF);
  outFileStream.close();

  if (res.Code() == kParseErrorNone || res.Code() == kParseErrorDocumentEmpty) {
    return 0;
  } else {
    return res.Code();
  }
}
