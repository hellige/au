#include "au/AuDecoder.h"
#include "au/FileByteSource.h"
#include "AuRecordHandler.h"
#include "TclapHelper.h"

#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace au {

namespace {

constexpr size_t DEFAULT_DICT_ENTRIES = 25;

std::string commafy(uint64_t val) {
  if (!val) return "0";
  char buf[32];
  int i = 0, j = 0;
  while (val) {
    if (i++ % 3 == 0 && j) buf[j++] = ',';
    buf[j++] = static_cast<char>('0' + val % 10);
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
   // we know we don't support enormous numbers... the cast avoids a warning.
  double count = static_cast<double>(bytes);
  while (count >= 1024 && s < suffixes.size()) {
    s++;
    count /= 1024;
  }
  if (count - floor(count) == 0.0)
    snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(count), suffixes[s]);
  else
    snprintf(buf, sizeof(buf), "%.1f%s", count, suffixes[s]);
  buf[31] = 0;
  return std::string(buf);
}

struct SizeHistogram {
  std::string name;
  size_t totalValBytes = 0;
  std::vector<size_t> buckets; // by power of two

  explicit SizeHistogram(const char *name) : name(name) {}

  void add(size_t size) {
    totalValBytes += size;
    size_t bucket = 0;
    if (size)
      bucket = 8*sizeof(size_t)
        - static_cast<unsigned>(__builtin_clzll(size));
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
      printf("        %10s: %s (%zu%%) %s\n", prettyBytes(1u << i).c_str(),
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

  explicit VarintHistogram(const char *name) : name(name) {}

  void add(size_t size) {
    if (size > buckets.size()) buckets.resize(size);
    buckets[size - 1]++;
  }

  void dumpStats(size_t totalBytes) {
    size_t totalInts = 0;
    auto onePastLastPopulated = 0u;
    for (auto i = 0u; i < buckets.size(); i++) {
      totalInts += buckets[i];
      if (buckets[i]) onePastLastPopulated = i + 1;
    }
    std::cout << "     " << name << ": " << commafy(totalInts) << '\n'
              << "       By length:\n";
    size_t totalIntBytes = 0;
    for (auto i = 0u; i < onePastLastPopulated; i++) {
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

struct StatsValueHandler : public NoopValueHandler {
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
  AuByteSource *source_;

  StatsValueHandler(std::vector<size_t> &dictFrequency)
      : dictFrequency(dictFrequency) {}

  void onValue(AuByteSource &source, const Dictionary::Dict &dict) {
    dictionary = &dict;
    source_ = &source;
    ValueParser<StatsValueHandler> parser(source, *this);
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

  void onTime(size_t pos, time_point) override {
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
  struct Header {
    size_t pos;
    size_t recordNum;
    uint64_t version;
    std::string metadata;
  };

  Dictionary dictionary;
  std::vector<size_t> dictFrequency;
  StatsValueHandler vh;
  AuRecordHandler<StatsValueHandler> next;
  bool fullDictDump;
  SizeHistogram valueHist {"Value records"};
  size_t numRecords = 0;
  size_t dictClears = 0;
  size_t dictAdds = 0;
  std::vector<Header> headers;
  size_t sor = 0;

  explicit StatsRecordHandler(bool fullDictDump)
  : vh(dictFrequency),
    next(dictionary, vh),
    fullDictDump(fullDictDump) {}

  void onRecordStart(size_t pos) {
    sor = pos;
    numRecords++;
    next.onRecordStart(pos);
  }

  void onHeader(uint64_t version, const std::string &metadata) {
    headers.push_back(Header{sor, numRecords, version, metadata});
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

  void onValue(size_t relDictPos, size_t len, AuByteSource &source) {
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

class StatsDecoder {
  std::string filename_;

public:
  StatsDecoder(const std::string &filename)
      : filename_(filename) {}

  int decode(StatsRecordHandler &handler) const {
    FileByteSourceImpl source(filename_, false);
    try {
      RecordParser(source, handler).parseStream();
    } catch (parse_error &e) {
      std::cout << e.what() << std::endl;
      return 1;
    }

    auto *dict = handler.dictionary.latest();
    if (dict && dict->size())
      dictStats(*dict, handler.dictFrequency, "at end of file",
                handler.fullDictDump);

    std::cout
        << "Stats for " << filename_ << ":\n"
        << "  Headers seen:\n";

    for (const auto &h : handler.headers) {
      std::cout
          << "     Record number " << commafy(h.recordNum) << " at byte "
          << commafy(h.pos) << ", format version " << h.version << ". ";
      if (h.metadata.empty())
        std::cout << "No metadata.\n";
      else
        std::cout << "With metadata:\n       " << h.metadata << "\n";
    }

    std::cout
        << "  Total read: " << prettyBytes(source.pos()) << '\n'
        << "  Records: " << commafy(handler.numRecords) << '\n'
        << "     Version headers: " << commafy(handler.headers.size()) << '\n'
        << "     Dictionary resets: " << commafy(handler.dictClears) << '\n'
        << "     Dictionary adds: " << commafy(handler.dictAdds) << '\n';
    handler.valueHist.dumpStats(source.pos());
    handler.vh.dumpStats(source.pos());

    return 0;
  }
};

void usage() {
  std::cout
      << "usage: au stats [options] [--] <path>...\n"
      << "\n"
      << "  -h --help        show usage and exit\n"
      << "  -d --dict        dump full dictionary\n";
}

}

int stats(int argc, const char * const *argv) {
  TclapHelper tclap(usage);

  TCLAP::SwitchArg dictDump("d", "dict", "dict", tclap.cmd(), false);
  TCLAP::UnlabeledMultiArg<std::string> fileNames(
      "path", "", false, "path", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  if (fileNames.getValue().empty()) {
    StatsRecordHandler handler(dictDump.isSet());
    return StatsDecoder("-").decode(handler);
  } else {
    for (auto &f : fileNames.getValue()) {
      StatsRecordHandler handler(dictDump.isSet());
      auto result = StatsDecoder(f).decode(handler);
      if (result) return result;
    }
  }

  return 0;
}

}
