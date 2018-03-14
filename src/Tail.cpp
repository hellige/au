#include "main.h"
#include "AuDecoder.h"

#include "tclap/CmdLine.h"

#include "JsonHandler.h"

template<typename OutputHandler>
class TailHandler : public NoopValueHandler {
  OutputHandler &outputHandler_;
  Dictionary &dictionary_;

public:
  TailHandler(Dictionary &dictionary, OutputHandler &handler)
      : outputHandler_(handler), dictionary_(dictionary)
  {}

  void onValue(FileByteSource &) {}
};

int tail(int argc, char **argv) {
  Dictionary dictionary;
  JsonHandler jsonHandler(dictionary);

  try {
    TCLAP::CmdLine cmd("Tail sub-command", ' ', AU_VERSION, true);
    TCLAP::UnlabeledValueArg<std::string> subCmd("subCmd", "Must be \"tail\"",
                                                 true, "tail", "command", cmd);
    TCLAP::SwitchArg follow("f", "follow", "Output appended data as file grows",
                            cmd, false);
    TCLAP::ValueArg lines("n", "lines", "output last n lines (default 10)",
                          false, 10, "integer", cmd);
    TCLAP::UnlabeledMultiArg<std::string> fileNames("fileNames", "Au files",
                                                    false, "FileName", cmd);
    cmd.parse(argc, argv);

    TailHandler<JsonHandler> tailHandler(dictionary, jsonHandler);
    RecordHandler<decltype(tailHandler)> recordHandler(dictionary, tailHandler);

    if (fileNames.getValue().empty()) {
      std::cerr << "Tailing stdin\n";
      AuDecoder("-").decode(recordHandler);
    } else {
      // TODO: Handle multiple files
      AuDecoder(fileNames.getValue()[0]).decode(recordHandler);
    }

  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
    return 1;
  }

  return 0;
}
