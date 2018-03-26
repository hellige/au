#include "main.h"
#include "JsonOutputHandler.h"
#include "Tail.h"

#include <tclap/CmdLine.h>

int tail(int argc, const char *const *argv) {
  Dictionary dictionary;
  JsonOutputHandler jsonHandler;

  try {
    TCLAP::CmdLine cmd("Tail sub-command", ' ', AU_VERSION, true);
    TCLAP::UnlabeledValueArg<std::string> subCmd("subCmd", "Must be \"tail\"",
                                                 true, "tail", "command", cmd);
    TCLAP::SwitchArg follow("f", "follow", "Output appended data as file grows",
                            cmd, false);
    // Offset in bytes so we can fine-tune the starting point for test purposes.
    TCLAP::ValueArg<size_t> startOffset("b", "bytes",
                                        "output last b bytes (default 1024)",
                                        false, 5 * 1024, "integer", cmd);
    TCLAP::UnlabeledValueArg<std::string> fileName("fileNames", "Au files",
                                                    true, "", "FileName", cmd);
    cmd.parse(argc, argv);

    if (fileName.getValue().empty() || fileName.getValue() == "-") {
      std::cerr << "Tailing stdin not supported\n";
    } else {
      TailByteSource source(fileName, follow);
      source.tail(startOffset);
      TailHandler tailHandler(dictionary, source);
      tailHandler.parseStream(jsonHandler);
    }

  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  return 0;
}
