#include "TclapHelper.h"
#include "Zindex.h"

namespace {

void usage() {
  std::cout
      << "usage: au zindex [options] [--] <path>\n"
      << "\n"
      << " Builds an index for a gzipped au file. Writes index to <path>.auzx.\n"
      << " <path> may be \"-\" for stdin, in which case index is written to stdin.auzx.\n"
      << "\n"
      << "  -h --help        show usage and exit\n";
}

}

int zindex(int argc, const char * const *argv) {
  TclapHelper tclap(usage);

  TCLAP::UnlabeledValueArg<std::string> path(
      "path", "", true, "", "path", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  // TODO support stdin
  return zindexFile(path.getValue());
}
