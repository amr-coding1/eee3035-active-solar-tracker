#include "esp_storage.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

#include "core/ast_config.h"

namespace ast_esp {

void LittleFsStorage::path_of(uint16_t file_id, char* out, size_t cap) {
  if (file_id == ast::kFileMetaA) snprintf(out, cap, "/ast/metaA.bin");
  else if (file_id == ast::kFileMetaB) snprintf(out, cap, "/ast/metaB.bin");
  else snprintf(out, cap, "/ast/s%03u.bin", static_cast<unsigned>(file_id));
}

bool LittleFsStorage::begin() {
  // Partition scheme "With SPIFFS partition": 9.4 MB data partition labelled
  // "spiffs". The ring buffer needs about 0.5 MB of it.
  mounted_ = LittleFS.begin(true /* format on first use */, "/littlefs", 4, "spiffs");
  if (mounted_ && !LittleFS.exists("/ast")) LittleFS.mkdir("/ast");
  return mounted_;
}

bool LittleFsStorage::read(uint16_t file_id, uint32_t offset, uint8_t* buf, size_t len) {
  if (!mounted_) return false;
  char path[24];
  path_of(file_id, path, sizeof path);
  if (!LittleFS.exists(path)) return false;   // avoids an error log line for a missing file
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  bool ok = static_cast<size_t>(f.size()) >= static_cast<size_t>(offset) + len && f.seek(offset);
  if (ok) ok = f.read(buf, len) == len;
  f.close();
  return ok;
}

bool LittleFsStorage::write(uint16_t file_id, uint32_t offset, const uint8_t* buf, size_t len) {
  if (!mounted_) return false;
  char path[24];
  path_of(file_id, path, sizeof path);
  // "r+" keeps the existing content; a new file has to be created with "w".
  File f = LittleFS.exists(path) ? LittleFS.open(path, "r+") : LittleFS.open(path, "w");
  if (!f) return false;
  bool ok = true;
  size_t size = f.size();
  if (size < offset) {
    // Zero-fill the gap (Storage contract). Happens only after a recovery.
    ok = f.seek(size);
    const uint8_t zeros[32] = {0};
    while (ok && size < offset) {
      const size_t n = (offset - size) < sizeof zeros ? (offset - size) : sizeof zeros;
      ok = f.write(zeros, n) == n;
      size += n;
    }
  }
  ok = ok && f.seek(offset) && f.write(buf, len) == len;
  f.close();   // close() commits the file: LittleFS updates it atomically
  return ok;
}

size_t LittleFsStorage::total_bytes() const { return mounted_ ? LittleFS.totalBytes() : 0; }
size_t LittleFsStorage::used_bytes() const { return mounted_ ? LittleFS.usedBytes() : 0; }

}  // namespace ast_esp
