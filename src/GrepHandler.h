#pragma once

#include "AuDecoder.h"

#include <algorithm>

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

  // Patterns to search for
  const std::vector<std::string> &pKey_;
  const std::vector<uint64_t> &pUint64_t_;
  const std::vector<int64_t> &pInt64_t_;
  const std::vector<std::string> &pFullStr_;

  bool matched_;

public:
  GrepHandler(Dictionary &dictionary, OutputHandler &handler,
              const std::vector<std::string> &pKey,
              const std::vector<uint64_t> &pUint64_t,
              const std::vector<int64_t> &pInt64_t,
              const std::vector<std::string> &pFullStr)
      : handler_(handler), dictionary_(dictionary),
        pKey_(pKey), pUint64_t_(pUint64_t), pInt64_t_(pInt64_t), pFullStr_(pFullStr),
        matched_(false) {
    // TODO: Sort pattern containers
  }

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

  template<typename C, typename V>
  bool find(const C &container, const V &value) {
    return std::find(container.cbegin(), container.cend(), value) != container.cend();
  }

  void onUint(uint64_t value) {
    if (find(pUint64_t_, value)) matched_ = true;
  }
};
