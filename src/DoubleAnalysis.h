#pragma once

#include "DoubleEncoding.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace au {

namespace {

/** Measures how much compressing doubles would save, and which of three
 * orthogonal mechanisms does the saving:
 *
 *  (a) self-contained per-value tiers, see DoubleEncoding.h
 *  (b) record-local back-reference to an identical earlier double
 *  (c) record-local XOR against the preceding double
 *
 * (b) and (c) are legal only because a value record is always decoded in its
 * entirety from its own beginning. Anything carried across records would break
 * tail, bisect and grep's context rewind. */
class DoubleAnalysis {
public:
  /// Beyond this we stop tracking new entries, but keep counting known ones.
  static constexpr size_t MAX_TRACKED = 1u << 20;
  static constexpr size_t MAX_BACKREF_WINDOW = 256;
  static constexpr size_t MAX_BUCKET = 64;
  static constexpr size_t TOP_N = 20;

private:
  using Tier = doubleenc::Tier;

  struct KeyStats {
    size_t count = 0;
    size_t decimal = 0;
    size_t float32 = 0;
    size_t raw = 0;
    size_t bytesA = 0;
  };

  // (a) per-value tiers
  size_t total_ = 0;
  size_t zero_ = 0, negZero_ = 0, nonFinite_ = 0, float32_ = 0, raw_ = 0;
  std::array<size_t, doubleenc::MAX_SCALE + 1> byScale_{};
  std::array<size_t, 12> sigLen_{};
  std::array<size_t, 9> rawTrailingZeros_{};
  size_t decimalNotWorthIt_ = 0;  //< verifies, but wouldn't be any smaller

  // projected sizes
  size_t bytesToday_ = 0, bytesA_ = 0, bytesAB_ = 0, bytesABC_ = 0;

  // (b) record-local repeats
  size_t repeatOfPrevious_ = 0;
  size_t repeatOfEarlier_ = 0;
  size_t repeatBeyondWindow_ = 0;

  // (c) xor against previous
  std::array<size_t, 9> xorSigBytesRecord_{};
  std::array<size_t, 9> xorSigBytesArray_{};
  size_t xorPairsRecord_ = 0, xorPairsArray_ = 0;

  // record / array shape
  size_t records_ = 0, recordsWithDoubles_ = 0;
  std::array<size_t, MAX_BUCKET + 2> perRecord_{};
  std::array<size_t, MAX_BUCKET + 2> longestRun_{};
  size_t doublesInRuns_ = 0;

  // census
  std::unordered_map<uint64_t, size_t> valueCounts_;
  size_t valuesUntracked_ = 0;
  std::unordered_map<std::string, KeyStats> byKey_;
  size_t keysUntracked_ = 0;

  // per-record scratch, reused so there is no per-record allocation
  std::vector<uint64_t> recordValues_;
  bool havePrev_ = false;
  uint64_t prevBits_ = 0;
  size_t recordLongestRun_ = 0;

  static size_t bucket(size_t n) { return n > MAX_BUCKET ? MAX_BUCKET + 1 : n; }

  static void pct(std::ostream &os, size_t part, size_t whole) {
    char buf[32];
    double p = whole ? 100.0 * static_cast<double>(part)
                       / static_cast<double>(whole)
                     : 0.0;
    snprintf(buf, sizeof(buf), "%5.1f%%", p);
    os << buf;
  }

  static std::string escapeJson(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
      switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    return out;
  }

  template <typename A>
  static void jsonArray(std::ostream &os, const char *name, const A &a) {
    os << "\"" << name << "\":[";
    for (size_t i = 0; i < a.size(); i++) {
      if (i) os << ',';
      os << a[i];
    }
    os << ']';
  }

public:
  /** @param inRun the preceding sibling in the same array was also a double
   * @param runLength length of that run, 1 if this value isn't in one
   * @param prevRunBits preceding double in the run, only valid when inRun */
  void onDouble(const std::string &key, double d,
                bool inRun, size_t runLength, uint64_t prevRunBits) {
    const uint64_t bits = doubleenc::bitsOf(d);
    const auto enc = doubleenc::classify(d);

    total_++;
    bytesToday_ += doubleenc::RAW_BYTES;
    bytesA_ += enc.bytes;

    if (enc.tier != Tier::Decimal && enc.tier != Tier::Zero
        && enc.tier != Tier::NonFinite && enc.tier != Tier::NegZero) {
      unsigned scale;
      int64_t significand;
      if (doubleenc::findDecimal(d, scale, significand)) decimalNotWorthIt_++;
    }

    switch (enc.tier) {
      case Tier::Zero:      zero_++; break;
      case Tier::NegZero:   negZero_++; break;
      case Tier::NonFinite: nonFinite_++; break;
      case Tier::Float32:   float32_++; break;
      case Tier::Raw: {
        raw_++;
        unsigned leading, trailing;
        doubleenc::zeroBytes(bits, leading, trailing);
        rawTrailingZeros_[trailing]++;
        break;
      }
      case Tier::Decimal:
        byScale_[enc.scale]++;
        sigLen_[doubleenc::varintLen(doubleenc::zigzag(enc.significand))]++;
        break;
    }

    // (b) scan backwards so the nearest match wins and the distance stays
    // small, stopping at the window we'd be willing to encode
    unsigned bBytes = enc.bytes;
    const size_t n = recordValues_.size();
    const size_t limit = std::min(n, MAX_BACKREF_WINDOW);
    bool found = false;
    for (size_t back = 0; back < limit; ++back) {
      if (recordValues_[n - 1 - back] != bits) continue;
      if (back == 0) repeatOfPrevious_++; else repeatOfEarlier_++;
      const unsigned cost =
          back == 0 ? 1u : 1u + doubleenc::varintLen(back);
      if (cost < bBytes) bBytes = cost;
      found = true;
      break;
    }
    if (!found && limit < n) {
      // a match the window cost us
      for (size_t i = n - limit; i-- > 0; ) {
        if (recordValues_[i] == bits) { repeatBeyondWindow_++; break; }
      }
    }
    bytesAB_ += bBytes;

    // (c)
    unsigned cBytes = bBytes;
    if (havePrev_) {
      const uint64_t x = bits ^ prevBits_;
      unsigned leading, trailing;
      doubleenc::zeroBytes(x, leading, trailing);
      xorSigBytesRecord_[x ? 8u - leading - trailing : 0u]++;
      xorPairsRecord_++;
      const unsigned cost = doubleenc::xorBytes(x);
      if (cost < cBytes) cBytes = cost;
    }
    if (inRun) {
      const uint64_t x = bits ^ prevRunBits;
      unsigned leading, trailing;
      doubleenc::zeroBytes(x, leading, trailing);
      xorSigBytesArray_[x ? 8u - leading - trailing : 0u]++;
      xorPairsArray_++;
    }
    bytesABC_ += cBytes;

    if (runLength > 1) doublesInRuns_++;
    recordLongestRun_ = std::max(recordLongestRun_, runLength);

    if (valueCounts_.size() < MAX_TRACKED) {
      valueCounts_[bits]++;
    } else {
      auto it = valueCounts_.find(bits);
      if (it != valueCounts_.end()) it->second++; else valuesUntracked_++;
    }

    KeyStats *ks = nullptr;
    if (byKey_.size() < MAX_TRACKED) {
      ks = &byKey_[key];
    } else {
      auto it = byKey_.find(key);
      if (it != byKey_.end()) ks = &it->second; else keysUntracked_++;
    }
    if (ks) {
      ks->count++;
      ks->bytesA += enc.bytes;
      switch (enc.tier) {
        case Tier::Decimal: case Tier::Zero: ks->decimal++; break;
        case Tier::Float32: ks->float32++; break;
        default: ks->raw++; break;
      }
    }

    recordValues_.push_back(bits);
    prevBits_ = bits;
    havePrev_ = true;
  }

  void onRecordStart() {
    recordValues_.clear();  // keeps the capacity, so no per-record allocation
    havePrev_ = false;
    prevBits_ = 0;
    recordLongestRun_ = 0;
  }

  void onRecordEnd() {
    records_++;
    const size_t n = recordValues_.size();
    perRecord_[bucket(n)]++;
    if (n) {
      recordsWithDoubles_++;
      longestRun_[bucket(recordLongestRun_)]++;
    }
  }

  size_t total() const { return total_; }

  void report(std::ostream &os, size_t streamBytes) const {
    os << "\n  Double encoding analysis:\n";
    if (!total_) {
      os << "     No doubles in this file.\n";
      return;
    }

    auto line = [&](const char *label, size_t count) {
      char buf[128];
      snprintf(buf, sizeof(buf), "        %-26s %12zu  ", label, count);
      os << buf;
      pct(os, count, total_);
      os << '\n';
    };
    auto projection = [&](const char *label, size_t bytes) {
      char buf[160];
      snprintf(buf, sizeof(buf), "        %-26s %12zu bytes  ", label, bytes);
      os << buf;
      pct(os, bytes, bytesToday_);
      os << " of today, saves ";
      pct(os, bytesToday_ - bytes, streamBytes);
      os << " of stream\n";
    };

    os << "     Doubles: " << total_ << ", currently "
       << bytesToday_ << " bytes (";
    pct(os, bytesToday_, streamBytes);
    os << " of stream)\n";
    os << "     Projected encoded size:\n";
    projection("(a) per-value tiers", bytesA_);
    projection("(a+b) + record repeats", bytesAB_);
    projection("(a+b+c) + xor delta", bytesABC_);

    os << "     Tier breakdown:\n";
    line("+0.0", zero_);
    line("-0.0 (falls back to raw)", negZero_);
    line("NaN / Inf (raw)", nonFinite_);
    size_t decimalTotal = 0;
    for (auto c : byScale_) decimalTotal += c;
    line("decimal", decimalTotal);
    for (unsigned k = 0; k <= doubleenc::MAX_SCALE; k++) {
      if (!byScale_[k]) continue;
      char buf[64];
      snprintf(buf, sizeof(buf), "  scale %u%s", k,
               k <= doubleenc::SCALE_IN_MARKER_LIMIT ? "" : " (needs ext)");
      line(buf, byScale_[k]);
    }
    line("float32 exact", float32_);
    line("raw (no tier applies)", raw_);
    line("  of which decimal-but-bigger", decimalNotWorthIt_);

    os << "     Decimal significand varint length:\n";
    for (size_t i = 0; i < sigLen_.size(); i++) {
      if (!sigLen_[i]) continue;
      char buf[64];
      snprintf(buf, sizeof(buf), "  %zu byte%s", i, i == 1 ? "" : "s");
      line(buf, sigLen_[i]);
    }

    if (raw_) {
      os << "     Raw-tier trailing zero bytes:\n";
      for (size_t i = 0; i < rawTrailingZeros_.size(); i++) {
        if (!rawTrailingZeros_[i]) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "  %zu", i);
        line(buf, rawTrailingZeros_[i]);
      }
    }

    os << "     Doubles per record (" << recordsWithDoubles_ << " of "
       << records_ << " records contain any):\n";
    for (size_t i = 0; i < perRecord_.size(); i++) {
      if (!perRecord_[i]) continue;
      char buf[64];
      if (i > MAX_BUCKET) snprintf(buf, sizeof(buf), "  >%zu", MAX_BUCKET);
      else snprintf(buf, sizeof(buf), "  %zu", i);
      char out[128];
      snprintf(out, sizeof(out), "        %-26s %12zu  ", buf, perRecord_[i]);
      os << out;
      pct(os, perRecord_[i], records_);
      os << '\n';
    }

    os << "     Record-local exact repeats:\n";
    line("same as previous double", repeatOfPrevious_);
    line("same as an earlier double", repeatOfEarlier_);
    line("match beyond backref window", repeatBeyondWindow_);

    os << "     Longest run of adjacent doubles in an array, per record:\n";
    for (size_t i = 0; i < longestRun_.size(); i++) {
      if (!longestRun_[i]) continue;
      char buf[64];
      if (i > MAX_BUCKET) snprintf(buf, sizeof(buf), "  >%zu", MAX_BUCKET);
      else snprintf(buf, sizeof(buf), "  %zu", i);
      char out[128];
      snprintf(out, sizeof(out), "        %-26s %12zu  ", buf, longestRun_[i]);
      os << out;
      pct(os, longestRun_[i], recordsWithDoubles_);
      os << '\n';
    }
    line("doubles inside a run", doublesInRuns_);

    auto xorTable = [&](const char *what, const std::array<size_t, 9> &h,
                        size_t pairs) {
      os << "     XOR significant bytes, " << what << " (" << pairs
         << " pairs):\n";
      for (size_t i = 0; i < h.size(); i++) {
        if (!h[i]) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "  %zu byte%s%s", i, i == 1 ? "" : "s",
                 i <= 6 ? " (a win)" : "");
        char out[128];
        snprintf(out, sizeof(out), "        %-26s %12zu  ", buf, h[i]);
        os << out;
        pct(os, h[i], pairs);
        os << '\n';
      }
    };
    if (xorPairsRecord_) xorTable("record-wide", xorSigBytesRecord_,
                                  xorPairsRecord_);
    if (xorPairsArray_) xorTable("array runs only", xorSigBytesArray_,
                                 xorPairsArray_);

    os << "     Distinct values: " << valueCounts_.size();
    if (valuesUntracked_)
      os << " (tracking capped; " << valuesUntracked_ << " not counted)";
    os << "\n       Most frequent:\n";
    for (auto &[count, bits] : topValues(TOP_N)) {
      double d;
      std::memcpy(&d, &bits, sizeof(d));
      char buf[128];
      snprintf(buf, sizeof(buf), "        %12zu  %.17g\n", count, d);
      os << buf;
    }

    os << "     Keys holding doubles: " << byKey_.size();
    if (keysUntracked_)
      os << " (tracking capped; " << keysUntracked_ << " not counted)";
    os << "\n       Top keys (count, decimal/float32/raw, bytes saved):\n";
    for (auto &[count, key] : topKeys(TOP_N)) {
      auto &k = byKey_.at(key);
      char buf[256];
      snprintf(buf, sizeof(buf),
               "        %12zu  %6zu/%6zu/%6zu  %10zu  %s\n",
               count, k.decimal, k.float32, k.raw,
               k.count * doubleenc::RAW_BYTES - k.bytesA,
               key.empty() ? "<no enclosing key>" : key.c_str());
      os << buf;
    }
  }

  void reportJson(std::ostream &os, const std::string &filename,
                  size_t streamBytes) const {
    os << "{\"file\":\"" << escapeJson(filename) << "\""
       << ",\"streamBytes\":" << streamBytes
       << ",\"records\":" << records_
       << ",\"recordsWithDoubles\":" << recordsWithDoubles_
       << ",\"doubles\":" << total_
       << ",\"bytesToday\":" << bytesToday_
       << ",\"bytesTierA\":" << bytesA_
       << ",\"bytesTierAB\":" << bytesAB_
       << ",\"bytesTierABC\":" << bytesABC_
       << ",\"zero\":" << zero_
       << ",\"negZero\":" << negZero_
       << ",\"nonFinite\":" << nonFinite_
       << ",\"float32\":" << float32_
       << ",\"raw\":" << raw_
       << ",\"decimalNotWorthIt\":" << decimalNotWorthIt_
       << ",\"repeatOfPrevious\":" << repeatOfPrevious_
       << ",\"repeatOfEarlier\":" << repeatOfEarlier_
       << ",\"repeatBeyondWindow\":" << repeatBeyondWindow_
       << ",\"doublesInRuns\":" << doublesInRuns_
       << ",\"xorPairsRecord\":" << xorPairsRecord_
       << ",\"xorPairsArray\":" << xorPairsArray_
       << ",\"distinctValues\":" << valueCounts_.size()
       << ",\"valuesUntracked\":" << valuesUntracked_
       << ",\"keys\":" << byKey_.size()
       << ",\"keysUntracked\":" << keysUntracked_
       << ',';
    jsonArray(os, "byScale", byScale_); os << ',';
    jsonArray(os, "significandVarintLen", sigLen_); os << ',';
    jsonArray(os, "rawTrailingZeroBytes", rawTrailingZeros_); os << ',';
    jsonArray(os, "doublesPerRecord", perRecord_); os << ',';
    jsonArray(os, "longestArrayRun", longestRun_); os << ',';
    jsonArray(os, "xorSigBytesRecord", xorSigBytesRecord_); os << ',';
    jsonArray(os, "xorSigBytesArray", xorSigBytesArray_);

    os << ",\"topValues\":[";
    bool first = true;
    for (auto &[count, bits] : topValues(TOP_N)) {
      double d;
      std::memcpy(&d, &bits, sizeof(d));
      char buf[128];
      snprintf(buf, sizeof(buf), "%s{\"count\":%zu,\"bits\":\"%016" PRIx64
               "\",\"value\":%.17g}", first ? "" : ",", count, bits, d);
      os << buf;
      first = false;
    }
    os << "],\"topKeys\":[";
    first = true;
    for (auto &[count, key] : topKeys(TOP_N)) {
      auto &k = byKey_.at(key);
      os << (first ? "" : ",")
         << "{\"key\":\"" << escapeJson(key) << "\",\"count\":" << count
         << ",\"decimal\":" << k.decimal << ",\"float32\":" << k.float32
         << ",\"raw\":" << k.raw << ",\"bytesTierA\":" << k.bytesA << '}';
      first = false;
    }
    os << "]}\n";
  }

private:
  std::vector<std::pair<size_t, uint64_t>> topValues(size_t n) const {
    std::vector<std::pair<size_t, uint64_t>> v;
    v.reserve(valueCounts_.size());
    for (auto &[bits, count] : valueCounts_) v.emplace_back(count, bits);
    n = std::min(n, v.size());
    std::partial_sort(v.begin(), v.begin() + static_cast<long>(n), v.end(),
                      std::greater<std::pair<size_t, uint64_t>>());
    v.resize(n);
    return v;
  }

  std::vector<std::pair<size_t, std::string>> topKeys(size_t n) const {
    std::vector<std::pair<size_t, std::string>> v;
    v.reserve(byKey_.size());
    for (auto &[key, stats] : byKey_) v.emplace_back(stats.count, key);
    n = std::min(n, v.size());
    std::partial_sort(v.begin(), v.begin() + static_cast<long>(n), v.end(),
                      std::greater<std::pair<size_t, std::string>>());
    v.resize(n);
    return v;
  }
};

}

}
