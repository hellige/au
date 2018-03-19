#include "main.h"
#include "AuDecoder.h"
#include "JsonOutputHandler.h"
#include "GrepHandler.h"

#include "tclap/CmdLine.h"

#include <cstdlib>

namespace {

bool setSignedPattern(Pattern &pattern, std::string &intPat) {
  const char *str = intPat.c_str();
  char *end;
  errno = 0;
  int64_t val = strtoll(str, &end, 10);
  if (errno == ERANGE) return false;
  if (end != str + intPat.size()) return false;
  pattern.intPattern = val;
  return true;
}

bool setUnsignedPattern(Pattern &pattern, std::string &intPat) {
  const char *str = intPat.c_str();
  char *end;
  errno = 0;
  uint64_t val = strtoull(str, &end, 10);
  if (errno == ERANGE) return false;
  if (!isdigit(*str)) return false; // don't allow negatives
  if (end != str + intPat.size()) return false;
  pattern.uintPattern = val;
  return true;
}

bool setIntPattern(Pattern &pattern, std::string &intPat) {
  return setSignedPattern(pattern, intPat) |
    setUnsignedPattern(pattern, intPat);
}

bool setDoublePattern(Pattern &pattern, std::string &intPat) {
  const char *str = intPat.c_str();
  char *end;
  errno = 0;
  double val = strtod(str, &end);
  if (errno == ERANGE) return false;
  if (!isdigit(*str)) return false; // don't allow negatives
  if (end != str + intPat.size()) return false;
  pattern.doublePattern = val;
  return true;
}

void usage() {
  std::cout
      << "usage: au grep [options] [--] <pattern> <path>...\n"
      << "\n"
      << "  -h --help        show usage and exit\n"
      << "  -k --key <key>   match pattern only in object values with key <key>\n"
      << "  -i --integer     match <pattern> with integer values\n"
      << "  -d --double      match <pattern> with double-precision float values\n"
      << "  -s --string      match <pattern> with string values\n"
      << "  -u --substring   match <pattern> as a substring of string values\n"
      << "                   implies -s, not compatible with -i/-d\n"
      << "  -i --integer     match <pattern> with integer values\n";
}

struct UsageVisitor : public TCLAP::Visitor {
  void visit() override {
    usage();
    exit(0);
  };
};

class GrepOutput : public TCLAP::StdOutput {
public:
  void failure(TCLAP::CmdLineInterface &, TCLAP::ArgException &e) override {
    std::cerr << e.error() << std::endl;
    ::usage();
    exit(1);
  }

  void usage(TCLAP::CmdLineInterface &) override {
    ::usage();
  }
};

}

// TODO teach grep to recognize null/true/false when appropriate
// TODO teach grep how to do -C/-A/-B. keep a running position of sor for nth record back and n "force dump" records forward, rewind and dump on match, etc
//      dumping up to current would naturally move running position forward to rewind would naturally still work.
// TODO teach grep -c

int grep(int argc, const char * const *argv) {
  Dictionary dictionary;
  JsonOutputHandler jsonHandler(dictionary);

  try {
    UsageVisitor usageVisitor;
    TCLAP::CmdLine cmd("", ' ', "", false);
    TCLAP::UnlabeledValueArg<std::string> subCmd(
        "grep", "grep", true, "grep", "command", cmd);
    TCLAP::SwitchArg help("h", "help", "help", cmd, false, &usageVisitor);

    TCLAP::ValueArg<std::string> key(
        "k", "key", "key", false, "", "string", cmd);
    TCLAP::SwitchArg matchInt("i", "integer", "integer", cmd);
    TCLAP::SwitchArg matchDouble("d", "double", "double", cmd);
    TCLAP::SwitchArg matchString("s", "string", "string", cmd);
    TCLAP::SwitchArg matchSubstring("u", "substring", "substring", cmd);
    TCLAP::UnlabeledValueArg<std::string> pat(
        "pattern", "", true, "", "pattern", cmd);
    TCLAP::UnlabeledMultiArg<std::string> fileNames(
        "fileNames", "", false, "filename", cmd);

    GrepOutput output;
    cmd.setOutput(&output);
    cmd.parse(argc, argv);

    Pattern pattern;
    if (key.isSet()) pattern.keyPattern = key.getValue();

    bool explicitStringMatch = matchString.isSet() || matchSubstring.isSet();
    bool numericMatch = matchInt.isSet() || matchDouble.isSet();
    bool defaultMatch = !(numericMatch || explicitStringMatch);

    if (matchSubstring.isSet() && numericMatch) {
      std::cerr << "-u (substring search) is not compatible with -i/-d."
                << std::endl;
      return 1;
    }

    // by default, we'll try to match anything, but won't be upset if the
    // pattern fails to parse as any particular thing...

    if (defaultMatch || explicitStringMatch) {
      pattern.strPattern = Pattern::StrPattern{
        pat.getValue(), !matchSubstring.isSet()};
    }

    if (defaultMatch || matchInt.isSet()) {
      bool success = setIntPattern(pattern, pat.getValue());
      if (!success && matchInt.isSet()) {
        std::cerr << "-i specified, but pattern '"
                  << pat.getValue() << "' is not an integer." << std::endl;
        return 1;
      }
    }

    if (defaultMatch || matchDouble.isSet()) {
      bool success = setDoublePattern(pattern, pat.getValue());
      if (!success && matchDouble.isSet()) {
        std::cerr << "-d specified, but pattern '"
                  << pat.getValue() << "' is not a double-precision number."
                  << std::endl;
        return 1;
      }
    }

    GrepHandler<JsonOutputHandler> grepHandler(
        dictionary, jsonHandler, std::move(pattern));
    AuRecordHandler<decltype(grepHandler)> recordHandler(dictionary, grepHandler);

    if (fileNames.getValue().empty()) {
      AuDecoder("-").decode(recordHandler, false);
    } else {
      for (auto &f : fileNames.getValue()) {
        AuDecoder(f).decode(recordHandler, false);
      }
    }
  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  return 0;
}

