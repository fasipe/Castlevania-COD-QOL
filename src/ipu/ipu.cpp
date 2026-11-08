#include "ipu/ipu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <stdexcept>

// ---- MPEG-2 variable-length code tables (ISO/IEC 13818-2 annex B) -------------------------------

struct VlcEntry {
  int16_t value = 0;  // decoded value; for DCT tables the run (kEob / kEscape for those codes)
  int8_t level = 0;   // DCT tables: the level
  uint8_t len = 0;    // code length in bits, 0 for an invalid code
};
struct VlcTable {
  VlcEntry e[1 << 16];
};

namespace {
// std::lround for the small values here, without the library call.
inline int roundHalfAway(double x) { return x >= 0 ? int(x + 0.5) : -int(0.5 - x); }

constexpr int kEob = -1, kEscape = -2;

struct Code {
  const char* bits;  // spaces are ignored
  int value, level;
};

// Expands a code list into a 16-bit lookup table; overlapping codes are a typo in the list.
VlcTable* build(std::initializer_list<Code> codes) {
  auto* t = new VlcTable;
  for (const Code& c : codes) {
    uint32_t prefix = 0, len = 0;
    for (const char* p = c.bits; *p; ++p)
      if (*p == '0' || *p == '1') prefix = prefix << 1 | uint32_t(*p - '0'), ++len;
    const uint32_t first = prefix << (16 - len), count = 1u << (16 - len);
    for (uint32_t i = first; i < first + count; ++i) {
      if (t->e[i].len) throw std::logic_error(std::string("overlapping MPEG code ") + c.bits);
      t->e[i] = {int16_t(c.value), int8_t(c.level), uint8_t(len)};
    }
  }
  return t;
}

// The long DCT codes shared by tables B-14 and B-15.
#define DCT_COMMON                                                                                  \
  {"0000 0001 1100", 3, 3}, {"0000 0001 0010", 4, 3}, {"0000 0001 1110", 6, 2},                     \
  {"0000 0001 0101", 7, 2}, {"0000 0001 0001", 8, 2}, {"0000 0001 1111", 17, 1},                    \
  {"0000 0001 1010", 18, 1}, {"0000 0001 1001", 19, 1}, {"0000 0001 0111", 20, 1},                  \
  {"0000 0001 0110", 21, 1}, {"0000 0000 1011 0", 1, 6}, {"0000 0000 1010 1", 1, 7},                \
  {"0000 0000 1010 0", 2, 5}, {"0000 0000 1001 1", 3, 4}, {"0000 0000 1001 0", 5, 3},               \
  {"0000 0000 1000 1", 9, 2}, {"0000 0000 1000 0", 10, 2}, {"0000 0000 1111 1", 22, 1},             \
  {"0000 0000 1111 0", 23, 1}, {"0000 0000 1110 1", 24, 1}, {"0000 0000 1110 0", 25, 1},            \
  {"0000 0000 1101 1", 26, 1}, {"0000 0000 0111 11", 0, 16}, {"0000 0000 0111 10", 0, 17},          \
  {"0000 0000 0111 01", 0, 18}, {"0000 0000 0111 00", 0, 19}, {"0000 0000 0110 11", 0, 20},         \
  {"0000 0000 0110 10", 0, 21}, {"0000 0000 0110 01", 0, 22}, {"0000 0000 0110 00", 0, 23},         \
  {"0000 0000 0101 11", 0, 24}, {"0000 0000 0101 10", 0, 25}, {"0000 0000 0101 01", 0, 26},         \
  {"0000 0000 0101 00", 0, 27}, {"0000 0000 0100 11", 0, 28}, {"0000 0000 0100 10", 0, 29},         \
  {"0000 0000 0100 01", 0, 30}, {"0000 0000 0100 00", 0, 31}, {"0000 0000 0011 000", 0, 32},        \
  {"0000 0000 0010 111", 0, 33}, {"0000 0000 0010 110", 0, 34}, {"0000 0000 0010 101", 0, 35},      \
  {"0000 0000 0010 100", 0, 36}, {"0000 0000 0010 011", 0, 37}, {"0000 0000 0010 010", 0, 38},      \
  {"0000 0000 0010 001", 0, 39}, {"0000 0000 0010 000", 0, 40}, {"0000 0000 0011 111", 1, 8},       \
  {"0000 0000 0011 110", 1, 9}, {"0000 0000 0011 101", 1, 10}, {"0000 0000 0011 100", 1, 11},       \
  {"0000 0000 0011 011", 1, 12}, {"0000 0000 0011 010", 1, 13}, {"0000 0000 0011 001", 1, 14},      \
  {"0000 0000 0001 0011", 1, 15}, {"0000 0000 0001 0010", 1, 16}, {"0000 0000 0001 0001", 1, 17},   \
  {"0000 0000 0001 0000", 1, 18}, {"0000 0000 0001 0100", 6, 3}, {"0000 0000 0001 1010", 11, 2},    \
  {"0000 0000 0001 1001", 12, 2}, {"0000 0000 0001 1000", 13, 2}, {"0000 0000 0001 0111", 14, 2},   \
  {"0000 0000 0001 0110", 15, 2}, {"0000 0000 0001 0101", 16, 2}, {"0000 0000 0001 1111", 27, 1},   \
  {"0000 0000 0001 1110", 28, 1}, {"0000 0000 0001 1101", 29, 1}, {"0000 0000 0001 1100", 30, 1},   \
  {"0000 0000 0001 1011", 31, 1}

struct Tables {
  VlcTable *mbai, *mbType[4], *cbp, *motion, *dmv, *dcLuma, *dcChroma, *dct0, *dct1;
  Tables() {
    mbai = build({{"1", 1, 0}, {"011", 2, 0}, {"010", 3, 0}, {"0011", 4, 0}, {"0010", 5, 0},
                  {"0001 1", 6, 0}, {"0001 0", 7, 0}, {"0000 111", 8, 0}, {"0000 110", 9, 0},
                  {"0000 1011", 10, 0}, {"0000 1010", 11, 0}, {"0000 1001", 12, 0}, {"0000 1000", 13, 0},
                  {"0000 0111", 14, 0}, {"0000 0110", 15, 0}, {"0000 0101 11", 16, 0}, {"0000 0101 10", 17, 0},
                  {"0000 0101 01", 18, 0}, {"0000 0101 00", 19, 0}, {"0000 0100 11", 20, 0},
                  {"0000 0100 10", 21, 0}, {"0000 0100 011", 22, 0}, {"0000 0100 010", 23, 0},
                  {"0000 0100 001", 24, 0}, {"0000 0100 000", 25, 0}, {"0000 0011 111", 26, 0},
                  {"0000 0011 110", 27, 0}, {"0000 0011 101", 28, 0}, {"0000 0011 100", 29, 0},
                  {"0000 0011 011", 30, 0}, {"0000 0011 010", 31, 0}, {"0000 0011 001", 32, 0},
                  {"0000 0011 000", 33, 0}, {"0000 0001 000", 0x23, 0}, {"0000 0001 111", 0x22, 0}});
    // Macroblock type flags: 1 intra, 2 pattern, 4 backward, 8 forward, 16 quant.
    mbType[1] = build({{"1", 0x01, 0}, {"01", 0x11, 0}});
    mbType[2] = build({{"1", 0x0A, 0}, {"01", 0x02, 0}, {"001", 0x08, 0}, {"0001 1", 0x01, 0},
                       {"0001 0", 0x1A, 0}, {"0000 1", 0x12, 0}, {"0000 01", 0x11, 0}});
    mbType[3] = build({{"10", 0x0C, 0}, {"11", 0x0E, 0}, {"010", 0x04, 0}, {"011", 0x06, 0},
                       {"0010", 0x08, 0}, {"0011", 0x0A, 0}, {"0001 1", 0x01, 0}, {"0001 0", 0x1E, 0},
                       {"0000 11", 0x1A, 0}, {"0000 10", 0x16, 0}, {"0000 01", 0x11, 0}});
    mbType[0] = build({{"1", 0x01, 0}});  // D pictures
    cbp = build({{"111", 60, 0}, {"1101", 4, 0}, {"1100", 8, 0}, {"1011", 16, 0}, {"1010", 32, 0},
                 {"1001 1", 12, 0}, {"1001 0", 48, 0}, {"1000 1", 20, 0}, {"1000 0", 40, 0},
                 {"0111 1", 28, 0}, {"0111 0", 44, 0}, {"0110 1", 52, 0}, {"0110 0", 56, 0},
                 {"0101 1", 1, 0}, {"0101 0", 61, 0}, {"0100 1", 2, 0}, {"0100 0", 62, 0},
                 {"0011 11", 24, 0}, {"0011 10", 36, 0}, {"0011 01", 3, 0}, {"0011 00", 63, 0},
                 {"0010 111", 5, 0}, {"0010 110", 9, 0}, {"0010 101", 17, 0}, {"0010 100", 33, 0},
                 {"0010 011", 6, 0}, {"0010 010", 10, 0}, {"0010 001", 18, 0}, {"0010 000", 34, 0},
                 {"0001 1111", 7, 0}, {"0001 1110", 11, 0}, {"0001 1101", 19, 0}, {"0001 1100", 35, 0},
                 {"0001 1011", 13, 0}, {"0001 1010", 49, 0}, {"0001 1001", 21, 0}, {"0001 1000", 41, 0},
                 {"0001 0111", 14, 0}, {"0001 0110", 50, 0}, {"0001 0101", 22, 0}, {"0001 0100", 42, 0},
                 {"0001 0011", 15, 0}, {"0001 0010", 51, 0}, {"0001 0001", 23, 0}, {"0001 0000", 43, 0},
                 {"0000 1111", 25, 0}, {"0000 1110", 37, 0}, {"0000 1101", 26, 0}, {"0000 1100", 38, 0},
                 {"0000 1011", 29, 0}, {"0000 1010", 45, 0}, {"0000 1001", 53, 0}, {"0000 1000", 57, 0},
                 {"0000 0111", 30, 0}, {"0000 0110", 46, 0}, {"0000 0101", 54, 0}, {"0000 0100", 58, 0},
                 {"0000 0011 1", 31, 0}, {"0000 0011 0", 47, 0}, {"0000 0010 1", 55, 0},
                 {"0000 0010 0", 59, 0}, {"0000 0001 1", 27, 0}, {"0000 0001 0", 39, 0},
                 {"0000 0000 1", 0, 0}});
    // Motion code magnitudes; a sign bit follows every non-zero one.
    motion = build({{"1", 0, 0}, {"01", 1, 0}, {"001", 2, 0}, {"0001", 3, 0}, {"0000 11", 4, 0},
                    {"0000 101", 5, 0}, {"0000 100", 6, 0}, {"0000 011", 7, 0}, {"0000 0101 1", 8, 0},
                    {"0000 0101 0", 9, 0}, {"0000 0100 1", 10, 0}, {"0000 0100 01", 11, 0},
                    {"0000 0100 00", 12, 0}, {"0000 0011 11", 13, 0}, {"0000 0011 10", 14, 0},
                    {"0000 0011 01", 15, 0}, {"0000 0011 00", 16, 0}});
    dmv = build({{"0", 0, 0}, {"10", 1, 0}, {"11", -1, 0}});
    dcLuma = build({{"100", 0, 0}, {"00", 1, 0}, {"01", 2, 0}, {"101", 3, 0}, {"110", 4, 0},
                    {"1110", 5, 0}, {"1111 0", 6, 0}, {"1111 10", 7, 0}, {"1111 110", 8, 0},
                    {"1111 1110", 9, 0}, {"1111 1111 0", 10, 0}, {"1111 1111 1", 11, 0}});
    dcChroma = build({{"00", 0, 0}, {"01", 1, 0}, {"10", 2, 0}, {"110", 3, 0}, {"1110", 4, 0},
                      {"1111 0", 5, 0}, {"1111 10", 6, 0}, {"1111 110", 7, 0}, {"1111 1110", 8, 0},
                      {"1111 1111 0", 9, 0}, {"1111 1111 10", 10, 0}, {"1111 1111 11", 11, 0}});
    // DCT coefficients (run, level) without their sign bit. Table B-14; "1s" for the first
    // coefficient of a non-intra block is handled by the decoder.
    dct0 = build({{"10", kEob, 0}, {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1},
                  {"0010 1", 0, 3}, {"0011 1", 3, 1}, {"0011 0", 4, 1}, {"0001 10", 1, 2},
                  {"0001 11", 5, 1}, {"0001 01", 6, 1}, {"0001 00", 7, 1}, {"0000 110", 0, 4},
                  {"0000 100", 2, 2}, {"0000 111", 8, 1}, {"0000 101", 9, 1}, {"0000 01", kEscape, 0},
                  {"0010 0110", 0, 5}, {"0010 0001", 0, 6}, {"0010 0101", 1, 3}, {"0010 0100", 3, 2},
                  {"0010 0111", 10, 1}, {"0010 0011", 11, 1}, {"0010 0010", 12, 1}, {"0010 0000", 13, 1},
                  {"0000 0010 10", 0, 7}, {"0000 0011 00", 1, 4}, {"0000 0010 11", 2, 3},
                  {"0000 0011 11", 4, 2}, {"0000 0010 01", 5, 2}, {"0000 0011 10", 14, 1},
                  {"0000 0011 01", 15, 1}, {"0000 0010 00", 16, 1}, {"0000 0001 1101", 0, 8},
                  {"0000 0001 1000", 0, 9}, {"0000 0001 0011", 0, 10}, {"0000 0001 0000", 0, 11},
                  {"0000 0001 1011", 1, 5}, {"0000 0001 0100", 2, 4}, {"0000 0000 1101 0", 0, 12},
                  {"0000 0000 1100 1", 0, 13}, {"0000 0000 1100 0", 0, 14}, {"0000 0000 1011 1", 0, 15},
                  DCT_COMMON});
    // Table B-15 (intra_vlc_format = 1).
    dct1 = build({{"0110", kEob, 0}, {"10", 0, 1}, {"010", 1, 1}, {"110", 0, 2}, {"0010 1", 2, 1},
                  {"0111", 0, 3}, {"0011 1", 3, 1}, {"0001 10", 4, 1}, {"0011 0", 1, 2},
                  {"0001 11", 5, 1}, {"0000 110", 6, 1}, {"0000 100", 7, 1}, {"1110 0", 0, 4},
                  {"0000 111", 2, 2}, {"0000 101", 8, 1}, {"1111 000", 9, 1}, {"0000 01", kEscape, 0},
                  {"1110 1", 0, 5}, {"0001 01", 0, 6}, {"1111 001", 1, 3}, {"0010 0110", 3, 2},
                  {"1111 010", 10, 1}, {"0010 0001", 11, 1}, {"0010 0101", 12, 1}, {"0010 0100", 13, 1},
                  {"0001 00", 0, 7}, {"0010 0111", 1, 4}, {"1111 1100", 2, 3}, {"1111 1101", 4, 2},
                  {"0000 0010 0", 5, 2}, {"0000 0010 1", 14, 1}, {"0000 0011 1", 15, 1},
                  {"0000 0011 01", 16, 1}, {"1111 011", 0, 8}, {"1111 100", 0, 9}, {"0010 0011", 0, 10},
                  {"0010 0010", 0, 11}, {"0010 0000", 1, 5}, {"0000 0011 00", 2, 4},
                  {"1111 1010", 0, 12}, {"1111 1011", 0, 13}, {"1111 1110", 0, 14}, {"1111 1111", 0, 15},
                  DCT_COMMON});
  }
};
#undef DCT_COMMON

const Tables& tables() {
  static const Tables t;
  return t;
}

constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr uint8_t kAlternate[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49,
                                    41, 33, 26, 18, 3,  11, 4,  12, 19, 27, 34, 42, 50, 58, 35, 43,
                                    51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44, 52, 60, 37, 45,
                                    53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
constexpr uint8_t kDefaultIntra[64] = {8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
                                       19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
                                       22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
                                       26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
constexpr uint8_t kNonLinearQ[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                     24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

// 8x8 inverse DCT (separable, double precision: well inside the IEEE 1180 accuracy limits).
void idct(const int16_t* in, int16_t* out) {
  static const auto basis = [] {
    std::array<double, 64> b{};
    for (int u = 0; u < 8; ++u)
      for (int x = 0; x < 8; ++x)
        b[u * 8 + x] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16);
    return b;
  }();
  double tmp[64];
  for (int v = 0; v < 8; ++v)
    for (int x = 0; x < 8; ++x) {
      double s = 0;
      for (int u = 0; u < 8; ++u) s += basis[u * 8 + x] * in[v * 8 + u];
      tmp[v * 8 + x] = s;
    }
  for (int x = 0; x < 8; ++x)
    for (int y = 0; y < 8; ++y) {
      double s = 0;
      for (int v = 0; v < 8; ++v) s += basis[v * 8 + y] * tmp[v * 8 + x];
      out[y * 8 + x] = int16_t(std::clamp(roundHalfAway(s), -256, 255));
    }
}

enum : uint32_t { kCmd = 0x10002000, kCtrl = 0x10002010, kBp = 0x10002020, kTop = 0x10002030,
                  kOutFifo = 0x10007000, kInFifo = 0x10007010 };
enum : uint32_t { cBclr, cIdec, cBdec, cVdec, cFdec, cSetiq, cSetvq, cCsc, cPack, cSetth };
constexpr uint32_t kFifoQw = 8, kInternalQw = 2;
}  // namespace

// ---- Registers --------------------------------------------------------------------------------

Ipu::Ipu(Dmac& dmac) : dmac_(dmac) {
  tables();  // build (and check) the code tables up front
  std::memcpy(iq_[0], kDefaultIntra, 64);
  std::memset(iq_[1], 16, 64);
  dmac_.devices[Dmac::kIpuFrom] = this;
  dmac_.devices[Dmac::kIpuTo] = this;
}

uint32_t Ipu::aheadQw() const { return uint32_t(in_.size() / 16 - bit_ / 128); }

bool Ipu::read(uint32_t phys, uint64_t& value) {
  const uint32_t reg = phys & ~7u, shift = (phys & 4) * 8;
  uint64_t v;
  switch (reg) {
    case kCmd:
      v = data_ | uint64_t(pending_) << 63;
      break;
    case kCtrl: {
      const uint32_t ahead = aheadQw(), fp = std::min(ahead, kInternalQw);
      v = std::min(ahead - fp, kFifoQw) | std::min<uint32_t>(uint32_t(out_.size() / 16), kFifoQw) << 4 |
          cbp_ << 8 | uint32_t(ecd_) << 14 | uint32_t(scd_) << 15 | ctrl_ | uint32_t(pending_) << 31;
      break;
    }
    case kBp: {
      const uint32_t ahead = aheadQw(), fp = std::min(ahead, kInternalQw);
      v = uint32_t(bit_ % 128) | std::min(ahead - fp, kFifoQw) << 8 | fp << 16;
      break;
    }
    case kTop:
      updateTop();
      v = top_ | uint64_t(!topValid_ || pending_) << 63;
      break;
    default:
      return false;
  }
  value = v >> shift;
  return true;
}

bool Ipu::write(uint32_t phys, uint64_t value) {
  const uint32_t v = uint32_t(value);
  switch (phys) {
    case kCmd:
      cmd_ = v;
      pending_ = true;
      ecd_ = false;
      ++commands[v >> 28];
      if ((v >> 28) == cCsc) cscLeft_ = v & 0x7FF;
      run();
      return true;
    case kCtrl:
      if (v & (1u << 30)) {  // reset
        in_.clear();
        out_.clear();
        bit_ = 0;
        pending_ = false;
        data_ = 0;
        ecd_ = scd_ = false;
      }
      ctrl_ = v & 0x07FF0000;
      return true;
    case kCmd + 4: case kCtrl + 4: case kBp: case kTop:
      return true;
  }
  return false;
}

bool Ipu::read128(uint32_t phys, void* qword) {
  if (phys != kOutFifo) return false;
  uint8_t* d = static_cast<uint8_t*>(qword);
  const size_t n = std::min<size_t>(out_.size(), 16);
  std::memcpy(d, out_.front(), n);
  std::memset(d + n, 0, 16 - n);
  out_.pop(n);
  run();
  return true;
}

bool Ipu::write128(uint32_t phys, const void* qword) {
  if (phys != kInFifo) return false;
  const uint8_t* s = static_cast<const uint8_t*>(qword);
  in_.insert(in_.end(), s, s + 16);
  run();
  return true;
}

void Ipu::dmaStarted(int) {
  run();
}

// ---- Data movement ----------------------------------------------------------------------------

// Tops up the FIFO from DMA channel 4 so that `ahead` qwords are buffered.
void Ipu::fill(uint32_t ahead) {
  while (aheadQw() < ahead && dmac_.active(Dmac::kIpuTo)) {
    const uint32_t want = ahead - aheadQw();
    const size_t at = in_.size();
    in_.resize(at + want * 16);
    const uint32_t got = dmac_.pull(Dmac::kIpuTo, in_.data() + at, want);
    in_.resize(at + got * 16);
    if (!got) break;
  }
}

void Ipu::need(int bits) {
  const uint64_t end = bit_ + uint64_t(bits);
  if (end <= in_.size() * 8) return;
  fill(uint32_t((end + 127) / 128 - bit_ / 128));
  if (end > in_.size() * 8) throw Starved{};
}

uint32_t Ipu::peek(int bits) {
  if (bits == 0) return 0;
  need(bits);
  uint64_t acc = 0;
  const size_t byte = size_t(bit_ / 8);
  for (int i = 0; i < 5; ++i) acc = acc << 8 | (byte + i < in_.size() ? in_[byte + i] : 0);
  return uint32_t(acc >> (40 - (bit_ % 8) - bits)) & (bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1);
}

uint32_t Ipu::get(int bits) {
  const uint32_t v = peek(bits);
  bit_ += bits;
  return v;
}

int32_t Ipu::getSigned(int bits) {
  const uint32_t v = get(bits);
  return int32_t(v << (32 - bits)) >> (32 - bits);
}

void Ipu::output(const void* data, uint32_t bytes) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  out_.data.insert(out_.data.end(), p, p + bytes);
}

// Moves finished output to DMA channel 3, whole qwords at a time.
void Ipu::drain() {
  while (out_.size() >= 16 && dmac_.active(Dmac::kIpuFrom)) {
    const uint32_t n = std::min<uint32_t>(uint32_t(out_.size() / 16), 64);
    const uint32_t moved = dmac_.push(Dmac::kIpuFrom, out_.front(), n);
    out_.pop(size_t(moved) * 16);
    if (moved < n) break;
  }
}

void Ipu::updateTop() {
  topValid_ = false;
  try {
    top_ = peek(32);
    topValid_ = true;
  } catch (Starved) {
  }
}

// Runs the pending command if its input is there; otherwise it stays busy until more arrives.
void Ipu::run() {
  for (int guard = 0; guard < 4096 && pending_; ++guard) {
    // A command that runs out of input starts over when more arrives, so it must leave no trace:
    // the read position and the DC predictors (a BDEC updates them block by block) go back.
    const uint64_t start = bit_;
    int dcPred[3];
    std::copy(std::begin(dcPred_), std::end(dcPred_), dcPred);
    try {
      execute();
      pending_ = cscLeft_ > 0 && (cmd_ >> 28) == cCsc;
    } catch (Starved) {
      bit_ = start;
      std::copy(std::begin(dcPred), std::end(dcPred), dcPred_);
      break;
    }
  }
  // Drop the qwords already consumed and keep the FIFO topped up.
  const size_t done = size_t(bit_ / 128) * 16;
  if (done) {
    in_.erase(in_.begin(), in_.begin() + done);
    bit_ -= done * 8;
  }
  fill(kFifoQw + kInternalQw);
  drain();
}

// ---- Commands ---------------------------------------------------------------------------------

int Ipu::vlc(const VlcTable& t) {
  const VlcEntry& e = t.e[peek(16)];
  if (!e.len) {
    ecd_ = true;
    return 0;
  }
  bit_ += e.len;
  return e.value;
}

void Ipu::execute() {
  const uint32_t cmd = cmd_, fb = cmd & 0x3F;
  const Tables& tb = tables();
  switch (cmd >> 28) {
    case cBclr:  // clear the input FIFO; the next qword starts at bit BP
      in_.clear();
      bit_ = cmd & 0x7F;
      break;
    case cBdec:
      bdec(cmd);
      break;
    case cVdec: {
      skip(int(fb));
      const uint64_t from = bit_;
      int32_t value = 0;
      switch ((cmd >> 26) & 3) {
        case 0: value = vlc(*tb.mbai); break;
        case 1: value = vlc(*tb.mbType[(ctrl_ >> 24) & 3]); break;
        case 2:
          value = vlc(*tb.motion);
          if (value && !ecd_ && get(1)) value = -value;
          break;
        case 3: value = vlc(*tb.dmv); break;
      }
      data_ = uint32_t(value & 0xFFFF) | uint32_t(bit_ - from) << 16;
      break;
    }
    case cFdec:  // skip FB bits, then show (without consuming) the next 32
      skip(int(fb));
      data_ = peek(32);
      break;
    case cSetiq: {  // 64 matrix bytes in zigzag order
      skip(int(fb));
      uint8_t m[64];
      for (int i = 0; i < 64; ++i) m[i] = uint8_t(peek(8)), bit_ += 8;
      uint8_t* dst = iq_[(cmd >> 27) & 1];
      for (int i = 0; i < 64; ++i) dst[kZigzag[i]] = m[i];
      break;
    }
    case cSetvq:
      for (int i = 0; i < 16; ++i) vqclut_[i] = uint16_t(get(16));
      break;
    case cCsc:
      if (cscLeft_) csc(cmd);
      break;
    case cSetth:
      th0_ = cmd & 0x1FF;
      th1_ = (cmd >> 16) & 0x1FF;
      break;
    default:
      if (fault.empty()) fault = std::format("IPU command 0x{:08X} not supported", cmd);
      break;
  }
  updateTop();
  // Start code detection: zero bits up to the next byte, then 0x000001. Only a look ahead: the
  // read position stays where the command left it even when the input runs out mid-check.
  scd_ = false;
  const uint64_t save = bit_;
  try {
    const int pad = int((8 - bit_ % 8) % 8);
    if (peek(pad) == 0) {
      bit_ += pad;
      scd_ = peek(24) == 1;
    }
  } catch (Starved) {
  }
  bit_ = save;
}

// One block's coefficients (dequantised, in natural order); returns false on a bad code.
int Ipu::decodeBlock(int16_t* coef, int block, bool intra) {
  const Tables& tb = tables();
  std::memset(coef, 0, 64 * sizeof(int16_t));
  const uint32_t qsc = (cmd_ >> 16) & 31;
  const int qscale = (ctrl_ >> 22) & 1 ? kNonLinearQ[qsc] : int(qsc) * 2;
  const uint8_t* scan = (ctrl_ >> 20) & 1 ? kAlternate : kZigzag;
  const uint8_t* w = iq_[intra ? 0 : 1];
  int i = 0, sum = 0;
  if (intra) {
    const int cc = block < 4 ? 0 : block - 3;
    const int size = vlc(cc ? *tb.dcChroma : *tb.dcLuma);
    if (ecd_) return 0;
    int diff = 0;
    if (size) {
      diff = int(get(size));
      if (diff < (1 << (size - 1))) diff -= (1 << size) - 1;
    }
    dcPred_[cc] += diff;
    coef[0] = int16_t(dcPred_[cc] << (3 - ((ctrl_ >> 16) & 3)));
    sum = coef[0];
    i = 1;
  }
  const VlcTable& table = intra && ((ctrl_ >> 21) & 1) ? *tb.dct1 : *tb.dct0;
  for (bool first = !intra;; first = false) {
    int run, level;
    if (first && peek(1)) {  // "1s": run 0, level 1 as the first coefficient of a non-intra block
      bit_ += 1;
      run = 0;
      level = get(1) ? -1 : 1;
    } else {
      const VlcEntry& e = table.e[peek(16)];
      if (!e.len) {
        ecd_ = true;
        return 0;
      }
      bit_ += e.len;
      if (e.value == kEob) break;
      if (e.value == kEscape) {
        run = int(get(6));
        level = getSigned(12);
        if (level == 0 || level == -2048) {
          ecd_ = true;
          return 0;
        }
      } else {
        run = e.value;
        level = get(1) ? -e.level : e.level;
      }
    }
    i += run;
    if (i > 63) {
      ecd_ = true;
      return 0;
    }
    const int pos = scan[i++];
    const int mag = std::abs(level);
    int val = intra ? (mag * qscale * w[pos]) / 16 : ((2 * mag + 1) * qscale * w[pos]) / 32;
    val = std::min(level < 0 ? -val : val, 2047);
    val = std::max(val, -2048);
    coef[pos] = int16_t(val);
    sum += val;
  }
  if ((sum & 1) == 0) coef[63] ^= 1;  // mismatch control
  return 1;
}

void Ipu::bdec(uint32_t cmd) {
  skip(int(cmd & 0x3F));
  const bool intra = (cmd >> 27) & 1, fieldDct = (cmd >> 25) & 1;
  if ((cmd >> 26) & 1)  // DC reset
    for (int& p : dcPred_) p = 128 << ((ctrl_ >> 16) & 3);
  uint32_t cbp = 0x3F;
  if (!intra) {
    cbp = uint32_t(vlc(*tables().cbp));
    if (ecd_) return;
  }
  cbp_ = cbp;
  int16_t mb[384] = {};  // Y 16x16, Cb 8x8, Cr 8x8
  for (int b = 0; b < 6; ++b) {
    if (!(cbp & (0x20 >> b))) continue;
    int16_t coef[64], pix[64];
    if (!decodeBlock(coef, b, intra)) return;
    idct(coef, pix);
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) {
        int16_t* dst;
        if (b < 4) {
          const int row = fieldDct ? 2 * y + (b >> 1) : y + 8 * (b >> 1);
          dst = &mb[row * 16 + 8 * (b & 1) + x];
        } else {
          dst = &mb[256 + 64 * (b - 4) + y * 8 + x];
        }
        *dst = pix[y * 8 + x];
      }
  }
  ++macroblocks;
  output(mb, sizeof mb);
}

// YCbCr (RAW8: Y 16x16, Cb 8x8, Cr 8x8 bytes) to RGB32 or RGB16, one macroblock at a time.
void Ipu::csc(uint32_t cmd) {
  const bool rgb16 = (cmd >> 27) & 1;
  if (bit_ % 8) bit_ += 8 - bit_ % 8;
  need(384 * 8);
  const uint8_t* src = in_.data() + bit_ / 8;
  uint32_t px[256];
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      const double Y = 1.164 * (src[y * 16 + x] - 16);
      const double cb = src[256 + (y / 2) * 8 + x / 2] - 128.0, cr = src[320 + (y / 2) * 8 + x / 2] - 128.0;
      const int r = std::clamp(roundHalfAway(Y + 1.596 * cr), 0, 255);
      const int g = std::clamp(roundHalfAway(Y - 0.813 * cr - 0.391 * cb), 0, 255);
      const int b = std::clamp(roundHalfAway(Y + 2.018 * cb), 0, 255);
      uint32_t a = 0x80;
      if (uint32_t(r) < th0_ && uint32_t(g) < th0_ && uint32_t(b) < th0_)
        a = 0;
      else if (uint32_t(r) < th1_ && uint32_t(g) < th1_ && uint32_t(b) < th1_)
        a = 0x40;
      px[y * 16 + x] = uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | a << 24;
    }
  bit_ += 384 * 8;
  if (rgb16) {
    uint16_t p16[256];
    for (int i = 0; i < 256; ++i) {
      const uint32_t c = px[i];
      p16[i] = uint16_t((c >> 3 & 0x1F) | (c >> 11 & 0x1F) << 5 | (c >> 19 & 0x1F) << 10 | (c >> 31) << 15);
    }
    output(p16, sizeof p16);
  } else {
    output(px, sizeof px);
  }
  ++macroblocks;
  if (cscLeft_) --cscLeft_;
}
