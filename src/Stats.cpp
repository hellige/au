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

  void onDictClear() {
    std::cout << "Dictionary cleared:\n";
  }

  void onDictAddStart(size_t) {
    std::cout << "\tDictionary appended:\n";
  }

  void onStringStart(size_t len) {
    str_.clear();
    str_.reserve(len);
  }

  void onStringEnd() {
    std::cout << "\t\t" << std::string_view(str_.data(), str_.size()) << "\n";
  }

  void onStringFragment(std::string_view frag) {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
};

struct SmallIntValueHandler : public NoopValueHandler {
  size_t count;
  size_t countLt64;
  size_t countLt128;
  SmallIntValueHandler() : count(0), countLt64(0), countLt128(0) {}
  void onInt(int64_t i) {
    count++;
    if (i < 64 && i > -64) countLt64++;
    if (i < 128 && i > -128) countLt128++;
  }
  void onUint(uint64_t u) {
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

  void onValue(size_t, size_t , FileByteSource &source) {
    ValueParser<SmallIntValueHandler> parser(source, vh);
    parser.value();
  }

  void onParseEnd() { vh.report(); }
};

}

int stats(int argc, char **argv) {
  SmallIntRecordHandler smallIntRecordHandler;
  DictDumpHandler dictDumpHandler;
  NoopRecordHandler *handler = &dictDumpHandler;
  std::vector<std::string> auFiles;

  try {
    TCLAP::CmdLine cmd("Statistics sub-command", ' ', "1", true);
    TCLAP::UnlabeledValueArg<std::string> subCmd("subCmd", "Must be \"stats\"", true, "stats", "Sub-command");
    TCLAP::SwitchArg dictDump("d", "dict", "Dictionary dump", cmd, false);
    TCLAP::SwitchArg intCnt("i", "ints", "Count of small integers", cmd, false);
    TCLAP::UnlabeledMultiArg<std::string> fileNames("fileNames", "Au files", false, "FileName");

    cmd.add(subCmd);
    cmd.add(fileNames);
    cmd.parse(argc, argv);

    if (intCnt.isSet()) {
      handler = &smallIntRecordHandler;
    } else if (dictDump.isSet()) {
      handler = &dictDumpHandler;
    }

    if (fileNames.getValue().empty()) {
      AuDecoder("-").decode(*handler);
    } else {
      for (auto &f : fileNames.getValue()) {
        AuDecoder(f).decode(*handler);
      }
    }
  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
  }


  return 0;
}
