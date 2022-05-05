#include "au/AuDecoder.h"
#include "au/FileByteSource.h"
#include "AuRecordHandler.h"
#include "JsonOutputHandler.h"

#include "gtest/gtest.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace au {

namespace {

class Decoder {
  std::string filename_;

public:
  Decoder(const std::string &filename)
      : filename_(filename) {}

  template<typename H>
  void decode(H &handler) const {
    FileByteSourceImpl source(filename_);
    try {
      RecordParser<H>(source, handler).parseStream();
    } catch (parse_error &e) {
      std::cerr << e.what() << std::endl;
    }
  }
};

}

TEST(AuDecoderTestCases, doesntCrashOnCases) {
  for (auto &p: fs::directory_iterator("cases")) {
    SCOPED_TRACE(std::string("Processing ") + p.path().c_str());
    try {
      Decoder auDecoder(p.path());
      Dictionary dictionary;
      JsonOutputHandler valueHandler;
      AuRecordHandler<JsonOutputHandler> recordHandler(dictionary,
                                                       valueHandler);
      auDecoder.decode(recordHandler);
    } catch (const std::exception &) {}
  }
}

}
