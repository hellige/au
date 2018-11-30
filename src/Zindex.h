#pragma once

#include "au/AuDecoder.h"

#include <memory>
#include <optional>

namespace au {

int zindexFile(const std::string &fileName,
               const std::optional<std::string> &indexFilename);

class ZipByteSource : public FileByteSource {
  class Impl;
  std::unique_ptr<Impl> impl_;
public:
  explicit ZipByteSource(const std::string &fname,
                         const std::optional<std::string> &indexFname);
  ~ZipByteSource();

  bool isSeekable() const override;
  size_t doRead(char *buf, size_t len) override;
  size_t endPos() const override;
  void doSeek(size_t abspos) override;
};

}
