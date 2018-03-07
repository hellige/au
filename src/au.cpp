#include "AuEncoder.h"
#include "AuDecoder.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace std;

namespace {

constexpr const char *AU_VERSION = "0.1";

constexpr int AU_FORMAT_VERSION = 1; // TODO extract

int canned(int, char**) {
  std::ostringstream os;
  Au au(os);

  au.encode([](AuFormatter &f) { f.map(); });
  au.encode([](AuFormatter &f) { f.map("key1", "value1", "key2", -5000, "keyToIntern3", false); });
  au.encode([](AuFormatter &f) { f.array(6, 1, 0, -7, -2, 5.9, -5.9); });
  au.encode([](AuFormatter &f) { f.array(); });

  auto NaNs = [](AuFormatter &f) { f.array(
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<long double>::quiet_NaN(), // converted to double
      0 / 0.0,
      std::sqrt(-1)
  ); };

  au.encode([&](AuFormatter &f) {
    f.map("NaNs", [&]() {
      NaNs(f);
    });
  });

  //au.encode([](AuFormatter &f) { f.value(-500); f.value(3987); }); // Invalid

  au.clearDictionary(false);
  au.encode([](AuFormatter &f) { f.map("key1", "value1", "key2", -5000, "keyToIntern3", false); });
  au.encode([](AuFormatter &f) {
    f.map("RepeatedVals",
      f.arrayVals([&]() {
        for (auto i = 0; i < 12; ++i) {
          f.value("valToIntern");
        }
      })
    );
  });
  au.encode([](AuFormatter &f) { f.value("valToIntern"); });

  cout << os.str();
  return 0;
}

int version(int, char **) {
  std::cout << "au version " << AU_VERSION
    << " (format version " << AU_FORMAT_VERSION << ")" << std::endl;
  return 0;
}

int usage(std::ostream &os) {
  os << "usage: au [--version] [--help] <command> [args]" << std::endl;
  return 1;
}

int help(int, char **) {
  usage(std::cout);
  std::cout << "\nCommands:\n"
    << "   canned   Dump a canned snippet\n"
    << "   cat      Decode listed files to stdout\n"
    << "   tail     Decode and/or follow file\n"
    << "   grep     Find records matching pattern\n"
    << "   enc      Encode listed files to stdout\n"
    << "   stats    Display file statistics\n";
  return 0;
}

int cat(int argc, char **argv) {
  if (argc == 0) {
    std::cout << "stdin" << std::endl; // TODO decode stdin
  } else {
    for (int i = 0; i < argc; i++) {
      std::string filename(argv[i]);
      if (filename == "-") {
          std::cout << "stdin" << std::endl; // TODO decode stdin
      } else {
        AuDecoder(filename).decode();
      }
    }
  }
  return 0;
}

int todo(int, char **) {
  std::cout << "not yet implemented." << std::endl; // TODO
  return 1;
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
  commands["cat"] = cat;
  commands["canned"] = canned;
  commands["tail"] = todo;
  commands["grep"] = todo;
  commands["enc"] = todo;
  commands["stats"] = todo;

  std::string cmd(argv[1]);
  auto it = commands.find(cmd);
  if (it == commands.end()) {
    std::cerr << "Unknown option or command: " << cmd << std::endl;
    return usage(std::cerr);
  }

  return it->second(argc-2, argv+2);
}
