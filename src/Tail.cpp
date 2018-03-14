#include "main.h"
#include "AuDecoder.h"

#include "tclap/CmdLine.h"

#include "JsonHandler.h"

int tail(int argc, char **argv) {
  Dictionary dictionary;

  try {
    TCLAP::CmdLine cmd("Tail sub-command", ' ', AU_VERSION, true);
    TCLAP::UnlabeledValueArg<std::string> subCmd("subCmd", "Must be \"tail\"",
                                                 true, "tail", "command", cmd);
    TCLAP::SwitchArg follow("f", "follow", "Output appended data as file grows",
                            cmd, false);
    TCLAP::ValueArg lines("n", "lines", "output last n lines (default 10)",
                          false, 10, "integer", cmd);

    cmd.parse(argc, argv);

  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
    return 1;
  }

  return 0;
}
