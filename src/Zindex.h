#pragma once

#include "au/AuDecoder.h"
#include "au/FileByteSource.h"

#include <memory>
#include <optional>

namespace au {

class FileByteSourceImpl;

int zindexFile(const std::string &fileName,
               const std::optional<std::string> &indexFilename);

class ZipByteSource : public FileByteSource {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  ZipByteSource(const std::string &fname,
                const std::optional<std::string> &indexFname);
  ZipByteSource(FileByteSourceImpl &source,
                const std::optional<std::string> &indexFname);
  ~ZipByteSource() override;

  bool isSeekable() const override;
  size_t doRead(char *buf, size_t len) override;
  size_t endPos() const override;
  void doSeek(size_t abspos) override;
};

}
