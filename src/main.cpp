#include "main.h"

#include "au/AuCommon.h"
#include "au/Version.h"

#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

int version(int, char **) {
  std::cout << "au version " << au::AU_VERSION
            << " (encodes/decodes format version "
            << au::FormatVersion1::AU_FORMAT_VERSION << ")" << std::endl;
  return 0;
}

int usage(std::ostream &os) {
  os << "usage: au [--version] [--help] <command> [args]" << std::endl;
  return 1;
}

int help(int, char **) {
  usage(std::cout);
  std::cout << "\nCommands:\n"
    << "   cat      Decode listed files to stdout (alias au2json)\n"
    << "   tail     Decode and/or follow file\n"
    << "   grep     Find records matching pattern\n"
    << "   enc      Encode listed files to stdout (alias json2au)\n"
    << "   stats    Display file statistics\n"
    << "   zindex   Build an index of a gzipped file (to support grep -o)\n"
    << "            Works for .json and .au files. Index will be written to <file>.auzx\n"
    << "            unless specified with -x <index>\n"
    << "\n"
    << "   zcat     cat gzipped au file (deprecated, just use cat)\n"
    << "   zgrep    grep in gzipped file (deprecated, just use grep)\n"
    << "   ztail    tail gzipped au file (deprecated, just use tail)\n";

  return 0;
}

}

int main(int argc, char **argv) {
  if (argc < 2) {
    help(0, nullptr);
    return 1;
  }

  std::unordered_map<std::string, std::function<int(int, char **)>> commands;
  commands["--version"] = version;
  commands["--help"] = help;
  commands["cat"] = au::cat;
  commands["au2json"] = au::cat;
  commands["tail"] = au::tail;
  commands["grep"] = au::grep;
  commands["enc"] = au::json2au;
  commands["json2au"] = au::json2au;
  commands["stats"] = au::stats;
  commands["zindex"] = au::zindex;
  commands["zgrep"] = au::zgrep;
  commands["zcat"] = au::zcat;
  commands["ztail"] = au::ztail;

  // SAFETY: argc >= 2 guaranteed by check above. Suppressing false positive
  // from Clang's -Wunsafe-buffer-usage static analysis.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

  std::string cmd(argv[1]);

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  auto it = commands.find(cmd);
  if (it == commands.end()) {
    std::cerr << "Unknown option or command: " << cmd << std::endl;
    return usage(std::cerr);
  }

  try {
    return it->second(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "Runtime error: " << e.what() << std::endl;
  }
}
