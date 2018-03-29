#include "au/AuEncoder.h"
#include "au/ParseError.h"
#include "TclapHelper.h"
#include "TimestampPattern.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/reader.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <string>
#include <string.h>

using namespace rapidjson;

namespace {

class JsonSaxHandler
    : public BaseReaderHandler<UTF8<>, JsonSaxHandler> {
  AuWriter &writer_;
  std::optional<bool> intern_;
  bool toInt_;
  size_t &timeConversionAttempts_, &timeConversionFailures_;

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
      writer_.value(i);
    } else {
      uint64_t u = strtoull(digits, &endptr, 10);
      if (endptr - digits != length) return false;
      writer_.value(u);
    }
    return true;
  }

  bool tryTime(const char *str, SizeType length) {
    using namespace std::chrono;
    timeConversionAttempts_++;
    auto result = parseTimestampPattern(std::string_view(str, length));
    if (result) {
      writer_.value(result->first);
      return true;
    } else {
      timeConversionFailures_++;
      return false;
    }
  }

public:
  explicit JsonSaxHandler(AuWriter &writer,
                          size_t &timeConversionAttempts,
                          size_t &timeConversionFailures)
      : writer_(writer), toInt_(false),
        timeConversionAttempts_(timeConversionAttempts),
        timeConversionFailures_(timeConversionFailures)
  {}

  bool Null() { writer_.null(); return true; }
  bool Bool(bool b) { writer_.value(b); return true; }
  bool Int(int i) { writer_.value(i); return true; }
  bool Uint(unsigned u) { writer_.value(u); return true; }
  bool Int64(int64_t i) { writer_.value(i); return true; }
  bool Uint64(uint64_t u) { writer_.value(u); return true; }
  bool Double(double d) { writer_.value(d); return true; }

  bool String(const char *str, SizeType length, [[maybe_unused]] bool copy) {
    constexpr size_t MAX_TIMESTAMP_LEN =
        sizeof("yyyy-mm-ddThh:mm:ss.mmmuuunnn") - 1;
    if (toInt_) {
      toInt_ = false;
      if (tryInt(str, length)) return true;
    } else if (length == MAX_TIMESTAMP_LEN
               || length == MAX_TIMESTAMP_LEN - 3
               || length == MAX_TIMESTAMP_LEN - 6
               || length == MAX_TIMESTAMP_LEN - 10) {
      // try times with ms, us, ns or just seconds...
      if (tryTime(str, length)) return true;
    }
    writer_.value(std::string_view(str, length), intern_);
    intern_.reset();
    return true;
  }

  bool StartObject() {
    writer_.startMap();
    return true;
  }

  bool Key(const char *str, SizeType length, [[maybe_unused]] bool copy) {
    writer_.value(std::string_view(str, length), true);
    if (0 == strncmp(str, "estdEventTime", length) ||
        0 == strncmp(str, "logTime", length) ||
        0 == strncmp(str, "execId", length) ||
        0 == strncmp(str, "px", length)) {
      intern_ = false;
    } else if (0 == strncmp(str, "key", length) ||
               0 == strncmp(str, "signed", length) ||
               0 == strncmp(str, "origFfeKey", length)) {
      //toInt_ = true; // This would make the diff fail
      intern_ = false;
    }
    return true;
  }

  bool EndObject([[maybe_unused]] SizeType memberCount) {
    writer_.endMap();
    return true;
  }

  bool StartArray() {
    writer_.startArray();
    return true;
  }

  bool EndArray([[maybe_unused]] SizeType elementCount) {
    writer_.endArray();
    return true;
  }
};

ssize_t encodeFile(const std::string &inFName,
                   std::ostream &out,
                   size_t maxEntries,
                   bool quiet) {
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

  auto metadata = STR("Encoded from json file "
                          << (inFName == "-" ? "<stdin>" : inFName )
                          << " by au");
  AuEncoder au(out, metadata, 250'000, 100);

  char readBuffer[65536];
  FileReadStream in(inF, readBuffer, sizeof(readBuffer));

  Reader reader;
  ParseResult res;
  size_t entriesProcessed = 0;
  size_t timeConversionAttempts = 0, timeConversionFailures = 0;
  auto lastTime = std::chrono::steady_clock::now();
  int lastDictSize = 0;
  while (res) {
    au.encode([&](auto &f) {
      JsonSaxHandler handler(f, timeConversionAttempts, timeConversionFailures);
      static constexpr auto parseOpt = kParseStopWhenDoneFlag +
                                       kParseFullPrecisionFlag +
                                       kParseNanAndInfFlag;
      res = reader.Parse<parseOpt>(in, handler);
    });

    entriesProcessed++;
    if (!quiet && entriesProcessed % 10'000 == 0) {
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

    if (entriesProcessed >= maxEntries) break;
  }
  if (!quiet && timeConversionAttempts) {
    std::cerr << "Time conversion attempts: " << timeConversionAttempts
              << " failures: " << timeConversionFailures << " ("
              << (100 * timeConversionFailures / timeConversionAttempts)
              << "%)\n";
  }

  fclose(inF);

  if (res.Code() == kParseErrorNone || res.Code() == kParseErrorDocumentEmpty) {
    return entriesProcessed;
  } else {
    std::cerr << "json parse error at "
              << (inFName == "-" ? "stdin" : inFName)
              << ":" << res.Offset() << ": "
              << rapidjson::GetParseError_En(res.Code()) << std::endl;
    return -1;
  }
}

void usage() {
  std::cout
    << "usage: au enc [options] [--] [<path>...]\n"
    << "\n"
    << " Encodes json to au. Reads stdin if no files specified. Writes to\n"
    << " stdout unless -o is specified. Any <path> may be \"-\" for stdin.\n"
    << "\n"
    << "  -h --help           show usage and exit\n"
    << "  -o --output <path>  output to file\n"
    << "  -q --quiet          do not print encoding statistics to stderr\n"
    << "  -c --count <count>  stop after encoding <count> records.\n";
}

} // namespace

int json2au(int argc, const char * const *argv) {
  TclapHelper tclap(usage);

  TCLAP::ValueArg<std::string> outfile(
      "o", "output", "output", false, "-", "string", tclap.cmd());
  TCLAP::ValueArg<size_t> count(
      "c", "count", "count", false, std::numeric_limits<size_t>::max(),
      "size_t", tclap.cmd());
  TCLAP::SwitchArg quiet("q", "quiet", "quiet", tclap.cmd(), false);
  TCLAP::UnlabeledMultiArg<std::string> fileNames(
      "fileNames", "", false, "filename", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  auto maxEntries = count.getValue();
  auto outFName = outfile.getValue();

  std::vector<std::string> inputFiles{"-"};
  if (fileNames.isSet()) inputFiles = fileNames.getValue();


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

  for (const auto &f : inputFiles) {
    auto result = encodeFile(f, out, maxEntries, quiet.isSet());
    if (result == -1) break;
    maxEntries -= result;
  }

  // no need to explicitly close outFileStream
  return 0;
}

