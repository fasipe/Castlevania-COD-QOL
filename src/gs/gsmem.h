#pragma once
#include <cstdint>
#include <vector>

// GS local memory (4 MB) and its pixel addressing. Each pixel format stores pixels in pages,
// pages in blocks and blocks in columns in its own order ("swizzle"); the functions below map a
// pixel (x, y) of a buffer at block pointer `bp` with width `bw` (in 64-pixel units) to its
// place in memory. The layouts follow the GS manual; the 8- and 4-bit column orders are derived
// from the 32-bit one, which they interleave.
enum Psm : uint32_t {
  PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A, PSMT8 = 0x13, PSMT4 = 0x14,
  PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C, PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32,
  PSMZ16S = 0x3A,
};

class GsMemory {
public:
  static constexpr uint32_t kSize = 4 * 1024 * 1024;

  GsMemory() : mem_(kSize / 4 + 1) {}  // + a word for vector gathers of the last bytes

  // Word (32-bit) index for 32/24-bit formats and their 8H/4HL/4HH aliases.
  static uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool z = false);
  // Halfword index for 16-bit formats.
  static uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t psm);
  static uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);  // byte index
  static uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);  // nibble index

  // Raw pixel value in the format's own width (no colour conversion).
  uint32_t read(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
  void write(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

  // Addresses along one row of a 32- or 16-bit buffer (words or halfwords): the row's share of
  // the address is worked out once, leaving a table lookup per pixel.
  struct Row {
    uint32_t base, pageUnits, mask;
    const uint16_t* swizzle;  // the row of the page's swizzle table
    uint32_t at(uint32_t x) const { return (base + (x >> 6) * pageUnits + swizzle[x & 63]) & mask; }
  };
  static Row row32(uint32_t bp, uint32_t bw, uint32_t y, bool z = false);
  static Row row16(uint32_t bp, uint32_t bw, uint32_t y, uint32_t psm);

  uint32_t* words() { return mem_.data(); }
  const uint32_t* words() const { return mem_.data(); }
  uint8_t* bytes() { return reinterpret_cast<uint8_t*>(mem_.data()); }

  static uint32_t bitsPerPixel(uint32_t psm);

private:
  std::vector<uint32_t> mem_;
};

// Addressing is on the rasterizer's hot path, so it is inline.
namespace gsmem_detail {
inline constexpr uint8_t kBlock32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
inline constexpr uint8_t kBlock16[8][4] = {{0, 2, 8, 10},   {1, 3, 9, 11},   {4, 6, 12, 14},  {5, 7, 13, 15},
                                    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
inline constexpr uint8_t kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
                                     {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
// Word order of an 8x8 PSMCT32 block (two columns of 8x2 pixels per 16 words).
inline constexpr uint8_t kColumn32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13},     {2, 3, 6, 7, 10, 11, 14, 15},   {16, 17, 20, 21, 24, 25, 28, 29},
    {18, 19, 22, 23, 26, 27, 30, 31}, {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
inline constexpr uint32_t kWordsPerBlock = 64;
inline constexpr uint32_t kBlocksPerPage = 32;
inline constexpr uint32_t kMemWords = GsMemory::kSize / 4;

// Shared by PSMT8 and PSMT4: a column is 4 rows; each row uses one row of the 32-bit column
// table, rows 2-3 (even columns) or 0-1 (odd columns) with the two 4-word halves swapped, and
// picks the byte/nibble lane from the row pair and the 8-pixel group.
inline uint32_t subword(uint32_t x, uint32_t y, uint32_t lanesPerGroup, uint32_t& lane) {
  const uint32_t cy = (y >> 2) & 3, yy = y & 3;
  const uint32_t row = cy * 2 + (yy & 1);
  uint32_t xx = x & 7;
  if (((cy & 1) != 0) != (yy >= 2)) xx ^= 4;
  lane = (yy >> 1) + 2 * ((x >> 3) & (lanesPerGroup - 1));
  return kColumn32[row][xx];
}

// Offsets of every pixel within a page (in the format's units, block included), computed once
// from the layouts above; addressing is then one table lookup.
struct Swizzle {
  uint16_t t32[32 * 64], t32z[32 * 64];                            // words
  uint16_t t16[64 * 64], t16s[64 * 64], t16z[64 * 64], t16sz[64 * 64];  // halfwords
  uint16_t t8[64 * 128];                                           // bytes
  uint16_t t4[128 * 128];                                          // nibbles
  uint16_t pad[2];  // vector gathers read 32 bits at the last entry
};
extern const Swizzle kSwizzle;

inline uint32_t addr32Slow(uint32_t x, uint32_t y, bool z) {
  uint32_t block = kBlock32[(y >> 3) & 3][(x >> 3) & 7];
  if (z) block ^= 24;
  return block * kWordsPerBlock + kColumn32[y & 7][x & 7];
}
inline uint32_t addr16Slow(uint32_t x, uint32_t y, uint32_t psm) {
  const bool s = psm == PSMCT16S || psm == PSMZ16S;
  uint32_t block = s ? kBlock16S[(y >> 3) & 7][(x >> 4) & 3] : kBlock16[(y >> 3) & 7][(x >> 4) & 3];
  if (psm == PSMZ16 || psm == PSMZ16S) block ^= 24;
  return block * kWordsPerBlock * 2 + kColumn32[y & 7][x & 7] * 2 + ((x >> 3) & 1);
}
inline uint32_t addr8Slow(uint32_t x, uint32_t y) {
  uint32_t lane;
  const uint32_t word = subword(x, y, 2, lane);
  return kBlock32[(y >> 4) & 3][(x >> 4) & 7] * kWordsPerBlock * 4 + word * 4 + lane;
}
inline uint32_t addr4Slow(uint32_t x, uint32_t y) {
  uint32_t lane;
  const uint32_t word = subword(x, y, 4, lane);
  return kBlock16[(y >> 4) & 7][(x >> 5) & 3] * kWordsPerBlock * 8 + word * 8 + lane;
}
}  // namespace gsmem_detail

inline uint32_t GsMemory::addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool z) {
  using namespace gsmem_detail;
  const uint32_t page = (x >> 6) + (y >> 5) * bw;
  const uint16_t* t = z ? kSwizzle.t32z : kSwizzle.t32;
  return ((bp + page * kBlocksPerPage) * kWordsPerBlock + t[(y & 31) * 64 + (x & 63)]) % kMemWords;
}

inline GsMemory::Row GsMemory::row32(uint32_t bp, uint32_t bw, uint32_t y, bool z) {
  using namespace gsmem_detail;
  return {(bp + (y >> 5) * bw * kBlocksPerPage) * kWordsPerBlock, kBlocksPerPage * kWordsPerBlock, kMemWords - 1,
          (z ? kSwizzle.t32z : kSwizzle.t32) + (y & 31) * 64};
}

inline GsMemory::Row GsMemory::row16(uint32_t bp, uint32_t bw, uint32_t y, uint32_t psm) {
  using namespace gsmem_detail;
  const uint16_t* t = psm == PSMCT16 ? kSwizzle.t16 : psm == PSMCT16S ? kSwizzle.t16s : psm == PSMZ16 ? kSwizzle.t16z : kSwizzle.t16sz;
  return {(bp + (y >> 6) * bw * kBlocksPerPage) * kWordsPerBlock * 2, kBlocksPerPage * kWordsPerBlock * 2, kMemWords * 2 - 1,
          t + (y & 63) * 64};
}

inline uint32_t GsMemory::addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t psm) {
  using namespace gsmem_detail;
  const uint32_t page = (x >> 6) + (y >> 6) * bw;
  const uint16_t* t = psm == PSMCT16 ? kSwizzle.t16 : psm == PSMCT16S ? kSwizzle.t16s : psm == PSMZ16 ? kSwizzle.t16z : kSwizzle.t16sz;
  return ((bp + page * kBlocksPerPage) * kWordsPerBlock * 2 + t[(y & 63) * 64 + (x & 63)]) % (kMemWords * 2);
}

inline uint32_t GsMemory::addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  using namespace gsmem_detail;
  const uint32_t page = (x >> 7) + (y >> 6) * (bw >> 1);
  return ((bp + page * kBlocksPerPage) * kWordsPerBlock * 4 + kSwizzle.t8[(y & 63) * 128 + (x & 127)]) % (kMemWords * 4);
}

inline uint32_t GsMemory::addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  using namespace gsmem_detail;
  const uint32_t page = (x >> 7) + (y >> 7) * (bw >> 1);
  return ((bp + page * kBlocksPerPage) * kWordsPerBlock * 8 + kSwizzle.t4[(y & 127) * 128 + (x & 127)]) % (kMemWords * 8);
}
