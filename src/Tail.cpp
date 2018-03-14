#include "main.h"
#include "AuDecoder.h"

#include "tclap/CmdLine.h"
#include <sys/stat.h>

#include "JsonHandler.h"


class TailByteSource : public FileByteSource {
public:
  explicit TailByteSource(const std::string &fname, bool follow, size_t bufferSizeInK = 16)
      : FileByteSource(fname, follow, bufferSizeInK)
  {
    if (fname == "-") {
      throw std::runtime_error("Tail on STDIN not supported");
    }
    tail(1 * 1024);
  }

  void seekTo(std::string_view needle) {
    while (true) {
      auto found = memmem(cur_, buffAvail(), needle.data(), needle.length());
      if (found) {
        size_t offset = static_cast<char *>(found) - cur_;
        pos_ += offset;
        cur_ += offset;
        return;
      } else {
        size_t offset = buffAvail();
        pos_ += offset;
        cur_ += offset;
        read(needle.length());
      }
    }
  }

  /// Seek to length bytes from the end of the stream
  void tail(size_t length) {
    struct stat stat;
    if (auto res = fstat(fd_, &stat); res < 0) {
      THROW_RT("failed to stat file");
    }

    length = std::min<size_t>(length, stat.st_size);
    auto startPos = stat.st_size - length;

    auto pos = lseek(fd_, static_cast<off_t>(startPos), SEEK_SET);
    if (pos < 0) {
      THROW_RT("failed to see to tail: " << strerror(errno));
    }
    cur_ = limit_ = buf_;
    pos_ = static_cast<size_t>(pos);
    if (!read(0))
      THROW_RT("failed to read from new location");
  }

protected:
  /// Available to be consumed
  size_t buffAvail() const {
    return static_cast<size_t>(limit_ - cur_);
  }
};

class DictionaryBuilder : public BaseParser {
  std::list<std::string> dictionary_;
  TailByteSource &source_;
  size_t dictAbsPos_;
  size_t endOfDictionary_;
  size_t lastDictPos_;

  struct StringBuilder {
    std::string str_;
    size_t maxLen_;
    StringBuilder(size_t maxLen) : maxLen_(maxLen) {}

    void onStringStart(size_t len) {
      if (len > maxLen_)
        throw std::length_error("String too long");
      str_.reserve(len);
    }
    void onStringFragment(std::string_view frag) {
      str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
    }
    void onStringEnd() {}
  };

public:
  DictionaryBuilder(TailByteSource &source, size_t endOfDictionary)
    : BaseParser(source), source_(source), dictAbsPos_(source.pos()),
      endOfDictionary_(endOfDictionary), lastDictPos_(source.pos())
  {}

  const std::list<std::string> &dictionary() const {
    return dictionary_;
  }

  void populate(Dictionary &dict) {
    dict.clear(lastDictPos_);
    for (auto &word : dictionary_) {
      dict.add(lastDictPos_, std::string_view(word.c_str(), word.length()));
    }
  }

  /// Returns true while dictionary is not complete (if we should continue building)
  void build() {
    bool complete = false;
    while (!complete) {
      auto insertionPoint = dictionary_.begin();

      auto marker = source_.next();
      if (marker.isEof()) THROW_RT("Reached EoF while building dictionary");
      switch (marker.charValue()) {
        case 'A': {
          auto prevDictRel = readVarint();
          // TODO: Would help to know full dict size as of this point so we know
          // when we get to 'C' if we found all the entries.
          while (source_.peek() == 'S') {
            expect('S');
            StringBuilder sb(endOfDictionary_ - source_.pos());
            parseString(sb);
            dictionary_.emplace(insertionPoint, sb.str_);
          };
          if (prevDictRel > dictAbsPos_)
            THROW_RT("Dict before start of file");
          dictAbsPos_ -= prevDictRel;
          source_.seek(dictAbsPos_);
        }
          break;
        case 'C':
          expect('E');
          expect('\n');
          // We have no way of knowing at this point if we found all entries. We
          // can just try parsing and if we don't find an intern string ref we'll
          // know only then that we've been printing garbage.
          complete = true;
          break;
        default:
          THROW_RT("Failed to build full dictionary. Found " << marker.charValue()
              << " at " << dictAbsPos_);
      }
    }
  }
};

class ValidatingHandler : public NoopValueHandler {
  Dictionary &dictionary_;
public:
  ValidatingHandler(Dictionary &dictionary) : dictionary_(dictionary) {}
  void onDictRef(size_t dictIdx) {
    if (dictIdx >= dictionary_.size()) {
      THROW_RT("Invalid dictionary index");
    }
  }
};

template<typename OutputHandler>
class TailHandler : public BaseParser {
  OutputHandler &outputHandler_;
  Dictionary &dictionary_;
  TailByteSource source_;

public:
  TailHandler(Dictionary &dictionary, OutputHandler &handler,
              std::string fileName, bool follow)
      : BaseParser(source_), outputHandler_(handler), dictionary_(dictionary),
        source_(fileName, follow)
  {
    // TODO: What assumptions do we make about AU_FORMAT_VERSION we're tailing?
    sync();
    // At this point we should have a full/valid dictionary and be positioned
    // at the start of a value record.
    RecordHandler<OutputHandler> recordHandler(dictionary_, outputHandler_);
    RecordParser<decltype(recordHandler)>(source_, recordHandler).parseStream();
    // TODO: Make EoF not be an exceptional condition when parsing stream
    //
  }

  bool sync() {
    while (true) {
      try {
        source_.seekTo("E\nV");
        term();
        auto sor = source_.pos();
        expect('V');
        auto backDictRef = readVarint();
        source_.seek(sor - backDictRef);
        DictionaryBuilder builder(source_, sor);
        builder.build();

        // We seem to have a complete dictionary. Let's try validating this val.
        source_.skip(sor - source_.pos());
        expect('V');
        backDictRef = readVarint();
        auto valueLen = readVarint();
        auto startOfValue = source_.pos();
        Dictionary validatingDict;
        builder.populate(validatingDict);
        ValidatingHandler validatingHandler(validatingDict);
        ValueParser<ValidatingHandler> valueValidator(source_, validatingHandler);
        valueValidator.value();
        term();
        if (valueLen != source_.pos() - startOfValue) {
          continue; // Find another start of value candidate marker
        }

        // We seem to have a good value record. Reset stream to start of record.
        source_.seek(sor);
        builder.populate(dictionary_);
        return true; // Sync was successful
      } catch (std::exception &e) {
        // TODO: Some errors (EoF on building dictionary) should be terminal...
        (void)e;
      }
    };
  }

protected:
  void term() const {
    expect('E');
    expect('\n');
  }
};

int tail(int argc, const char * const *argv) {
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
    TCLAP::UnlabeledValueArg<std::string> fileName("fileNames", "Au files",
                                                    true, "", "FileName", cmd);
    cmd.parse(argc, argv);

    //TailHandler<JsonHandler> tailHandler(dictionary, jsonHandler);
    //RecordHandler<decltype(tailHandler)> recordHandler(dictionary, tailHandler);

    if (fileName.getValue().empty()) {
      std::cerr << "File name missing. Tailing stdin not supported\n";
    } else {
      TailHandler<JsonHandler> tailHandler(dictionary, jsonHandler,
                                           fileName, follow);
    }

  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
    return 1;
  }

  return 0;
}
