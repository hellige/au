#pragma once

#include "au/AuByteSource.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace au {

struct NoopValueHandler {
  virtual ~NoopValueHandler() = default;

  virtual void onObjectStart() {}
  virtual void onObjectEnd() {}
  virtual void onArrayStart() {}
  virtual void onArrayEnd() {}
  virtual void onNull([[maybe_unused]] size_t pos) {}
  virtual void onBool([[maybe_unused]] size_t pos, bool) {}
  virtual void onInt([[maybe_unused]] size_t pos, int64_t) {}
  virtual void onUint([[maybe_unused]] size_t pos, uint64_t) {}
  virtual void onDouble([[maybe_unused]] size_t pos, double) {}
  virtual void onTime(
      [[maybe_unused]] size_t pos,
      [[maybe_unused]] std::chrono::system_clock::time_point nanos) {}
  virtual void onDictRef([[maybe_unused]] size_t pos,
                         [[maybe_unused]] size_t dictIdx) {}
  virtual void onStringStart([[maybe_unused]] size_t sov,
                             [[maybe_unused]] size_t length) {}
  virtual void onStringEnd() {}
  virtual void onStringFragment([[maybe_unused]] std::string_view fragment) {}
};

struct NoopRecordHandler {
  virtual ~NoopRecordHandler() = default;

  virtual void onRecordStart([[maybe_unused]] size_t absPos) {}
  virtual void onValue([[maybe_unused]]size_t relDictPos, size_t len,
                       AuByteSource &source) {
    // We would normally hand off to the ValueParser here which will consume len
    source.skip(len);
  }
  virtual void onHeader([[maybe_unused]] uint64_t version,
                        [[maybe_unused]] const std::string &metadata) {}
  virtual void onDictClear() {}
  virtual void onDictAddStart([[maybe_unused]] size_t relDictPos) {}
  virtual void onStringStart([[maybe_unused]] size_t,
                             [[maybe_unused]] size_t strLen) {}
  virtual void onStringEnd() {}
  virtual void onStringFragment([[maybe_unused]] std::string_view fragment) {}
};

}
