#include "AuOutputHandler.h"
#include "Dictionary.h"
#include "JsonOutputHandler.h"
#include "au/AuDecoder.h"
#include "AuRecordHandler.h"
#include "TclapHelper.h"

namespace au {

namespace {

void usage() {
  std::cout
      << "usage: au cat [options] [--] <path>...\n"
      << "\n"
      << " Decodes au to json. Reads stdin if no files specified. Writes to\n"
      << " stdout. Any <path> may be \"-\" for stdin.\n"
      << "\n"
      << "  -h --help        show usage and exit\n"
      << "  -e --encode      output au-encoded records rather than json\n";
}

template<typename H>
int doCat(const std::string &fileName, H &handler) {
  Dictionary dictionary;
  AuRecordHandler recordHandler(dictionary, handler);
  FileByteSourceImpl source(fileName, false);
  try {
    RecordParser(source, recordHandler).parseStream();
  } catch (const std::exception &e) {
    std::cerr << e.what() << " while processing " << fileName << "\n";
    return 1;
  }
  return 0;
}

int catFile(const std::string &fileName, bool encodeOutput) {
  if (encodeOutput) {
    AuOutputHandler handler(
        AU_STR("Re-encoded by au from original au file "
                << (fileName == "-" ? "<stdin>" : fileName)));
    return doCat(fileName, handler);
  } else {
    JsonOutputHandler handler;
    return doCat(fileName, handler);
  }
}

}

int cat(int argc, const char * const *argv) {
  TclapHelper tclap(usage);

  TCLAP::UnlabeledMultiArg<std::string> fileNames(
      "path", "", false, "path", tclap.cmd());

  TCLAP::SwitchArg encode("e", "encode", "encode", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  std::vector<std::string> inputFiles{"-"};
  if (fileNames.isSet()) inputFiles = fileNames.getValue();

  for (const auto &f : inputFiles) {
    auto result = catFile(f, encode.isSet());
    if (result) return result;
  }

  return 0;
}

}