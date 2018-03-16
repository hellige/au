#include "AuDecoder.h"

#include "RecordHandler.h"

#include <tclap/CmdLine.h>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string commafy(int64_t val) {
  if (val > 0) return commafy(val);
  auto result = commafy(-val);
  result.insert(0, 1, '-');
  return result;
}

std::string commafy(uint64_t val) {
  if (!val) return "0";
  char buf[32];
  int i = 0, j = 0;
  while (val) {
    if (i++ % 3 == 0 && j) buf[j++] = ',';
    buf[j++] = '0' + static_cast<char>(val % 10);
    val /= 10;
  }
  buf[j] = 0;
  std::string result(buf);
  std::reverse(result.begin(), result.end());
  return result;
}

std::string prettyBytes(size_t bytes) {
  const char* suffixes[] = {" bytes", "K", "M", "G", "T", "P", "E"};
  char buf[32];
  int s = 0; // which suffix to use
  double count = bytes;
  while (count >= 1024 && s < 7) {
    s++;
    count /= 1024;
  }
  if (count - floor(count) == 0.0)
    snprintf(buf, sizeof(buf), "%d%s", (int)count, suffixes[s]);
  else
    snprintf(buf, sizeof(buf), "%.1f%s", count, suffixes[s]);
  buf[31] = 0;
  return std::string(buf);
}

void dictStats(Dictionary &dictionary, const char *event) {
  std::cout
      << "Dictionary stats " << event << ":\n"
      << "  Total entries: " << commafy(dictionary.size()) << '\n';
}

struct DictDumpHandler : public NoopRecordHandler {
  std::vector<char> str_;

  DictDumpHandler() {
    str_.reserve(1 << 16);
  }

  void onDictClear() override {
    std::cout << "Dictionary cleared:\n";
  }

  void onDictAddStart(size_t) override {
    std::cout << "\tDictionary appended:\n";
  }

  void onStringStart(size_t len) override {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() override {
    std::cout << "\t\t" << std::string_view(str_.data(), str_.size()) << "\n";
  }

  void onStringFragment(std::string_view frag) override {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
};

#include <cmath>
struct SizeHistogram {
  std::string name;
  size_t totalValBytes = 0;
  std::vector<size_t> buckets; // by power of two

  SizeHistogram(const char *name) : name(name) {}

  void add(size_t size) {
    totalValBytes += size;
    auto bucket = size ? 8*sizeof(size_t) - __builtin_clzll(size) : 0;
    if (bucket+1 > buckets.size()) buckets.resize(bucket+1);
    buckets[bucket]++;
  }

  void dumpStats(size_t totalBytes) {
    size_t totalStrings = 0;
    for (auto count : buckets) totalStrings += count;
    std::cout << "     " << name << ": " << commafy(totalStrings) << '\n'
              << "       By length, less than:\n";
    for (auto i = 0u; i < buckets.size(); i++) {
      auto bytes = buckets[i] * (i+1);
      printf("        %10s: %s (%zu%%) %s\n", prettyBytes(1 << i).c_str(),
             commafy(buckets[i]).c_str(),
             100*buckets[i]/totalStrings,
             prettyBytes(bytes).c_str());
    }
    std::cout << "       Total bytes: " << prettyBytes(totalValBytes)
              << " (" << (100 * totalValBytes / totalBytes) << "% of stream)\n";
  }
};


struct VarintHistogram {
  std::string name;
  std::vector<size_t> buckets; // by size

  VarintHistogram(const char *name) : name(name) {}

  void add(size_t size) {
    if (size > buckets.size()) buckets.resize(size);
    buckets[size - 1]++;
  }

  void dumpStats(size_t totalBytes) {
    size_t totalInts = 0;
    auto lastPopulated = 0u;
    for (auto i = 0u; i < buckets.size(); i++) {
      totalInts += buckets[i];
      if (buckets[i]) lastPopulated = i;
    }
    std::cout << "     " << name << ": " << commafy(totalInts) << '\n'
              << "       By length:\n";
    size_t totalIntBytes = 0;
    for (auto i = 0u; i <= lastPopulated; i++) {
      auto bytes = buckets[i] * (i+1);
      totalIntBytes += bytes;
      printf("        %3d: %s (%zu%%) %s\n", i+1, commafy(buckets[i]).c_str(),
             100*buckets[i]/totalInts, prettyBytes(bytes).c_str());
    }
    std::cout << "       Total bytes: " << prettyBytes(totalIntBytes)
              << " (" << (100 * totalIntBytes / totalBytes) << "% of stream)\n";
  }
};

struct SmallIntValueHandler : public NoopValueHandler {
  const Dictionary &dictionary;
  size_t doubles = 0;
  size_t doubleBytes = 0;
  SizeHistogram stringHist {"String values"};
  size_t strings = 0;
  size_t stringBytes = 0;
  size_t dictExpansionBytes = 0;
  VarintHistogram intValues {"Integer values"};
  VarintHistogram dictRefs {"Dictionary references"};
  VarintHistogram stringLengths {"String length encodings"};
  FileByteSource *source_;

  SmallIntValueHandler(const Dictionary &dictionary) : dictionary(dictionary) {}

  void onValue(FileByteSource &source) {
    source_ = &source;
    ValueParser<SmallIntValueHandler> parser(source, *this);
    parser.value();
    source_ = nullptr;
  }

  void onInt(size_t pos, int64_t) override {
    intValues.add(source_->pos() - pos);
  }

  void onUint(size_t pos, uint64_t) override {
    intValues.add(source_->pos() - pos);
  }

  void onDouble(size_t pos, double) override {
    doubles++;
    doubleBytes += source_->pos() - pos;
  }

  void onDictRef(size_t pos, size_t idx) override {
    dictRefs.add(source_->pos() - pos);
    dictExpansionBytes += dictionary[idx].size();
  }

  void onStringStart(size_t pos, size_t len) override {
    stringHist.add(len);
    strings++;
    stringLengths.add(source_->pos() - pos);
    stringBytes += len;
  }

  void dumpStats(size_t totalBytes) {
    std::cout
        << "  Values:\n"
        << "     Doubles: " << commafy(doubles) << '\n';
    std::cout << "       Total bytes: " << prettyBytes(doubleBytes)
              << " (" << (100 * doubleBytes / totalBytes) << "% of stream)\n";
    intValues.dumpStats(totalBytes);
    dictRefs.dumpStats(totalBytes);
    std::cout << "       Total bytes to be expanded from dictionary: "
              << prettyBytes(dictExpansionBytes) << '\n';
    std::cout
        << "     Strings: " << commafy(strings) << '\n';
    std::cout << "       Total bytes: " << prettyBytes(stringBytes)
              << " (" << (100 * stringBytes / totalBytes) << "% of stream)\n";
    stringHist.dumpStats(totalBytes);
    stringLengths.dumpStats(totalBytes);
  }
};

struct SmallIntRecordHandler {
  Dictionary dictionary;
  SmallIntValueHandler vh;
  RecordHandler<SmallIntValueHandler> next;
  size_t numRecords = 0;
  size_t numValues = 0;
  size_t dictClears = 0;
  size_t dictAdds = 0;
  size_t headers = 0;

  SmallIntRecordHandler()
  : vh(dictionary), next(dictionary, vh) {}

  void onRecordStart(size_t pos) {
    numRecords++;
    next.onRecordStart(pos);
  }

  void onHeader(uint64_t version) {
    headers++;
    next.onHeader(version);
  }

  void onDictClear() {
    dictClears++;
    if (dictionary.size()) dictStats(dictionary, "upon clear");
    next.onDictClear();
  }

  void onDictAddStart(size_t relDictPos) {
    dictAdds++;
    next.onDictAddStart(relDictPos);
  }

  void onValue(size_t relDictPos, size_t len, FileByteSource &source) {
    numValues++;
    next.onValue(relDictPos, len, source);
  }

  void onStringStart(size_t pos, size_t len) {
    next.onStringStart(pos, len);
  }

  void onStringEnd() {
    next.onStringEnd();
  }

  void onStringFragment(std::string_view fragment) {
    next.onStringFragment(fragment);
  }

  void onParseEnd() {
    dictStats(dictionary, "at end of file");
    next.onParseEnd();
  }
};

void usage() {
  std::cout
      << "usage: au stats [options] [--] <path>...\n"
      << "\n"
      << "  -h --help        show usage and exit\n"
      << "  -d --dict        dump dictionary\n"
      << "  -i --ints        show count of small integers\n";
}

struct UsageVisitor : public TCLAP::Visitor {
  void visit() override {
    usage();
    exit(0);
  };
};

class StatsOutput : public TCLAP::StdOutput {
public:
  void failure(TCLAP::CmdLineInterface &, TCLAP::ArgException &e) override {
    std::cerr << e.error() << std::endl;
    ::usage();
    exit(1);
  }

  void usage(TCLAP::CmdLineInterface &) override {
    ::usage();
  }
};

class StatsDecoder {
  std::string filename_;

public:
  StatsDecoder(const std::string &filename)
      : filename_(filename) {}

  void decode(SmallIntRecordHandler &handler) const {
    FileByteSource source(filename_, false);
    try {
      RecordParser(source, handler).parseStream();
      handler.onParseEnd();
    } catch (parse_error &e) {
      std::cout << e.what() << std::endl;
    }

    std::cout
        << "Stats for " << filename_ << ":\n"
        << "  Total read: " << prettyBytes(source.pos()) << '\n'
        << "  Records: " << commafy(handler.numRecords) << '\n'
        << "     Version headers: " << commafy(handler.headers) << '\n'
        << "     Dictionary resets: " << commafy(handler.dictClears) << '\n'
        << "     Dictionary adds: " << commafy(handler.dictAdds) << '\n'
        << "     Values: " << commafy(handler.numValues) << '\n';
    handler.vh.dumpStats(source.pos());
  }
};

}

// TODO here are some stats i'd like:
//   - version(s)
// X - total bytes
// X - total records (by type)
//   - absolute count and count-in-bytes by type
//   - histogram of varint size for:
// X   - values
//     - backrefs
//     - record length
// X   - string length
// X   - dict refs
// X - histogram of string lengths (by power of two?)
//   - histogram of record lengths?
//   - count-in-bytes of dictionary ref expansions (and histogram?)
//   - dictionary stats:
//     - size of dict
//     - frequency of reference

int stats(int argc, const char * const *argv) {
  SmallIntRecordHandler smallIntRecordHandler;
//  DictDumpHandler dictDumpHandler;
//  NoopRecordHandler *handler = &dictDumpHandler;
  std::vector<std::string> auFiles;

  try {
    UsageVisitor usageVisitor;
    TCLAP::CmdLine cmd("", ' ', "", false);
    TCLAP::UnlabeledValueArg<std::string> subCmd(
        "stats", "stats", true, "stats", "command", cmd);
    TCLAP::SwitchArg help("h", "help", "help", cmd, false, &usageVisitor);

    TCLAP::SwitchArg dictDump("d", "dict", "Dictionary dump", cmd, false);
    TCLAP::SwitchArg intCnt("i", "ints", "Count of small integers", cmd, false);
    TCLAP::UnlabeledMultiArg<std::string> fileNames(
        "fileNames", "", false, "filename", cmd);

    StatsOutput output;
    cmd.setOutput(&output);
    cmd.parse(argc, argv);

//    if (intCnt) {
//      handler = &smallIntRecordHandler;
//    } else if (dictDump) {
//      handler = &dictDumpHandler;
//    }

    if (fileNames.getValue().empty()) {
      StatsDecoder("-").decode(smallIntRecordHandler);
    } else {
      for (auto &f : fileNames.getValue()) {
        StatsDecoder(f).decode(smallIntRecordHandler);
      }
    }
  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  return 0;
}
