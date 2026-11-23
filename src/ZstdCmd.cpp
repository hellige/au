#include "TclapHelper.h"
#include "ZstdSeekable.h"

#include <thread>

namespace au {

namespace {

void usage() {
  std::cout
      << "usage: au zstd [options] [--] <path>\n"
      << "\n"
      << " Compresses <path> in the seekable zstd format, which supports random\n"
      << " access (au grep binary search, au tail) without a separate index.\n"
      << " Output is a standard zstd stream readable by any zstd tool. Writes\n"
      << " to <path>.zst unless -o is given. <path> may be \"-\" for stdin.\n"
      << "\n"
      << "  -h --help              show usage and exit\n"
      << "  -o --output <path>     write to <path> (\"-\" for stdout)\n"
      << "  -l --level <n>         compression level (default 9)\n"
      << "  -T --threads <n>       compression threads (default 1, 0 = #cores)\n"
      << "  -f --frame-size <n>    uncompressed bytes per frame; accepts K/M/G\n"
      << "                         suffixes (default 16M)\n"
      << "     --long <n>          enable long-distance matching with window\n"
      << "                         log <n> (only useful for large frames)\n"
      << "     --no-check          don't write per-frame content checksums\n"
      << "     --au                input is au: start each frame with the full\n"
      << "                         dictionary so frames decode independently,\n"
      << "                         which makes binary search much faster. The\n"
      << "                         decoded records are unchanged, but the\n"
      << "                         decompressed bytes will differ from the input.\n"
      << "  -v --verbose           print progress and stats to stderr\n";
}

size_t parseSize(const std::string &s) {
  size_t idx = 0;
  auto n = std::stoull(s, &idx);
  auto suffix = s.substr(idx);
  if (suffix.empty()) return n;
  if (suffix == "K" || suffix == "k" || suffix == "KiB") return n << 10;
  if (suffix == "M" || suffix == "m" || suffix == "MiB") return n << 20;
  if (suffix == "G" || suffix == "g" || suffix == "GiB") return n << 30;
  THROW_RT("Invalid size: " << s);
}

}

int zstd(int argc, const char * const *argv) {
  TclapHelper tclap(usage);

  TCLAP::UnlabeledValueArg<std::string> path(
      "path", "", true, "", "path", tclap.cmd());
  TCLAP::ValueArg<std::string> output(
      "o", "output", "output", false, "", "path", tclap.cmd());
  TCLAP::ValueArg<int> level(
      "l", "level", "level", false, 9, "int", tclap.cmd());
  TCLAP::ValueArg<unsigned> threads(
      "T", "threads", "threads", false, 1, "int", tclap.cmd());
  TCLAP::ValueArg<std::string> frameSize(
      "f", "frame-size", "frame-size", false, "16M", "size", tclap.cmd());
  TCLAP::ValueArg<int> longWindow(
      "", "long", "long", false, 0, "int", tclap.cmd());
  TCLAP::SwitchArg noCheck("", "no-check", "no-check", tclap.cmd());
  TCLAP::SwitchArg verbose("v", "verbose", "verbose", tclap.cmd());
  TCLAP::SwitchArg au("", "au", "au", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  ZstdCompressOptions opts;
  opts.level = level.getValue();
  opts.threads = threads.getValue();
  if (opts.threads == 0)
    opts.threads = std::max(1u, std::thread::hardware_concurrency());
  opts.frameSize = parseSize(frameSize.getValue());
  opts.longWindowLog = longWindow.getValue();
  opts.checksum = !noCheck.getValue();
  opts.verbose = verbose.getValue();
  opts.auFrames = au.getValue();

  std::string outFile;
  if (output.isSet()) outFile = output.getValue();
  else if (path.getValue() == "-") outFile = "-";
  else outFile = path.getValue() + ".zst";

  return zstdCompressFile(path.getValue(), outFile, opts);
}

}
