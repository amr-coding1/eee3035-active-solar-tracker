// PC implementation of ast::Storage: one ordinary file per storage "file id"
// inside a state directory (the device's "flash").
#pragma once
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "ast_config.h"
#include "ast_storage.h"

namespace ast_host {

class DirStorage : public ast::Storage {
 public:
  explicit DirStorage(const std::string& dir) : dir_(dir) { ::mkdir(dir_.c_str(), 0777); }

  std::string path(uint16_t file_id) const {
    char name[32];
    if (file_id == ast::kFileMetaA) snprintf(name, sizeof name, "meta_a.bin");
    else if (file_id == ast::kFileMetaB) snprintf(name, sizeof name, "meta_b.bin");
    else snprintf(name, sizeof name, "seg_%03u.bin", static_cast<unsigned>(file_id));
    return dir_ + "/" + name;
  }

  bool read(uint16_t file_id, uint32_t offset, uint8_t* buf, size_t len) override {
    const int fd = ::open(path(file_id).c_str(), O_RDONLY);
    if (fd < 0) return false;
    const ssize_t n = ::pread(fd, buf, len, static_cast<off_t>(offset));
    ::close(fd);
    return n == static_cast<ssize_t>(len);
  }

  bool write(uint16_t file_id, uint32_t offset, const uint8_t* buf, size_t len) override {
    const int fd = ::open(path(file_id).c_str(), O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return false;
    // pwrite beyond the end zero-fills the gap, as the Storage contract requires.
    const ssize_t n = ::pwrite(fd, buf, len, static_cast<off_t>(offset));
    const bool ok = (n == static_cast<ssize_t>(len)) && ::close(fd) == 0;
    if (n != static_cast<ssize_t>(len)) ::close(fd);
    return ok;
  }

 private:
  std::string dir_;
};

}  // namespace ast_host
