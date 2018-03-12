#pragma once

#include "AuDecoder.h"

/**
 * This ValueHandler looks for specific patterns, and if the pattern is found,
 * rewinds the data stream to the start of the record, then delegates to another
 * ValueHandler (the OutputHandler) to output the matched record.
 *
 * @tparam OutputHandler A ValueHandler to delegate matching records to.
 */
template <typename OutputHandler>
class GrepHandler : public NoopValueHandler {
  OutputHandler &handler_;
  Dictionary &dictionary_;
  uint64_t pattern_;
  bool matched_;

public:
  GrepHandler(Dictionary &dictionary, OutputHandler &handler, uint64_t pattern)
  : handler_(handler),
    dictionary_(dictionary),
    pattern_(pattern),
    matched_(false) {}

  void onValue(FileByteSource &source) {
    matched_ = false;
    auto sov = source.pos();
    ValueParser<GrepHandler> parser(source, *this);
    parser.value();

    if (matched_) {
      source.seek(sov);
      handler_.onValue(source);
    }
  }

  void onUint(uint64_t value) {
    if (value == pattern_) matched_ = true;
  }
};
