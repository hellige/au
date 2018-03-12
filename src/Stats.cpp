#include "AuDecoder.h"

#include <iostream>
#include <vector>

int stats(int argc, char **argv) {
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

  SmallIntRecordHandler smallIntRecordHandler;
  DictDumpHandler dictDumpHandler;

  NoopRecordHandler *handler = &dictDumpHandler;

  if (argc == 0) {
    AuDecoder("-").decode(*handler);
  } else {
    for (int i = 0; i < argc; i++) {
      std::string filename(argv[i]);
      AuDecoder(filename).decode(*handler);
    }
  }

  return 0;
}
