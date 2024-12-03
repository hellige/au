#include "main.h"
#include "AuOutputHandler.h"
#include "JsonOutputHandler.h"
#include "GrepHandler.h"
#include "StreamDetection.h"
#include "TclapHelper.h"
#include "TimestampPattern.h"
#include "au/AuDecoder.h"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <tclap/SwitchArg.h>

namespace au {

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
  // clang hates bitwise-or of booleans. casting one to int is the recommended
  // way to insist that this is really what i want.
  return static_cast<int>(setSignedPattern(pattern, intPat)) |
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

bool setAtomPattern(Pattern &pattern, std::string &atomPat) {
  if (atomPat == "true") {
    pattern.atomPattern = Pattern::Atom::True;
    return true;
  }
  if (atomPat == "false") {
    pattern.atomPattern = Pattern::Atom::False;
    return true;
  }
  if (atomPat == "null") {
    pattern.atomPattern = Pattern::Atom::Null;
    return true;
  }
  return false;
}

bool setTimestampPattern(Pattern &pattern, const std::string &tsPat) {
  pattern.timestampPattern = parseFlexPattern(tsPat);
  return pattern.timestampPattern.has_value();
}

int grepFile(Pattern &pattern,
             const std::string &fileName,
             bool encodeOutput,
             bool asciiLog,
             bool compressed,
             const std::optional<std::string> &indexFile) {
  auto source = detectSource(fileName, indexFile, compressed);

  if (asciiLog) {
    if (isAuFile(*source)) {
      std::cerr << fileName << " appears to be au-encoded. -l is unlikely to"
        << " to do anything useful here!" << std::endl;
      return 1;
    }
    return AsciiGrepper(pattern, *source).doGrep();
  } else if (isAuFile(*source)) {
    if (encodeOutput) {
      AuOutputHandler handler(
          AU_STR("Encoded by au: grep output from au file "
                 << (fileName == "-" ? "<stdin>" : fileName)));
      return AuGrepper(pattern, *source, handler).doGrep();
    } else {
      JsonOutputHandler handler;
      return AuGrepper(pattern, *source, handler).doGrep();
    }
  } else { // assume file is json
    if (encodeOutput) {
      std::cerr << fileName << " appears to be json. au-encoded output is not"
        << " yet supported when searching within json" << std::endl;
      return 1;
    } else {
      JsonOutputHandler handler;
      return JsonGrepper(pattern, *source, handler).doGrep();
    }
  }
}

bool isRePattern(std::string_view sv, const TCLAP::SwitchArg &noRegexFlag) {
  // R(...)
  if (!noRegexFlag.isSet() && sv.starts_with("R")) {
    const std::string_view remaining{sv.begin() + 1, sv.end()};
    return remaining.starts_with("(") && remaining.ends_with(")");
  }
  return false;
}

std::unique_ptr<re2::RE2> tryMakeRe(std::string_view sv) {
  const std::string_view actualPattern{sv.begin() + 2, sv.end() - 1};
  auto re = std::make_unique<re2::RE2>(actualPattern, re2::RE2::Quiet);
  if (!re->ok()) {
    std::cerr << "regex failed to compile: " << actualPattern << std::endl;
    std::cerr << "  error: " << re->error() << std::endl;
    return nullptr;
  }
  return re;
}

void usage(const char *cmd) {
  // clang-format off
  std::cout
      << "usage: au " << cmd << " [options] [--] <pattern> <path>...\n"
      << "\n"
      << "  -h --help           show usage and exit\n"
      << "  -e --encode         output au-encoded records rather than json\n"
      << "  -k --key <key>      match pattern only in object values with key <key>\n"
      << "  -o --ordered <key>  like -k, but values for <key> are assumed to to be\n"
      << "                      roughly ordered\n"
      << "  -g --or-greater     match any value equal to or greater than <pattern>\n"
      << "  -l --ascii-log      see below\n"
      << "  -i --integer        match <pattern> with integer values\n"
      << "  -d --double         match <pattern> with double-precision float values\n"
      << "  -t --timestamp      match <pattern> with timestamps: format is\n"
      << "                      2018-03-27T18:45:00.123456789 or any prefix thereof\n"
      << "                      2018-03-27T18:45:00.123, 2018-03-27T18:4, 2018-03, etc.\n"
      << "  -a --atom           match <pattern> only with atomic literals:\n"
      << "                      true, false, null\n"
      << "  -s --string         match <pattern> with string values\n"
      << "  -u --substring      match <pattern> as a substring of string values\n"
      << "                      implies -s, not compatible with -i/-d\n"
      << "  -m --matches <n>    show only the first <n> matching records\n"
      << "  -B --before <n>     show <n> records of context before each match\n"
      << "  -A --after <n>      show <n> records of context after each match\n"
      << "  -C --context <n>    equivalent to -A n -B n\n"
      << "  -F --follow-context print records following match until first explicitly\n"
      << "                      non-matching record (i.e., record with matching key\n"
      << "                      but non-matching value)\n"
      << "  -c --count          print count of matching records per file\n"
      << "  -r --no-regex       explicitly disable regex matching for all arguments,\n"
      << "                      even if they look like R(...)\n"
      << "  -x --index <path>   use gzip index in <path> (only for zgrep)\n"
      << "\n"
      << "  Timestamps may be specified without a date (e.g., 18:45:00.123), in which \n"
      << "  case the first few records of the stream will be scanned for timestamp matches.\n"
      << "  If a match is found, the pattern date will be set from the first matching\n"
      << "  timestamp. If the resulting timestamp is prior to the start of the file, the\n"
      << "  date will be incremented. This provides a reasonable default for log files\n"
      << "  which span less than twenty-four hours.\n"
      << "\n"
      << "  When -l is specified, the input files are assumed to be plain ASCII log files\n"
      << "  (rather than JSON or au-encoded), possibly gzipped, with a timestamp at the\n"
      << "  beginning of each line. <pattern> is expected to be a timestamp (or prefix\n"
      << "  thereof, as with -t). Files are binary searched for lines with timestamps\n"
      << "  matching <pattern>. Most output-controlling arguments (e.g., -m, -F, -C, -c)\n"
      << "  are accepted in combination with -l.\n"
      << "\n"
      << "  Regular Expressions:\n"
      << "    Most string patterns support regular expression mode. To enable, specify the\n"
      << "    string in the form \"R(...)\", where ... can be any regular expression. For\n"
      << "    example, the following could be used to ignore case while matching a value:\n"
      << "\n"
      << "      au " << cmd << " \"R((?i)somevalue)\" <path>...\n"
      << "\n"
      << "    Note that while -o/--ordered supports a regex key, the corresponding \n"
      << "    <pattern> must not be a regular expression. The same is true for a <pattern>\n"
      << "    when -g/--or-greater is specified.\n"
      << "\n"
      << "    By default, a regex pattern must match the entire string. -u/--substring\n"
      << "    can be used to only match part of the string. This utility uses the \"re2\"\n"
      << "    library for matching, so consult their documentation for specifics about\n"
      << "    supported syntax."
      ;
  // clang-format on
}

int grepCmd(int argc, const char *const *argv, bool compressed) {
  TclapHelper tclap([compressed]() { usage(compressed ? "zgrep" : "grep"); });

  TCLAP::ValueArg<std::string> key(
      "k", "key", "key", false, "", "string", tclap.cmd());
  TCLAP::ValueArg<std::string> ordered(
      "o", "ordered", "ordered", false, "", "string", tclap.cmd());
  TCLAP::ValueArg<uint32_t> context(
      "C", "context", "context", false, 0, "uint32_t", tclap.cmd());
  TCLAP::ValueArg<uint32_t> before(
      "B", "before", "before", false, 0, "uint32_t", tclap.cmd());
  TCLAP::ValueArg<uint32_t> after(
      "A", "after", "after", false, 0, "uint32_t", tclap.cmd());
  TCLAP::ValueArg<uint32_t> matches(
      "m", "matches", "matches", false, 0, "uint32_t", tclap.cmd());
  TCLAP::ValueArg<std::string> index(
      "x", "index", "index", false, "", "string", tclap.cmd());
  TCLAP::SwitchArg orGreater("g", "or-greater", "or-greater", tclap.cmd());
  TCLAP::SwitchArg followContext(
      "F", "follow-context", "follow-context", tclap.cmd());
  TCLAP::SwitchArg asciiLog("l", "ascii-log", "ascii-log", tclap.cmd());
  TCLAP::SwitchArg encode("e", "encode", "encode", tclap.cmd());
  TCLAP::SwitchArg count("c", "count", "count", tclap.cmd());
  TCLAP::SwitchArg matchAtom("a", "atom", "atom", tclap.cmd());
  TCLAP::SwitchArg matchInt("i", "integer", "integer", tclap.cmd());
  TCLAP::SwitchArg matchTimestamp("t", "timestamp", "timestamp", tclap.cmd());
  TCLAP::SwitchArg matchDouble("d", "double", "double", tclap.cmd());
  TCLAP::SwitchArg matchString("s", "string", "string", tclap.cmd());
  TCLAP::SwitchArg matchSubstring("u", "substring", "substring", tclap.cmd());
  TCLAP::SwitchArg noRegex("r", "no-regex", "no-regex", tclap.cmd());
  TCLAP::UnlabeledValueArg<std::string> pat(
      "pattern", "", true, "", "pattern", tclap.cmd());
  TCLAP::UnlabeledMultiArg<std::string> fileNames(
      "path", "", false, "path", tclap.cmd());

  if (!tclap.parse(argc, argv)) return 1;

  {
    auto n = 0;
    if (key.isSet()) n++;
    if (ordered.isSet()) n++;
    if (asciiLog.isSet()) n++;
    if (n > 1) {
      std::cerr << "only one of -k, -o or -l may be specified." << std::endl;
      return 1;
    }
  }

  Pattern pattern;

  if (key.isSet()) {
    if (isRePattern(key.getValue(), noRegex)) {
      pattern.keyPattern = tryMakeRe(key.getValue());
      if (!pattern.keyPattern)
        return 1;
    } else {
      pattern.keyPattern = key.getValue();
    }
  }
  if (ordered.isSet()) {
    if (isRePattern(ordered.getValue(), noRegex)) {
      pattern.keyPattern = tryMakeRe(ordered.getValue());
      if (!pattern.keyPattern)
        return 1;
    } else {
      pattern.keyPattern = ordered.getValue();
    }
    pattern.bisect = true;
  }
  if (asciiLog.isSet()) {
    pattern.bisect = true;
  }

  if (orGreater.isSet()) {
    pattern.matchOrGreater = true;
  }
  if (matches.isSet()) pattern.numMatches = matches.getValue();

  bool explicitTimestampMatch = asciiLog.isSet() || matchTimestamp.isSet();
  bool explicitStringMatch = matchString.isSet() || matchSubstring.isSet();
  bool numericMatch = matchInt.isSet() || matchDouble.isSet()
                      || matchTimestamp.isSet() || matchAtom.isSet();
  bool defaultMatch = !(numericMatch || explicitStringMatch);

  if (matchSubstring.isSet() && numericMatch) {
    std::cerr << "-u (substring search) is not compatible with -i/-d/-t/-a."
              << std::endl;
    return 1;
  }

  const bool patIsRegex = isRePattern(pat.getValue(), noRegex);
  if (patIsRegex && ordered.isSet()) {
    std::cerr << "Pattern for -o/--ordered cannot be regex" << std::endl;
    return 1;
  }
  if (patIsRegex && pattern.matchOrGreater) {
    std::cerr << "Pattern for -g/--or-greater cannot be regex" << std::endl;
    return 1;
  }

  // by default, we'll try to match anything, but won't be upset if the
  // pattern fails to parse as any particular thing...

  if (defaultMatch || explicitStringMatch) {
    if (patIsRegex) {
      auto re = tryMakeRe(pat.getValue());
      if (!re) return 1;
      pattern.strPattern =
          Pattern::StrPattern{std::move(re), !matchSubstring.isSet()};
    } else {
      pattern.strPattern =
          Pattern::StrPattern{pat.getValue(), !matchSubstring.isSet()};
    }
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

  if (defaultMatch || explicitTimestampMatch) {
    bool success = setTimestampPattern(pattern, pat.getValue());
    if (!success && explicitTimestampMatch) {
      std::cerr << "-t/-l specified, but pattern '"
                << pat.getValue() << "' is not a date/time."
                << std::endl;
      return 1;
    }
  }

  if (defaultMatch || matchAtom.isSet()) {
    bool success = setAtomPattern(pattern, pat.getValue());
    if (!success && matchAtom.isSet()) {
      std::cerr << "-a specified, but pattern '"
                << pat.getValue() << "' is not true, false or null."
                << std::endl;
      return 1;
    }
  }

  if (context.isSet())
    pattern.beforeContext = pattern.afterContext = context.getValue();
  if (before.isSet())
    pattern.beforeContext = before.getValue();
  if (after.isSet())
    pattern.afterContext = after.getValue();
  if (followContext.isSet())
    pattern.forceFollow = true;

  pattern.count = count.isSet();

  std::optional<std::string> indexFile;
  if (index.isSet()) indexFile = index.getValue();

  if (fileNames.getValue().empty()) {
    return grepFile(pattern, "-", encode.isSet(), asciiLog.isSet(), compressed,
                    indexFile);
  } else {
    for (auto &f : fileNames) {
      auto result =
          grepFile(pattern, f, encode.isSet(), asciiLog.isSet(), compressed,
                   indexFile);
      if (result) return result;
    }
  }

  return 0;
}
}

int grep(int argc, const char * const *argv) {
  return grepCmd(argc, argv, false);
}

int zgrep(int argc, const char * const *argv) {
  return grepCmd(argc, argv, true);
}

}
