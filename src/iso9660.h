#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Read-only ISO9660 reader. The original disc image is never written to.
class Iso9660 {
public:
  static constexpr uint32_t kSectorSize = 2048;

  struct Entry {
    std::string path;  // "/SOUND/SD.BIN", without the ";1" version suffix
    uint32_t lba = 0;
    uint32_t size = 0;
  };

  bool open(const std::filesystem::path& path, std::string& error);

  // Accepts "SLES_537.55", "/IOP/PADMAN.IRX" or "cdrom0:\\SLES_537.55;1"; case-insensitive.
  std::optional<Entry> find(std::string_view path) const;
  std::vector<uint8_t> readFile(const Entry& entry) const;
  bool readSectors(uint32_t lba, uint32_t count, void* dst) const;
  const std::vector<Entry>& entries() const { return entries_; }

private:
  bool walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth);

  mutable std::ifstream file_;
  std::vector<Entry> entries_;
};
