#include "AuEncoder.h"
#include "AuDecoder.h"

#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/reader.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;
using namespace rapidjson;

namespace {

constexpr const char *AU_VERSION = "0.1";

constexpr int AU_FORMAT_VERSION = 1; // TODO extract

int version(int, char **) {
  std::cout << "au version " << AU_VERSION
    << " (format version " << AU_FORMAT_VERSION << ")" << std::endl;
  return 0;
}

int usage(std::ostream &os) {
  os << "usage: au [--version] [--help] <command> [args]" << std::endl;
  return 1;
}

int help(int, char **) {
  usage(std::cout);
  std::cout << "\nCommands:\n"
    << "   cat      Decode listed files to stdout\n"
    << "   tail     Decode and/or follow file\n"
    << "   grep     Find records matching pattern\n"
    << "   enc      Encode listed files to stdout\n"
    << "   json2au  <json_file> <au_file> [count]\n"
    << "            Encode json to au (either file can be '-')\n"
    << "            Optionally stops after count lines have been encoded\n"
    << "   stats    Display file statistics\n";
  return 0;
}

int cat(int argc, char **argv) {
  if (argc == 0) {
    AuDecoder("-").decode();
  } else {
    for (int i = 0; i < argc; i++) {
      std::string filename(argv[i]);
      AuDecoder(filename).decode();
    }
  }
  return 0;
}

int todo(int, char **) {
  std::cout << "not yet implemented." << std::endl; // TODO
  return 1;
}

class JsonSaxHandler : public BaseReaderHandler<UTF8<>, JsonSaxHandler> {
  AuFormatter &formatter;
  std::optional<bool> intern;
  bool toInt;

  bool tryInt(const char* str, SizeType length) {
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
      int64_t i = u * -1;
      formatter.value(i);
    } else {
      uint64_t u = strtoull(digits, &endptr, 10);
      if (endptr - digits != length) return false;
      formatter.value(u);
    }
    return true;
  }

public:
  explicit JsonSaxHandler(AuFormatter &formatter)
      : formatter(formatter), toInt(false)
  {}

  bool Null() { formatter.null(); return true; }
  bool Bool(bool b) { formatter.value(b); return true; }
  bool Int(int i) { formatter.value(i); return true; }
  bool Uint(unsigned u) { formatter.value(u); return true; }
  bool Int64(int64_t i) { formatter.value(i); return true; }
  bool Uint64(uint64_t u) { formatter.value(u); return true; }
  bool Double(double d) { formatter.value(d); return true; }
  bool String(const char* str, SizeType length, [[maybe_unused]] bool copy) {
    if (toInt) {
      toInt = false;
      if (tryInt(str, length)) return true;
    }
    formatter.value(std::string_view(str, length), intern);
    intern.reset();
    return true;
  }
  bool StartObject() { formatter.startMap(); return true; }
  bool Key(const char* str, SizeType length, [[maybe_unused]] bool copy) {
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
  bool EndObject([[maybe_unused]] SizeType memberCount) { formatter.endMap(); return true; }
  bool StartArray() { formatter.startArray(); return true; }
  bool EndArray([[maybe_unused]] SizeType elementCount) { formatter.endArray(); return true; }
};

int json2au(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    return -1;
  }
  std::string inFName(argv[0]);
  std::string outFName(argv[1]);

  size_t maxEntries = std::numeric_limits<size_t>::max();
  if (argc >= 3) {
    maxEntries = strtoull(argv[2], nullptr, 0);
  }

  FILE *inF;

  if (inFName == "-") {
    inF = fdopen(fileno(stdin), "rb");
  } else {
    inF = fopen(inFName.c_str(), "rb");
  }

  std::streambuf *outBuf;
  std::ofstream outFileStream;
  if (outFName == "-") {
    outBuf = std::cout.rdbuf();
  } else {
    outFileStream.open(outFName, std::ios_base::binary);
    outBuf = outFileStream.rdbuf();
  }
  std::ostream out(outBuf);
  Au au(out, 250'000, 100);

  char readBuffer[65536];
  FileReadStream in(inF, readBuffer, sizeof(readBuffer));

  Reader reader;
  ParseResult res;
  size_t entriesProcessed = 0;
  auto lastTime = std::chrono::steady_clock::now();
  size_t lastDictSize = 0;
  while (res) {
    au.encode([&](AuFormatter &f) {
      JsonSaxHandler handler(f);
      static constexpr auto parseOpt = kParseStopWhenDoneFlag +
                                       kParseFullPrecisionFlag +
                                       kParseNanAndInfFlag;
      res = reader.Parse<parseOpt>(in, handler);
    });

    entriesProcessed++;
    if (entriesProcessed % 10'000 == 0) {
      auto stats = au.getStats();
      auto tNow = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tNow - lastTime);
      cerr << "Processed: " << stats["Records"] << " entries in "
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

  fclose(inF);

  if (res.Code() == kParseErrorNone || res.Code() == kParseErrorDocumentEmpty) {
    return 0;
  } else {
    return res.Code();
  }
}

struct DictDumpHandler : public NoopHandler {
  bool dictAdd_;
  std::vector<char> str_;
  std::vector<std::string> dictEntries_;

  DictDumpHandler() : dictAdd_(false) {}

  void onDictClear() {
    cout << "\n\nDictionary cleared ***********************\n\n\n";
  }

  void onDictAddStart() { dictAdd_ = true; }
  void onDictAddEnd() {
    dictAdd_ = false;
    for (auto &e : dictEntries_) {
      std::cout << e << "\n";
    }
    std::cout << "\n";
    dictEntries_.clear();
  }

  void onStringStart(size_t len) {
    if (!dictAdd_) return;
    str_.clear();
    str_.reserve(len);
  }
  void onStringEnd() {
    if (dictAdd_) dictEntries_.emplace_back(str_.data(), str_.size());
  }
  void onStringFragment(std::string_view frag) {
    if (dictAdd_) str_.insert(str_.end(), frag.data(), frag.data()+frag.size());
  }
};

int stats(int argc, char **argv) {
  DictDumpHandler handler;
  if (argc == 0) {
    AuDecoder("-").decode(handler);
  } else {
    for (int i = 0; i < argc; i++) {
      std::string filename(argv[i]);
      AuDecoder(filename).decode(handler);
    }
  }
  return 0;
}

}


int main(int argc, char **argv) {
  if (argc < 2) {
    help(0, nullptr);
    return 1;
  }

  std::unordered_map<std::string, std::function<int(int, char **)>> commands;
  commands["--version"] = version;
  commands["--help"] = help;
  commands["cat"] = cat;
  commands["tail"] = todo;
  commands["grep"] = todo;
  commands["enc"] = todo;
  commands["json2au"] = json2au;
  commands["stats"] = stats;

  std::string cmd(argv[1]);
  auto it = commands.find(cmd);
  if (it == commands.end()) {
    std::cerr << "Unknown option or command: " << cmd << std::endl;
    return usage(std::cerr);
  }

  return it->second(argc-2, argv+2);
}

