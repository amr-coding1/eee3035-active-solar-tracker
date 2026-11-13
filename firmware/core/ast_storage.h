// Abstract persistent storage used by the ring buffer (contract 7.2).
//
// The ring buffer never sees a file system: it sees numbered "files" that can
// be read and written at a byte offset. Adapters:
//   ESP32  : LittleFS files /ast/sNNN.bin, /ast/metaA.bin (ast_datalink/src/...)
//   PC     : a directory of files (firmware/host/host_storage.h)
//   tests  : RAM with fault injection (MemStorage below)
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <map>
#include <vector>

namespace ast {

class Storage {
 public:
  virtual ~Storage() {}
  // Reads exactly `len` bytes at `offset`. Returns false when the file does
  // not exist, is too short, or the device reports an error.
  virtual bool read(uint16_t file_id, uint32_t offset, uint8_t* buf, size_t len) = 0;
  // Writes `len` bytes at `offset`, creating the file and zero-filling any gap
  // before `offset`. The data must be durable (flushed) when true is returned.
  virtual bool write(uint16_t file_id, uint32_t offset, const uint8_t* buf, size_t len) = 0;
};

// RAM storage for unit tests, with the failure modes flash really has.
class MemStorage : public Storage {
 public:
  bool read(uint16_t file_id, uint32_t offset, uint8_t* buf, size_t len) override {
    ++reads;
    auto it = files.find(file_id);
    if (it == files.end()) return false;
    const std::vector<uint8_t>& f = it->second;
    if (static_cast<size_t>(offset) + len > f.size()) return false;
    for (size_t i = 0; i < len; ++i) buf[i] = f[offset + i];
    return true;
  }

  bool write(uint16_t file_id, uint32_t offset, const uint8_t* buf, size_t len) override {
    ++writes;
    if (fail_all_writes) return false;
    if (fail_after_writes >= 0) {
      if (fail_after_writes == 0) {
        // Simulated power cut during this write: only the first torn_bytes land.
        apply(file_id, offset, buf, torn_bytes < len ? torn_bytes : len);
        fail_all_writes = true;   // the device is "off" until the test restores power
        fail_after_writes = -1;
        return false;
      }
      --fail_after_writes;
    }
    apply(file_id, offset, buf, len);
    return true;
  }

  void power_restored() { fail_all_writes = false; fail_after_writes = -1; }

  std::map<uint16_t, std::vector<uint8_t>> files;
  int fail_after_writes = -1;  // >= 0: that many writes succeed, the next one is torn
  size_t torn_bytes = 0;       // bytes of the torn write that reach the medium
  bool fail_all_writes = false;
  unsigned long reads = 0;
  unsigned long writes = 0;

 private:
  void apply(uint16_t file_id, uint32_t offset, const uint8_t* buf, size_t len) {
    std::vector<uint8_t>& f = files[file_id];
    if (f.size() < static_cast<size_t>(offset) + len) f.resize(static_cast<size_t>(offset) + len, 0);
    for (size_t i = 0; i < len; ++i) f[offset + i] = buf[i];
  }
};

}  // namespace ast
