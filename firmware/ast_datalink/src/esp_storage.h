// LittleFS implementation of ast::Storage (contract 7.2).
// Files: /ast/s000.bin ... /ast/s119.bin (96 records each), /ast/metaA.bin, /ast/metaB.bin
#pragma once
#include "core/ast_storage.h"

namespace ast_esp {

class LittleFsStorage : public ast::Storage {
 public:
  // Mounts the "spiffs" data partition as LittleFS (formats it the first
  // time). Returns false if there is no usable partition.
  bool begin();
  bool read(uint16_t file_id, uint32_t offset, uint8_t* buf, size_t len) override;
  bool write(uint16_t file_id, uint32_t offset, const uint8_t* buf, size_t len) override;
  size_t total_bytes() const;
  size_t used_bytes() const;
  bool mounted() const { return mounted_; }

 private:
  static void path_of(uint16_t file_id, char* out, size_t cap);
  bool mounted_ = false;
};

}  // namespace ast_esp
