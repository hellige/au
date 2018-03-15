#include "AuDecoder.h"

#include <iostream>
#include <string.h>
#include "tclap/CmdLine.h"
#include <vector>

namespace {

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

struct SmallIntValueHandler : public NoopValueHandler {
  size_t count;
  size_t countLt64;
  size_t countLt128;
  SmallIntValueHandler() : count(0), countLt64(0), countLt128(0) {}
  void onInt(int64_t i) override {
    count++;
    if (i < 64 && i > -64) countLt64++;
    if (i < 128 && i > -128) countLt128++;
  }
  void onUint(uint64_t u) override {
    count++;
    if (u < 64) countLt64++;
    if (u < 128) countLt128++;
  }
  void report() {
    std::cout << "Total ints : " << count << "\n";
    std::cout << "Total < 128: " << countLt128 << "\n";
    std::cout << "Total < 64 : " << countLt64 << "\n";
  }
};

struct SmallIntRecordHandler : public NoopRecordHandler {
  SmallIntValueHandler vh;

  void onValue(size_t, size_t, FileByteSource &source) override {
    ValueParser<SmallIntValueHandler> parser(source, vh);
    parser.value();
  }

  void onParseEnd() override { vh.report(); }
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

}

// TODO here are some stats i'd like:
//   - total bytes
//   - total records (by type)
//   - absolute count and count-in-bytes by type
//   - histogram of varint size for:
//     - values
//     - backrefs
//     - record length
//     - string length
//     - dict refs
//   - histogram of string lengths (by power of two?)
//   - count-in-bytes of dictionary refs
//   - dictionary stats:
//     - size of dict
//     - frequency of reference

int stats(int argc, const char * const *argv) {
  SmallIntRecordHandler smallIntRecordHandler;
  DictDumpHandler dictDumpHandler;
  NoopRecordHandler *handler = &dictDumpHandler;
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

    if (intCnt) {
      handler = &smallIntRecordHandler;
    } else if (dictDump) {
      handler = &dictDumpHandler;
    }

    if (fileNames.getValue().empty()) {
      AuDecoder("-").decode(*handler, false);
    } else {
      for (auto &f : fileNames.getValue()) {
        AuDecoder(f).decode(*handler, false);
      }
    }
  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  return 0;
}
