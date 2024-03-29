#include "AuOutputHandler.h"
#include "AuRecordHandler.h"
#include "Dictionary.h"
#include "JsonOutputHandler.h"
#include "StreamDetection.h"
#include "TclapHelper.h"
#include "au/AuDecoder.h"

namespace au {

namespace {

void usage(const char *cmd) {
  std::cout
      << "usage: au " << cmd << " [options] [--] <path>...\n"
      << "\n"
      << " Decodes au to json. Reads stdin if no files specified. Writes to\n"
      << " stdout. Any <path> may be \"-\" for stdin.\n"
      << "\n"
      << "  -h --help        show usage and exit\n"
      << "  -e --encode      output au-encoded records rather than json\n";
}

template<typename H>
int doCat(const std::string &fileName, H &handler, bool compressed) {
  Dictionary dictionary;
  AuRecordHandler recordHandler(dictionary, handler);
  auto source = detectSource(fileName, std::nullopt, compressed);
  if (!checkAuFile(*source)) return 1;
  try {
    RecordParser<AuRecordHandler<H>>(*source, recordHandler).parseStream();
  } catch (const std::exception &e) {
    std::cerr << e.what() << " while processing " << fileName << "\n";
    return 1;
  }
  return 0;
}

int catFile(const std::string &fileName, bool encodeOutput, bool compressed) {
  if (encodeOutput) {
    AuOutputHandler handler(
        AU_STR("Re-encoded by au from original au file "
                << (fileName == "-" ? "<stdin>" : fileName)));
    return doCat(fileName, handler, compressed);
  } else {
    JsonOutputHandler handler;
    return doCat(fileName, handler, compressed);
  }
}

int catCmd(int argc, const char * const *argv, bool compressed) {
  TclapHelper tclap([compressed]() { usage(compressed ? "zcat" : "cat"); });

  TCLAP::UnlabeledMultiArg<std::string> fileNames(
      "path", "", false, "path", tclap.cmd());

  TCLAP::SwitchArg encode("e", "encode", "encode", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  std::vector<std::string> inputFiles{"-"};
  if (fileNames.isSet()) inputFiles = fileNames.getValue();

  for (const auto &f : inputFiles) {
    auto result = catFile(f, encode.isSet(), compressed);
    if (result) return result;
  }

  return 0;
}

}

int cat(int argc, const char * const *argv) {
  return catCmd(argc, argv, false);
}

int zcat(int argc, const char * const *argv) {
  return catCmd(argc, argv, true);
}

}
