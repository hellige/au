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
    << "   canned   Dump a canned snippet\n"
    << "   cat      Decode listed files to stdout\n"
    << "   tail     Decode and/or follow file\n"
    << "   grep     Find records matching pattern\n"
    << "   enc      Encode listed files to stdout\n"
    << "   json2au  Encode json to au (either can be '-')\n"
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
public:
  explicit JsonSaxHandler(AuFormatter &formatter) : formatter(formatter) {}

  bool Null() { formatter.null(); return true; }
  bool Bool(bool b) { formatter.value(b); return true; }
  bool Int(int i) { formatter.value(i); return true; }
  bool Uint(unsigned u) { formatter.value(u); return true; }
  bool Int64(int64_t i) { formatter.value(i); return true; }
  bool Uint64(uint64_t u) { formatter.value(u); return true; }
  bool Double(double d) { formatter.value(d); return true; }
  bool String(const char* str, SizeType length, [[maybe_unused]] bool copy) {
    formatter.value(std::string_view(str, length), intern);
    intern.reset();
    return true;
  }
  bool StartObject() { formatter.startMap(); return true; }
  bool Key(const char* str, SizeType length, [[maybe_unused]] bool copy) {
    formatter.value(std::string_view(str, length), true);
    if (0 == strncmp(str, "estdEventTime", length) ||
        0 == strncmp(str, "logTime", length)) {
      intern = false;
    }
    return true;
  }
  bool EndObject([[maybe_unused]] SizeType memberCount) { formatter.endMap(); return true; }
  bool StartArray() { formatter.startArray(); return true; }
  bool EndArray([[maybe_unused]] SizeType elementCount) { formatter.endArray(); return true; }
};

int json2au(int argc, char **argv) {
  if (argc < 2) {
    return -1;
  }

  std::string inFName(argv[0]);
  std::string outFName(argv[1]);

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
  Au au(out);

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
    if (entriesProcessed % 10000 == 0) {
      auto stats = au.getStats();
      auto tNow = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tNow - lastTime);
      cerr << "Processed: " << entriesProcessed << " entries in "
           << elapsed.count() << "ms. DictSize: " << stats["DictSize"]
           << " DictDelta: " << stats["DictSize"] - lastDictSize
           << " CacheSize: " << stats["CacheSize"] << "\n";
      lastTime = tNow;
      lastDictSize = stats["DictSize"];
    }
  }

  cerr << res.Code() << endl;

  fclose(inF);
  return res.Code();
}

int stats(int argc, char **argv) {
  for (int i = 0; i < argc; i++) {

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
  commands["stats"] = todo;

  std::string cmd(argv[1]);
  auto it = commands.find(cmd);
  if (it == commands.end()) {
    std::cerr << "Unknown option or command: " << cmd << std::endl;
    return usage(std::cerr);
  }

  return it->second(argc-2, argv+2);
}

