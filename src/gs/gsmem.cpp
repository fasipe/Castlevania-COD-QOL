#include "gs/gsmem.h"

namespace gsmem_detail {
namespace {
Swizzle makeSwizzle() {
  Swizzle s{};
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      s.t32[y * 64 + x] = uint16_t(addr32Slow(x, y, false));
      s.t32z[y * 64 + x] = uint16_t(addr32Slow(x, y, true));
    }
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      s.t16[y * 64 + x] = uint16_t(addr16Slow(x, y, PSMCT16));
      s.t16s[y * 64 + x] = uint16_t(addr16Slow(x, y, PSMCT16S));
      s.t16z[y * 64 + x] = uint16_t(addr16Slow(x, y, PSMZ16));
      s.t16sz[y * 64 + x] = uint16_t(addr16Slow(x, y, PSMZ16S));
    }
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 128; ++x) s.t8[y * 128 + x] = uint16_t(addr8Slow(x, y));
  for (uint32_t y = 0; y < 128; ++y)
    for (uint32_t x = 0; x < 128; ++x) s.t4[y * 128 + x] = uint16_t(addr4Slow(x, y));
  return s;
}
}  // namespace
const Swizzle kSwizzle = makeSwizzle();
}  // namespace gsmem_detail



uint32_t GsMemory::bitsPerPixel(uint32_t psm) {
  switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    case PSMCT24: case PSMZ24: return 24;
    default: return 32;
  }
}

uint32_t GsMemory::read(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const {
  switch (psm) {
    case PSMCT32: case PSMZ32: return mem_[addr32(bp, bw, x, y, psm == PSMZ32)];
    case PSMCT24: case PSMZ24: return mem_[addr32(bp, bw, x, y, psm == PSMZ24)] & 0xFFFFFF;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
      return reinterpret_cast<const uint16_t*>(mem_.data())[addr16(bp, bw, x, y, psm)];
    case PSMT8: return reinterpret_cast<const uint8_t*>(mem_.data())[addr8(bp, bw, x, y)];
    case PSMT4: {
      const uint32_t n = addr4(bp, bw, x, y);
      return (reinterpret_cast<const uint8_t*>(mem_.data())[n >> 1] >> ((n & 1) * 4)) & 0xF;
    }
    case PSMT8H: return mem_[addr32(bp, bw, x, y)] >> 24;
    case PSMT4HL: return (mem_[addr32(bp, bw, x, y)] >> 24) & 0xF;
    case PSMT4HH: return mem_[addr32(bp, bw, x, y)] >> 28;
  }
  return 0;
}

void GsMemory::write(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) {
  switch (psm) {
    case PSMCT32: case PSMZ32: mem_[addr32(bp, bw, x, y, psm == PSMZ32)] = v; return;
    case PSMCT24: case PSMZ24: {
      uint32_t& w = mem_[addr32(bp, bw, x, y, psm == PSMZ24)];
      w = (w & 0xFF000000) | (v & 0xFFFFFF);
      return;
    }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
      reinterpret_cast<uint16_t*>(mem_.data())[addr16(bp, bw, x, y, psm)] = uint16_t(v);
      return;
    case PSMT8: reinterpret_cast<uint8_t*>(mem_.data())[addr8(bp, bw, x, y)] = uint8_t(v); return;
    case PSMT4: {
      const uint32_t n = addr4(bp, bw, x, y);
      uint8_t& b = reinterpret_cast<uint8_t*>(mem_.data())[n >> 1];
      b = (n & 1) ? uint8_t((b & 0x0F) | (v << 4)) : uint8_t((b & 0xF0) | (v & 0xF));
      return;
    }
    case PSMT8H: {
      uint32_t& w = mem_[addr32(bp, bw, x, y)];
      w = (w & 0x00FFFFFF) | (v << 24);
      return;
    }
    case PSMT4HL: {
      uint32_t& w = mem_[addr32(bp, bw, x, y)];
      w = (w & 0xF0FFFFFF) | ((v & 0xF) << 24);
      return;
    }
    case PSMT4HH: {
      uint32_t& w = mem_[addr32(bp, bw, x, y)];
      w = (w & 0x0FFFFFFF) | ((v & 0xF) << 28);
      return;
    }
  }
}
