#include "elf.h"

#include <cstring>

#include "ee/memory.h"

namespace {
template <class T>
T get(const std::vector<uint8_t>& f, size_t off) {
  T v{};
  if (off + sizeof(T) <= f.size()) std::memcpy(&v, f.data() + off, sizeof(T));
  return v;
}
}  // namespace

bool loadElf(const std::vector<uint8_t>& f, Memory& mem, ElfImage& out, std::string& error) {
  if (f.size() < 52 || std::memcmp(f.data(), "\x7F" "ELF", 4) != 0) {
    error = "not an ELF file";
    return false;
  }
  if (f[4] != 1 || f[5] != 1 || get<uint16_t>(f, 18) != 8) {
    error = "expected a 32-bit little-endian MIPS ELF";
    return false;
  }
  out.entry = get<uint32_t>(f, 24);
  const uint32_t phoff = get<uint32_t>(f, 28);
  const uint16_t phentsize = get<uint16_t>(f, 42), phnum = get<uint16_t>(f, 44);
  for (uint32_t i = 0; i < phnum; ++i) {
    const size_t ph = phoff + size_t(i) * phentsize;
    if (get<uint32_t>(f, ph) != 1) continue;  // PT_LOAD only
    ElfSegment seg{get<uint32_t>(f, ph + 8), get<uint32_t>(f, ph + 16), get<uint32_t>(f, ph + 20),
                   get<uint32_t>(f, ph + 24)};
    const uint32_t offset = get<uint32_t>(f, ph + 4);
    uint8_t* dst = mem.hostPtr(seg.vaddr, seg.memSize);
    if (!dst || seg.fileSize > seg.memSize || size_t(offset) + seg.fileSize > f.size() ||
        (seg.vaddr & 0x1FFFFFFF) + uint64_t(seg.memSize) > Memory::kRamSize) {
      error = "segment outside EE RAM or file";
      return false;
    }
    std::memcpy(dst, f.data() + offset, seg.fileSize);
    std::memset(dst + seg.fileSize, 0, seg.memSize - seg.fileSize);
    out.segments.push_back(seg);
  }
  if (out.segments.empty()) {
    error = "no loadable segments";
    return false;
  }
  return true;
}
