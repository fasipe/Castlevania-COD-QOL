#pragma once
#include <cstdint>

// 16-bit GS pixels (5:5:5:1) <-> 32-bit RGBA; alpha 0x80 stands for the set bit.
inline uint32_t from16(uint32_t c) {
  const uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
  return r | (g << 8) | (b << 16) | ((c & 0x8000) ? 0x80000000u : 0);
}
inline uint32_t to16(uint32_t c) {
  return ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 31) << 15);
}

inline uint32_t bits(uint64_t v, int lo, int n) { return uint32_t((v >> lo) & ((1ull << n) - 1)); }
