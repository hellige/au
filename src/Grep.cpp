#include "AuDecoder.h"

#include "tclap/CmdLine.h"

#include "JsonHandler.h"
#include "GrepHandler.h"

int grep(int argc, char **argv) {
  Dictionary dictionary;
  JsonHandler jsonHandler(dictionary);

  try {
    TCLAP::CmdLine cmd("Grep sub-command", ' ', "1", true);
    TCLAP::UnlabeledValueArg<std::string> subCmd("subCmd", "Must be \"grep\"",
                                                 true, "grep", "string");
    TCLAP::MultiArg<std::string> key("k", "key", "Key to search for",
                                     false, "string");
    TCLAP::MultiArg<uint64_t> uInt("u", "uint", "Unsigned integer",
                                   false, "uint64_t");
    TCLAP::MultiArg<int64_t> sInt("s", "sint", "Signed integer",
                                  false, "int64_t");
    TCLAP::MultiArg<std::string> str("f", "full", "Full string",
                                     false, "string");
    TCLAP::UnlabeledMultiArg<std::string> fileNames("fileNames", "Au files",
                                                    false, "FileName");

    cmd.add(subCmd);
    cmd.add(key);
    cmd.add(uInt);
    cmd.add(sInt);
    cmd.add(str);
    cmd.add(fileNames);
    cmd.parse(argc, argv);

    
    GrepHandler<JsonHandler> grepHandler(
        dictionary, jsonHandler, key.getValue(), uInt.getValue(), sInt.getValue(), str.getValue());
    RecordHandler<decltype(grepHandler)> recordHandler(dictionary, grepHandler);

    if (fileNames.getValue().empty()) {
      std::cerr << "Grepping stdin\n";
      AuDecoder("-").decode(recordHandler);
    } else {
      for (auto &f : fileNames.getValue()) {
        std::cerr << "Grepping " << f << "\n";
        AuDecoder(f).decode(recordHandler);
      }
    }
  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
  }

  return 0;
}
