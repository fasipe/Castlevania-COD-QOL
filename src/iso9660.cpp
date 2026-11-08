#include "iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

std::string normalize(std::string_view path) {
  if (auto colon = path.find(':'); colon != std::string_view::npos) path.remove_prefix(colon + 1);
  if (auto semi = path.find(';'); semi != std::string_view::npos) path = path.substr(0, semi);
  std::string out = "/";
  for (char c : path) {
    if (c == '\\') c = '/';
    if (c == '/' && out.back() == '/') continue;
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}
}  // namespace

bool Iso9660::open(const std::filesystem::path& path, std::string& error) {
  file_.open(path, std::ios::binary);
  if (!file_) {
    error = "cannot open " + path.string();
    return false;
  }
  uint8_t pvd[kSectorSize];
  if (!readSectors(16, 1, pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0) {
    error = "not an ISO9660 image";
    return false;
  }
  const uint8_t* root = pvd + 156;
  entries_.clear();
  if (!walk(le32(root + 2), le32(root + 10), "", 0)) {
    error = "corrupt directory tree";
    return false;
  }
  return true;
}

bool Iso9660::readSectors(uint32_t lba, uint32_t count, void* dst) const {
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(lba) * kSectorSize);
  file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(count) * kSectorSize);
  return static_cast<bool>(file_);
}

bool Iso9660::walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth) {
  if (depth > 16) return false;
  const uint32_t sectors = (size + kSectorSize - 1) / kSectorSize;
  std::vector<uint8_t> dir(sectors * kSectorSize);
  if (!readSectors(lba, sectors, dir.data())) return false;
  for (size_t i = 0; i < size;) {
    const uint8_t len = dir[i];
    if (len == 0) {  // records never straddle sectors; skip padding
      i = (i / kSectorSize + 1) * kSectorSize;
      continue;
    }
    if (i + len > dir.size() || len < 34) return false;
    const uint8_t* rec = &dir[i];
    const uint8_t nameLen = rec[32];
    std::string name(reinterpret_cast<const char*>(rec + 33), nameLen);
    i += len;
    if (nameLen == 1 && (name[0] == 0 || name[0] == 1)) continue;  // "." and ".."
    if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
    const std::string full = prefix + "/" + name;
    if (rec[25] & 2) {
      if (!walk(le32(rec + 2), le32(rec + 10), full, depth + 1)) return false;
    } else {
      entries_.push_back({normalize(full), le32(rec + 2), le32(rec + 10)});
    }
  }
  return true;
}

std::optional<Iso9660::Entry> Iso9660::find(std::string_view path) const {
  const std::string want = normalize(path);
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.path == want; });
  if (it == entries_.end()) return std::nullopt;
  return *it;
}

std::vector<uint8_t> Iso9660::readFile(const Entry& entry) const {
  std::vector<uint8_t> data(entry.size);
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(entry.lba) * kSectorSize);
  file_.read(reinterpret_cast<char*>(data.data()), entry.size);
  if (!file_) data.clear();
  return data;
}
