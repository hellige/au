#include "AuDecoder.h"

#include "AuRecordHandler.h"

#include <tclap/CmdLine.h>

#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr size_t DEFAULT_DICT_ENTRIES = 25;

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
  std::array<const char *, 5> suffixes {"B", "K", "M", "G", "T"};
  char buf[32];
  auto s = 0u; // which suffix to use
  double count = bytes;
  while (count >= 1024 && s < suffixes.size()) {
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

  void dumpStats(std::optional<size_t> totalBytes) {
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
    if (totalBytes) {
    std::cout << "       Total bytes: " << prettyBytes(totalValBytes)
                << " (" << (100 * totalValBytes / *totalBytes)
                << "% of stream)\n";
    }
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
      auto bytes = buckets[i] * (i + 1);
      totalIntBytes += bytes;
      printf("        %3d: %s (%zu%%) %s\n", i + 1, commafy(buckets[i]).c_str(),
             100 * buckets[i] / totalInts, prettyBytes(bytes).c_str());
    }
    std::cout << "       Total bytes: " << prettyBytes(totalIntBytes)
              << " (" << (100 * totalIntBytes / totalBytes)
              << "% of stream)\n";
  }
};

void dictStats(const Dictionary::Dict &dictionary,
               const std::vector<size_t> &dictFrequency,
               const char *event,
               bool fullDump) {
  std::cout
      << "Dictionary stats " << event << ":\n"
      << "  Total entries: " << commafy(dictionary.size()) << '\n';
  SizeHistogram hist {"Dictionary entries"};
  for (auto &&entry : dictionary.entries()) hist.add(entry.size());
  hist.dumpStats({});

  auto numEntries = dictionary.size();
  if (!fullDump) numEntries = std::min(numEntries, DEFAULT_DICT_ENTRIES);

  std::vector<std::pair<size_t, std::string>> byFreq;
  for (auto i = 0u; i < dictionary.size(); i++)
    byFreq.emplace_back(dictFrequency[i], dictionary.at(i));
  std::sort(byFreq.begin(), byFreq.end(),
            std::greater<std::pair<size_t, std::string>>());
  std::cout << "     Referral count";
  if (numEntries != dictionary.size())
    std::cout << " (top " << numEntries << " entries)";
  std::cout << ":\n";
  for (auto i = 0u; i < numEntries; i++)
    std::cout << "       " << commafy(byFreq[i].first) << ": "
              << byFreq[i].second << '\n';
}

struct SmallIntValueHandler : public NoopValueHandler {
  std::vector<size_t> &dictFrequency;
  const Dictionary::Dict *dictionary = nullptr;
  size_t doubles = 0;
  size_t doubleBytes = 0;
  size_t timestamps = 0;
  size_t timestampBytes = 0;
  size_t bools = 0;
  size_t boolBytes = 0;
  size_t nulls = 0;
  size_t nullBytes = 0;
  SizeHistogram stringHist {"String values"};
  SizeHistogram dictStringHist {"Strings from dictionary"};
  VarintHistogram intValues {"Integer values"};
  VarintHistogram dictRefs {"Dictionary references"};
  VarintHistogram stringLengths {"String length encodings"};
  FileByteSource *source_;

  SmallIntValueHandler(std::vector<size_t> &dictFrequency)
      : dictFrequency(dictFrequency) {}

  void onValue(FileByteSource &source, const Dictionary::Dict &dict) {
    dictionary = &dict;
    source_ = &source;
    ValueParser<SmallIntValueHandler> parser(source, *this);
    parser.value();
    source_ = nullptr;
  }

  void onBool(size_t pos, bool) override {
    bools++;
    boolBytes += source_->pos() - pos;
  }

  void onNull(size_t pos) override {
    nulls++;
    nullBytes += source_->pos() - pos;
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

  void onTime(size_t pos, std::chrono::nanoseconds) {
    timestamps++;
    timestampBytes += source_->pos() - pos;
  }

  void onDictRef(size_t pos, size_t idx) override {
    dictStringHist.add(dictionary->at(idx).size());
    dictRefs.add(source_->pos() - pos);
    dictFrequency[idx]++;
  }

  void onStringStart(size_t pos, size_t len) override {
    stringHist.add(len);
    stringLengths.add(source_->pos() - pos);
  }

  void dumpStats(size_t totalBytes) {
    std::cout
        << "  Values:\n"
        << "     Doubles: " << commafy(doubles) << '\n'
        << "       Total bytes: " << prettyBytes(doubleBytes)
        << " (" << (100 * doubleBytes / totalBytes) << "% of stream)\n"
        << "     Timestamps: " << commafy(timestamps) << '\n'
        << "       Total bytes: " << prettyBytes(timestampBytes)
        << " (" << (100 * timestampBytes / totalBytes) << "% of stream)\n"
        << "     Bools: " << commafy(bools) << '\n'
        << "       Total bytes: " << prettyBytes(boolBytes)
        << " (" << (100 * boolBytes / totalBytes) << "% of stream)\n"
        << "     Nulls: " << commafy(nulls) << '\n'
        << "       Total bytes: " << prettyBytes(nullBytes)
        << " (" << (100 * nullBytes / totalBytes) << "% of stream)\n";
    intValues.dumpStats(totalBytes);
    dictRefs.dumpStats(totalBytes);
    dictStringHist.dumpStats({});
    stringHist.dumpStats(totalBytes);
    stringLengths.dumpStats(totalBytes);
  }
};

struct StatsRecordHandler {
  Dictionary dictionary;
  std::vector<size_t> dictFrequency;
  SmallIntValueHandler vh;
  AuRecordHandler<SmallIntValueHandler> next;
  bool fullDictDump;
  SizeHistogram valueHist {"Value records"};
  uint64_t formatVersion = 0;
  std::string metadata;
  size_t numRecords = 0;
  size_t dictClears = 0;
  size_t dictAdds = 0;
  size_t headers = 0;

  StatsRecordHandler(bool fullDictDump)
  : vh(dictFrequency),
    next(dictionary, vh),
    fullDictDump(fullDictDump) {}

  void onRecordStart(size_t pos) {
    numRecords++;
    next.onRecordStart(pos);
  }

  void onHeader(uint64_t version, const std::string &metadata) {
    headers++;
    formatVersion = version;
    this->metadata = metadata;
    next.onHeader(version, metadata);
  }

  void onDictClear() {
    dictClears++;
    auto *dict = dictionary.latest();
    if (dict && dict->size())
      dictStats(*dict, dictFrequency, "upon clear", fullDictDump);
    dictFrequency.clear();
    next.onDictClear();
  }

  void onDictAddStart(size_t relDictPos) {
    dictAdds++;
    next.onDictAddStart(relDictPos);
  }

  void onValue(size_t relDictPos, size_t len, FileByteSource &source) {
    valueHist.add(len);
    next.onValue(relDictPos, len, source);
  }

  void onStringStart(size_t pos, size_t len) {
    next.onStringStart(pos, len);
  }

  void onStringEnd() {
    dictFrequency.emplace_back(0);
    next.onStringEnd();
  }

  void onStringFragment(std::string_view fragment) {
    next.onStringFragment(fragment);
  }
};

void usage() {
  std::cout
      << "usage: au stats [options] [--] <path>...\n"
      << "\n"
      << "  -h --help        show usage and exit\n"
      << "  -d --dict        dump full dictionary\n";
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

  void decode(StatsRecordHandler &handler) const {
    FileByteSource source(filename_, false);
    try {
      RecordParser(source, handler).parseStream();
    } catch (parse_error &e) {
      std::cout << e.what() << std::endl;
    }

    auto *dict = handler.dictionary.latest();
    if (dict && dict->size())
      dictStats(*dict, handler.dictFrequency, "at end of file",
                handler.fullDictDump);

    // TODO record/dump format version and metadata for all headers in stream
    std::cout
        << "Stats for " << filename_ << " (format version "
        << handler.formatVersion << "):\n"
        << "  Stream metadata: " << handler.metadata << '\n'
        << "  Total read: " << prettyBytes(source.pos()) << '\n'
        << "  Records: " << commafy(handler.numRecords) << '\n'
        << "     Version headers: " << commafy(handler.headers) << '\n'
        << "     Dictionary resets: " << commafy(handler.dictClears) << '\n'
        << "     Dictionary adds: " << commafy(handler.dictAdds) << '\n';
    handler.valueHist.dumpStats(source.pos());
    handler.vh.dumpStats(source.pos());
  }
};

}

int stats(int argc, const char * const *argv) {
  std::vector<std::string> auFiles;

  try {
    UsageVisitor usageVisitor;
    TCLAP::CmdLine cmd("", ' ', "", false);
    TCLAP::UnlabeledValueArg<std::string> subCmd(
        "stats", "stats", true, "stats", "command", cmd);
    TCLAP::SwitchArg help("h", "help", "help", cmd, false, &usageVisitor);

    TCLAP::SwitchArg dictDump("d", "dict", "Dictionary dump", cmd, false);
    TCLAP::UnlabeledMultiArg<std::string> fileNames(
        "fileNames", "", false, "filename", cmd);

    StatsOutput output;
    cmd.setOutput(&output);
    cmd.parse(argc, argv);

    if (fileNames.getValue().empty()) {
      StatsRecordHandler handler(dictDump.isSet());
      StatsDecoder("-").decode(handler);
    } else {
      for (auto &f : fileNames.getValue()) {
        StatsRecordHandler handler(dictDump.isSet());
        StatsDecoder(f).decode(handler);
      }
    }
  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  return 0;
}
