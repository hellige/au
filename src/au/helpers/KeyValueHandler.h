#pragma once

#include "AuRecordHandler.h"
#include "Dictionary.h"
#include "au/AuDecoder.h"
#include "au/Handlers.h"

#include <functional>
#include <string>
#include <variant>
#include <vector>

/** @file This pair of handlers can be used to process all _scalar_ key-value
 * pairs in an au file via a callback. If the value is not one of the ValType
 * types, the callback will not be called. Usage:
 * 
 *      au::BufferByteSource auBuf(someDataBuffer, bufferLen);
 *      KeyValueRecHandler handler(
 *        [](const std::string &key, au::KeyValueHandler::ValType val) {
 *          // Do something with the key:value
 *        }, auBuf);
 */

namespace au {
struct KeyValueHandler : public au::NoopValueHandler {
  au::Dictionary::Dict &dict_;
  std::vector<char> str_;

  using ValType =
      std::variant<std::nullptr_t, uint64_t, int64_t, double, bool,
                   std::string, std::chrono::system_clock::time_point>;
  using CallbackType = std::function<void(const std::string &path, ValType v)>;
  const CallbackType &callback_;

  enum class Context : uint8_t { BARE, OBJECT, ARRAY };
  struct ContextMarker {
    Context context;
    size_t counter;
    std::string parent;
    std::string key;
    ContextMarker(Context context, const std::string &parent,
                  const std::string &key)
        : context(context), parent(parent), key(key) {}
    std::string path() const { return parent + "/" + key; }
  };
  std::vector<ContextMarker> context_;

  KeyValueHandler(au::Dictionary::Dict &dict, const CallbackType &kvCallback)
      : dict_(dict), callback_(kvCallback) {
    context_.emplace_back(Context::BARE, "", "");
  }

  bool isKey() const {
    auto &c = context_.back();
    return (c.context == Context::OBJECT) && (c.counter % 2 == 0);
  }

  void incrCounter() {
    context_.back().counter++;
  }

  void callback(ValType val) {
    callback_(context_.back().path(), val);
  }

  void onObjectStart() override {
    auto &c = context_.back();
    if (c.context == Context::BARE) {
      context_.emplace_back(Context::OBJECT, "", "");
    } else {
      context_.emplace_back(Context::OBJECT, c.parent + "/" + c.key, "");
    }
  }
  void onObjectEnd() override {
    context_.pop_back();
    incrCounter();
  }

  void onArrayStart() override {
    auto &c = context_.back();
    context_.emplace_back(Context::ARRAY, c.parent + "/" + c.key, "");
  }
  void onArrayEnd() {
    context_.pop_back();
    incrCounter();
  }

  void onNull(size_t) override {
    callback(nullptr);
    incrCounter();
  }
  void onBool(size_t, bool b) override {
    callback(b);
    incrCounter();
  }
  void onInt(size_t, int64_t v) override {
    callback(v);
    incrCounter();
  }
  void onUint(size_t, uint64_t v) override {
    callback(v);
    incrCounter();
  }
  void onDouble(size_t, double d) override {
    callback(d);
    incrCounter();
  }
  virtual void onTime(size_t, std::chrono::system_clock::time_point nanos) {
    callback(nanos);
    incrCounter();
  }

  void onDictRef(size_t, size_t dictIdx) override {
    if (isKey()) {
      context_.back().key = dict_.at(dictIdx);
    } else {
      callback(dict_.at(dictIdx));
    }
    incrCounter();
  }

  void onStringStart(size_t, size_t len) override {
    str_.clear();
    str_.reserve(len);
  }
  void onStringFragment(std::string_view frag) override {
    str_.insert(str_.end(), frag.data(), frag.data() + frag.size());
  }
  void onStringEnd() {
    std::string_view sv(&str_.front(), str_.size());
    if (isKey()) {
      context_.back().key = sv;
    } else {
      callback(std::string(sv));
    }
    incrCounter();
  }
};

class KeyValueRecHandler : public au::NoopRecordHandler {
  const KeyValueHandler::CallbackType kvCallback;
  au::Dictionary dictionary;
  au::AuRecordHandler<KeyValueRecHandler> auRecHandler;

 public:
  KeyValueRecHandler(const KeyValueHandler::CallbackType &&kvCallback)
      : kvCallback(kvCallback),
        auRecHandler(dictionary, *this)
  {}

  void onValue(au::AuByteSource &src, au::Dictionary::Dict &dict) {
    KeyValueHandler kvHandler(dict, kvCallback);
    au::ValueParser parser(src, kvHandler);
    parser.value();
  }
};

} // namespace

