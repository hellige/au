#include "Dictionary.h"
#include "JsonOutputHandler.h"
#include "au/AuDecoder.h"
#include "AuRecordHandler.h"
#include "TclapHelper.h"

namespace {

void usage() {
  std::cout
      << "usage: au cat [options] [--] <path>...\n"
      << "\n"
      << " Decodes au to json. Reads stdin if no files specified. Writes to\n"
      << " stdout. Any <path> may be \"-\" for stdin.\n"
      << "\n"
      << "  -h --help        show usage and exit\n";
}

}

int cat(int argc, const char * const *argv) {
  TclapHelper tclap(usage);

  TCLAP::UnlabeledMultiArg<std::string> fileNames(
      "path", "", false, "path", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  Dictionary dictionary;
  JsonOutputHandler valueHandler;
  AuRecordHandler<JsonOutputHandler> recordHandler(dictionary, valueHandler);

  std::vector<std::string> inputFiles{"-"};
  if (fileNames.isSet()) inputFiles = fileNames.getValue();

  int result = 0;
  for (const auto &f : inputFiles) {
    try {
      AuDecoder(f).decode(recordHandler, false);
    } catch (const std::exception &e) {
      std::cerr << e.what() << " while processing " << f << "\n";
      result = 1;
    }
  }

  return result;
}