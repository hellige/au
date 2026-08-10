#pragma once

#include <sstream>
#include <stdexcept>
#include <string>

#define AU_STR(EXPRS) (([&]() -> std::string { \
    std::ostringstream oss_; \
    oss_ << EXPRS; \
    return oss_.str(); \
    })())

#define AU_THROW(stuff) \
  do { \
    std::ostringstream _message; \
    _message << stuff; \
    throw au::parse_error(_message.str()); \
  } while (0)

#define THROW_RT(stuff) throw std::runtime_error(AU_STR(stuff))

namespace au {

struct parse_error : std::runtime_error {
  explicit parse_error(const std::string &what)
      : std::runtime_error(what) {}
};

/** Distinguished from a generic parse_error so that callers still deciding
 * whether a stream is au at all can tell "not an au file" apart from "an au
 * file we can't read". */
struct bad_version : parse_error {
  explicit bad_version(const std::string &what)
      : parse_error(what) {}
};

}
