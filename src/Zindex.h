#pragma once

#include "au/AuDecoder.h"

#include <memory>

int zindexFile(const std::string &fileName);

class ZipByteSource : public FileByteSource {
  class Impl;
  std::unique_ptr<Impl> impl_;
public:
  explicit ZipByteSource(const std::string &fname);
  ~ZipByteSource();

  size_t doRead(char *buf, size_t len) override;
  size_t endPos() const override;
  void doSeek(size_t abspos) override;
};
