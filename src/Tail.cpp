#include "main.h"
#include "JsonOutputHandler.h"
#include "Tail.h"
#include "TclapHelper.h"
#include "au/FileByteSource.h"
#include "StreamDetection.h"

namespace au {

namespace {

void usage() {
  std::cout
      << "usage: au tail [options] [--] <path>...\n"
      << "\n"
      << "  -h --help           show usage and exit\n"
      << "  -f --follow         output appended data as the file grows\n"
      << "  -b --bytes <n>      start <n> bytes from end of file (default 5k)\n"
      << "  -x --index <path>   use gzip index in <path>\n";
}

int tailCmd(int argc, const char *const *argv, bool compressed) {
  TclapHelper tclap(usage);

  TCLAP::SwitchArg follow("f", "follow", "follow", tclap.cmd(), false);
  // Offset in bytes so we can fine-tune the starting point for test purposes.
  TCLAP::ValueArg<size_t> startOffset(
      "b", "bytes", "bytes", false, 5 * 1024, "integer", tclap.cmd());
  TCLAP::UnlabeledValueArg<std::string> fileName(
      "path", "", true, "path", "", tclap.cmd());
  TCLAP::ValueArg<std::string> index(
      "x", "index", "index", false, "", "string", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  Dictionary dictionary;
  JsonOutputHandler jsonHandler;

  if (fileName.getValue().empty() || fileName.getValue() == "-") {
    std::cerr << "Tailing stdin not supported\n";
  } else {
    std::optional<std::string> indexFile =
        index.isSet() ? std::optional{index.getValue()}
                      : std::nullopt;
    auto source = detectSource(fileName, indexFile, compressed);
    if (!source->isSeekable()) {
      std::cerr << "Cannot tail non-seekable file '" << source->name() << "'"
          << std::endl;
      return 0;
    }
    source->tail(startOffset);
    TailHandler tailHandler(dictionary, *source);
    tailHandler.parseStream(jsonHandler);
  }

  return 0;
}

}

int tail(int argc, const char * const *argv) {
  return tailCmd(argc, argv, false);
}

int ztail(int argc, const char * const *argv) {
  return tailCmd(argc, argv, true);
}

}
