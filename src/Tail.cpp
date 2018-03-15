#include "main.h"
#include "AuDecoder.h"

#include "tclap/CmdLine.h"
#include <sys/stat.h>

#include "JsonHandler.h"


class TailByteSource : public FileByteSource {
public:
  explicit TailByteSource(const std::string &fname, bool follow,
                          size_t startOffset, size_t bufferSizeInK = 16)
      : FileByteSource(fname, follow, bufferSizeInK)
  {
    tail(startOffset);
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
        skip(buffAvail());
        read(needle.length() - 1);
      }
    }
  }

  void seekTo(size_t absPos) {
    if (absPos < pos()) {
      seek(absPos);
    } else if (absPos > pos()) {
      skip(absPos - pos());
    }
  }

  /// Seek to length bytes from the end of the stream
  void tail(size_t length) {
    struct stat stat;
    if (auto res = fstat(fd_, &stat); res < 0) {
      THROW_RT("failed to stat file: " << strerror(errno));
    }

    length = std::min<size_t>(length, stat.st_size);
    auto startPos = stat.st_size - length;

    auto pos = lseek(fd_, static_cast<off_t>(startPos), SEEK_SET);
    if (pos < 0) {
      THROW_RT("failed to seek to tail: " << strerror(errno));
    }
    cur_ = limit_ = buf_;
    pos_ = static_cast<size_t>(pos);
    if (!read(0))
      THROW_RT("failed to read from start of tail location");
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
  /// A valid dictionary must end before this point
  size_t endOfDictAbsPos_;
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
  DictionaryBuilder(TailByteSource &source, size_t endOfDictAbsPos)
      : BaseParser(source), source_(source), dictAbsPos_(source.pos()),
        endOfDictAbsPos_(endOfDictAbsPos), lastDictPos_(source.pos())
  {}

  void populate(Dictionary &dict) {
    dict.clear(lastDictPos_);
    for (auto &word : dictionary_) {
      dict.add(lastDictPos_, std::string_view(word.c_str(), word.length()));
    }
  }

  /// Builds a complete dictionary or throws if it can't
  void build() {
    bool complete = false;
    while (!complete) {
      auto insertionPoint = dictionary_.begin();

      auto marker = source_.next();
      if (marker.isEof()) THROW_RT("Reached EoF while building dictionary");
      switch (marker.charValue()) {
        case 'A': {
          auto prevDictRel = readVarint();
          while (source_.peek() == 'S') {
            expect('S');
            StringBuilder sb(endOfDictAbsPos_ - source_.pos());
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
          complete = true;
          break;
        default:
          THROW_RT("Failed to build full dictionary. Found 0x" << std::hex
                   << (int)marker.charValue() << " at 0x" << dictAbsPos_
                   << std::dec << ". Expected 'A' (0x41) or 'C' (0x43).");
      }
    }
  }
};

/** This handler simply checks that the value we're unpacking doesn't go on past
 * the expected end of the value record. If we start decoding an endless string
 * of T's, we don't want to wait until the whole "record" has been unpacked
 * before coming up for air and validating the length. */
class ValidatingHandler : public NoopValueHandler {
  Dictionary &dictionary_;
  FileByteSource &source_;
  size_t absEndOfValue_;

public:
  ValidatingHandler(Dictionary &dictionary, FileByteSource &source,
                    size_t absEndOfValue)
      : dictionary_(dictionary), source_(source), absEndOfValue_(absEndOfValue)
  {}

  void onObjectStart() override { checkBounds(); }
  void onObjectEnd() override { checkBounds(); }
  void onArrayStart() override { checkBounds(); }
  void onArrayEnd() override { checkBounds(); }
  void onNull() override { checkBounds(); }
  void onBool(bool) override { checkBounds(); }
  void onInt(int64_t) override { checkBounds(); }
  void onUint(uint64_t) override { checkBounds(); }
  void onDouble(double) override { checkBounds(); }

  void onDictRef(size_t dictIdx) override {
    if (dictIdx >= dictionary_.size()) {
      THROW_RT("Invalid dictionary index");
    }
    checkBounds();
  }

  void onStringStart(size_t len) override {
    if (source_.pos() + len > absEndOfValue_) {
      THROW_RT("String is too long.");
    }
    checkBounds();
  }

  void onStringFragment(std::string_view) override { checkBounds(); }

private:
  void checkBounds() {
    if (source_.pos() > absEndOfValue_) {
      THROW_RT("Invalid value record structure/length.");
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
              std::string fileName, bool follow, size_t startOffset)
      : BaseParser(source_), outputHandler_(handler), dictionary_(dictionary),
        source_(fileName, follow, startOffset)
  {
    // TODO: What assumptions do we make about AU_FORMAT_VERSION we're tailing?
    sync();

    // At this point we should have a full/valid dictionary and be positioned
    // at the start of a value record.
    RecordHandler<OutputHandler> recordHandler(dictionary_, outputHandler_);
    RecordParser<decltype(recordHandler)>(source_, recordHandler).parseStream();
  }

  bool sync() {
    while (true) {
      size_t sor = source_.pos();
      try {
        source_.seekTo("E\nV");
        sor = source_.pos() + 2;
        term();
        expect('V');
        auto backDictRef = readVarint();
        if (backDictRef > sor) {
          THROW_RT("Back dictionary reference is before the start of the file. "
                   "Current absolute position: " << sor << " backDictRef: "
                   << backDictRef);
        }
        source_.seek(sor - backDictRef);
        DictionaryBuilder builder(source_, sor);
        builder.build();

        // We seem to have a complete dictionary. Let's try validating this val.
        source_.skip(sor - source_.pos());
        expect('V');
        if (backDictRef != readVarint()) {
          THROW_RT("Read different value 2nd time!");
        }
        auto valueLen = readVarint();
        auto startOfValue = source_.pos();

        Dictionary validatingDict;
        builder.populate(validatingDict);

        ValidatingHandler validatingHandler(validatingDict, source_,
                                            startOfValue + valueLen);
        ValueParser<ValidatingHandler> valueValidator(source_,
                                                      validatingHandler);
        valueValidator.value();
        term();
        if (valueLen != source_.pos() - startOfValue) {
          THROW_RT("Length doesn't match. Expected: " << valueLen << " actual "
                   << source_.pos() - startOfValue);
        }

        // We seem to have a good value record. Reset stream to start of record.
        source_.seek(sor);
        builder.populate(dictionary_);
        return true; // Sync was successful
      } catch (std::exception &e) {
        std::cerr << "Ignoring exception while synchronizing start of tailing: "
                  << e.what() << "\n";
        source_.seekTo(sor + 1);
      }
    };
  }

protected:
  void term() const {
    expect('E');
    expect('\n');
  }
};

int tail(int argc, const char *const *argv) {
  Dictionary dictionary;
  JsonHandler jsonHandler(dictionary);

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
      TailHandler<JsonHandler> tailHandler(dictionary, jsonHandler,
                                           fileName, follow, startOffset);
    }

  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  return 0;
}
