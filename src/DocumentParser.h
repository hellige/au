#pragma once

#include "au/AuDecoder.h"
#include "Dictionary.h"
#include "AuRecordHandler.h"

#include <rapidjson/document.h>

#include <vector>

class DocumentParser {
  rapidjson::Document document_;

  struct ValueHandler {
    rapidjson::Document *doc;
    AuByteSource &source;
    const Dictionary::Dict &dict;
    std::vector<char> str;
    std::vector<size_t> count;

    ValueHandler(AuByteSource &source,
                 const Dictionary::Dict &dict)
        : doc(nullptr), source(source), dict(dict) {
      count.reserve(8);
      count.emplace_back(0);
    }

    bool operator()(rapidjson::Document& d) {
      doc = &d;
      ValueParser<decltype(*this)> vp(source, *this);
      vp.value();
      return true;
    }

    void onObjectStart() { doc->StartObject(); count.emplace_back(0); }
    void onObjectEnd() { doc->EndObject(count.back()/2); count.pop_back(); }
    void onArrayStart() { doc->StartArray(); count.emplace_back(0); }
    void onArrayEnd() { doc->EndArray(count.back()); count.pop_back(); }
    void onNull(size_t) { doc->Null(); count.back()++; }
    void onBool(size_t, bool v) { doc->Bool(v); count.back()++; }
    void onInt(size_t, int64_t v) { doc->Int64(v); count.back()++; }
    void onUint(size_t, uint64_t v) { doc->Uint64(v); count.back()++; }
    void onDouble(size_t, double d) { doc->Double(d); count.back()++; }
    void onTime(size_t, std::chrono::system_clock::time_point) {
      count.back()++;
      THROW_RT("Timestamps not supported in rapidjson document parser!");
    }
    void onDictRef(size_t, size_t idx) {
      const auto &v = dict.at(idx);
      doc->String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), true);
      count.back()++;
    }

    void onStringStart(size_t, size_t len) {
      str.clear();
      str.reserve(len);
    }
    void onStringEnd() {
      doc->String(
          str.data(), static_cast<rapidjson::SizeType>(str.size()), true);
      count.back()++;
    }
    void onStringFragment(std::string_view frag) {
      str.insert(str.end(), frag.data(), frag.data() + frag.size());
    }
  };

public:
  const rapidjson::Document &document() const { return document_; }

  void parse(AuByteSource &source, Dictionary &dictionary) {
    AuRecordHandler rh(dictionary, *this);
    if (!RecordParser(source, rh).parseUntilValue())
      THROW_RT("DocumentParser failed to parse value record!");
  }

  void onValue(AuByteSource &source, const Dictionary::Dict &dict) {
    ValueHandler handler(source, dict);
    document_.Populate(handler);
  }
};

