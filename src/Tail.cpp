#include "main.h"
#include "JsonOutputHandler.h"
#include "Tail.h"
#include "TclapHelper.h"


namespace {

void usage() {
  std::cout
      << "usage: au tail [options] [--] <path>...\n"
      << "\n"
      << "  -h --help        show usage and exit\n"
      << "  -f --follow      output appended data as the file grows\n"
      << "  -b --bytes <n>   start <n> bytes from end of file (default 5k)\n";
}

}

int tail(int argc, const char *const *argv) {
  TclapHelper tclap(usage);

  TCLAP::SwitchArg follow("f", "follow", "follow", tclap.cmd(), false);
  // Offset in bytes so we can fine-tune the starting point for test purposes.
  TCLAP::ValueArg<size_t> startOffset(
      "b", "bytes", "bytes", false, 5 * 1024, "integer", tclap.cmd());
  TCLAP::UnlabeledValueArg<std::string> fileName(
      "path", "", true, "path", "", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  Dictionary dictionary;
  JsonOutputHandler jsonHandler;

  if (fileName.getValue().empty() || fileName.getValue() == "-") {
    std::cerr << "Tailing stdin not supported\n";
  } else {
    FileByteSourceImpl source(fileName, follow);
    source.tail(startOffset);
    TailHandler tailHandler(dictionary, source);
    tailHandler.parseStream(jsonHandler);
  }

  return 0;
}
