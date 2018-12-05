#pragma once

#include "au/AuDecoder.h"
#include "Dictionary.h"

#include <list>

namespace au {

class DictionaryBuilder : public BaseParser {
  std::list<std::string> newEntries_;
  AuByteSource &source_;
  Dictionary &dictionary_;
  /// A valid dictionary must end before this point
  size_t endOfDictAbsPos_;
  size_t lastDictPos_;

public:
  DictionaryBuilder(AuByteSource &source,
                    Dictionary &dictionary,
                    size_t endOfDictAbsPos)
      : BaseParser(source),
        source_(source),
        dictionary_(dictionary),
        endOfDictAbsPos_(endOfDictAbsPos),
        lastDictPos_(source.pos())
  {}

  /// Builds a complete dictionary or throws if it can't
  void build() {
    while (true) {
      // at the top of this loop, we know source_.pos() points to the
      // beginning of a dictionary entry which is NOT currently in any
      // dict. if the backref of the original record pointed into a known
      // dictionary, we wouldn't have called this function. the 'A' branch
      // of this function maintains the invariant: we bail out when the next
      // link in the backref chain points to a valid dict.
      auto insertionPoint = newEntries_.begin();
      auto sor = source_.pos();
      auto marker = source_.next();
      if (marker.isEof()) THROW_RT("Reached EoF while building dictionary");
      switch (marker.charValue()) {
        case 'A': {
          auto prevDictRel = readBackref();
          if (prevDictRel > sor)
            THROW_RT("Dict before start of file");

          while (source_.peek() != marker::RecordEnd) {
            StringBuilder sb(endOfDictAbsPos_ - source_.pos() - 1);
            parseFullString(sb);
            newEntries_.emplace(insertionPoint, sb.str());
          }
          term();

          auto prevDictAbsPos = sor - prevDictRel;
          if (auto *dict = dictionary_.search(prevDictAbsPos)) {
            if (prevDictAbsPos != dict->lastDictPos_) {
              THROW_RT("something wrong, should've hit end of dict exactly: "
                       << prevDictAbsPos << " vs " << dict->lastDictPos_);
            }

            populate(*dict);
            return;
          }

          source_.seek(sor - prevDictRel);
          break;
        }
        case 'C': {
          parseFormatVersion();
          term();

          // always clear the dictionary. by the invariant above, it must
          // not be a known dictionary so there's no need to check whether it
          // already exists.
          populate(dictionary_.clear(sor));
          return;
        }
        default:
          THROW_RT("Failed to build full dictionary. Found 0x"
                       << std::hex
                       << marker.uint8Value() << " at 0x"
                       << sor
                       << std::dec << ". Expected 'A' (0x41) or 'C' (0x43).");
      }
    }
  }

private:
  void populate(Dictionary::Dict &dict) const {
    for (auto &word : newEntries_)
      dict.add(lastDictPos_, std::string_view(word.c_str(), word.length()));
  }
};

/** This handler simply checks that the value we're unpacking doesn't go on past
 * the expected end of the value record. If we start decoding an endless string
 * of T's, we don't want to wait until the whole "record" has been unpacked
 * before coming up for air and validating the length. */
class ValidatingHandler : public NoopValueHandler {
  const Dictionary::Dict &dictionary_;
  AuByteSource &source_;
  size_t absEndOfValue_;

public:
  ValidatingHandler(const Dictionary::Dict &dictionary,
                    AuByteSource &source,
                    size_t absEndOfValue)
      : dictionary_(dictionary), source_(source), absEndOfValue_(absEndOfValue)
  {}

  void onObjectStart() override { checkBounds(); }
  void onObjectEnd() override { checkBounds(); }
  void onArrayStart() override { checkBounds(); }
  void onArrayEnd() override { checkBounds(); }
  void onNull(size_t) override { checkBounds(); }
  void onBool(size_t, bool) override { checkBounds(); }
  void onInt(size_t, int64_t) override { checkBounds(); }
  void onUint(size_t, uint64_t) override { checkBounds(); }
  void onDouble(size_t, double) override { checkBounds(); }
  void onTime(size_t, std::chrono::system_clock::time_point) override {
    checkBounds();
  }

  void onDictRef(size_t, size_t dictIdx) override {
    if (dictIdx >= dictionary_.size()) {
      THROW_RT("Invalid dictionary index");
    }
    checkBounds();
  }

  void onStringStart(size_t, size_t len) override {
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

class TailHandler : public BaseParser {
  Dictionary &dictionary_;
  AuByteSource &source_;

public:
  TailHandler(Dictionary &dictionary, AuByteSource &source)
      : BaseParser(source), dictionary_(dictionary), source_(source) {}

  template <typename OutputHandler>
  void parseStream(OutputHandler &handler) {
    if (!sync()) {
      std::cerr
          << "Unable to find the start of a valid value record. "
             "Consider starting earlier in the file. See the -b option.\n";
      return;
    }

    // At this point we should have a full/valid dictionary and be positioned
    // at the start of a value record.
    AuRecordHandler<OutputHandler> recordHandler(dictionary_, handler);
    RecordParser<decltype(recordHandler)>(source_, recordHandler).parseStream();
  }

  bool sync() {
    while (true) {
      size_t sor = source_.pos();
      try {
        const char marker[] = {marker::RecordEnd, '\n', 'V', 0};
        if (!source_.seekTo(marker)) {
          return false;
        }
        term();
        sor = source_.pos();
        expect('V');
        auto backDictRef = readBackref();
        if (backDictRef > sor) {
          THROW_RT("Back dictionary reference is before the start of the file. "
                   "Current absolute position: " << sor << " backDictRef: "
                                                 << backDictRef);
        }

        if (!dictionary_.search(sor - backDictRef)) {
          source_.seek(sor - backDictRef);
          DictionaryBuilder builder(source_, dictionary_, sor);
          builder.build();
          // We seem to have a complete dictionary. Let's try validating this val.
          source_.seek(sor);
          expect('V');
          if (backDictRef != readBackref()) {
            THROW_RT("Read different value 2nd time!");
          }
        }

        auto valueLen = readVarint();
        auto startOfValue = source_.pos();

        auto &dict = dictionary_.findDictionary(sor, backDictRef);
        ValidatingHandler validatingHandler(
            dict, source_, startOfValue + valueLen);
        ValueParser<ValidatingHandler> valueValidator(
            source_, validatingHandler);
        valueValidator.value();
        term();
        if (valueLen != source_.pos() - startOfValue) {
          THROW_RT("Length doesn't match. Expected: " << valueLen << " actual "
                                                      << source_.pos() - startOfValue);
        }

        // We seem to have a good value record. Reset stream to start of record.
        source_.seek(sor);
        return true; // Sync was successful
      } catch (std::exception &e) {
        std::cerr << "Ignoring exception while synchronizing start of tailing "
                     "(attempted start-of-record: " << sor << "): "
                  << e.what() << "\n";
        source_.seek(sor + 1);
      }
    };
  }
};

}
