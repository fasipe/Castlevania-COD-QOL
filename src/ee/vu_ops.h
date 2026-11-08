#pragma once
// The VU micro-mode instruction handlers and pair sequencing, shared by the interpreter (vu.cpp)
// and the micro programs compiled ahead of time (recomp/vu), so both behave identically.
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <immintrin.h>

#include "ee/vu.h"

namespace vuops {
// Lanes enabled by a dest field (bit 3 = x = lane 0), and 4-bit reversal (lane order to flag order).
inline const __m128i kDestMask[16] = {
#define M(d) _mm_setr_epi32((d) & 8 ? -1 : 0, (d) & 4 ? -1 : 0, (d) & 2 ? -1 : 0, (d) & 1 ? -1 : 0)
    M(0), M(1), M(2), M(3), M(4), M(5), M(6), M(7), M(8), M(9), M(10), M(11), M(12), M(13), M(14), M(15)
#undef M
};
inline constexpr uint8_t kReverse4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

// clampf for four lanes.
inline __m128 clampv(__m128 v) {
  const __m128i b = _mm_castps_si128(v);
  const __m128i infinite = _mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F800000));
  const __m128i saturated = _mm_or_si128(_mm_and_si128(b, _mm_set1_epi32(int(0x80000000u))), _mm_set1_epi32(0x7F7FFFFF));
  return _mm_castsi128_ps(_mm_blendv_epi8(b, saturated, infinite));
}

// VU floats have no infinities: overflowing results saturate to the largest finite value. Bit
// patterns with the top exponent are ordinary huge numbers to the VU; x86 passes them through
// arithmetic unchanged (as NaNs), which is what programs using them as bit masks expect (e.g.
// I = 0xFFFF8000 added to 1.0 must keep its low bits).
inline float clampf(float f) {
  const uint32_t b = std::bit_cast<uint32_t>(f);
  if ((b & 0x7FFFFFFFu) == 0x7F800000u) return std::bit_cast<float>((b & 0x80000000u) | 0x7F7FFFFFu);
  return f;
}
inline float asFloat(uint32_t v) { return std::bit_cast<float>(v); }

// MAC zero/sign flags of the enabled lanes of a result, and the status bits derived from them.
inline uint32_t macFlags(__m128 v, __m128 mask) {
  const int zero = _mm_movemask_ps(_mm_and_ps(_mm_cmpeq_ps(v, _mm_setzero_ps()), mask));
  const int sign = _mm_movemask_ps(_mm_and_ps(_mm_cmplt_ps(v, _mm_setzero_ps()), mask));
  return kReverse4[zero] | kReverse4[sign] << 4;
}
// Sets MAC and the status bits derived from it (zero/sign, and their sticky copies).
inline void writeMac(uint32_t* vi, uint32_t mac) {
  vi[kVuMac] = mac;
  uint32_t st = vi[kVuStatus] & ~0xFu;
  if (mac & 0xF) st |= 1;
  if (mac & 0xF0) st |= 2;
  vi[kVuStatus] = st | ((st & 0xF) << 6);
}
inline void setFlags(uint32_t* vi, __m128 v, __m128 mask) { writeMac(vi, macFlags(v, mask)); }
inline uint32_t asBits(float f) { return std::bit_cast<uint32_t>(f); }
inline uint32_t divide(float n, float d) {
  if (d == 0.0f) return asBits(std::signbit(n) != std::signbit(d) ? -FLT_MAX : FLT_MAX);
  return asBits(clampf(n / d));
}
enum { kAdd, kSub, kMul, kMadd, kMsub, kMax, kMin };  // upper arithmetic
enum { kBc, kFt, kI, kQ };                            // its second operand

// Upper instructions that write MAC/status (arithmetic other than MAX/MINI, OPMULA/OPMSUB).
inline bool upperSetsFlags(uint32_t hi) {
  const uint32_t funct = hi & 63;
  if (funct < 0x3C) return funct <= 0x2F && !(funct >= 0x10 && funct <= 0x17) && funct != 0x1D && funct != 0x1F &&
                           funct != 0x2B && funct != 0x2F;
  const uint32_t fn2 = ((hi >> 4) & 0x7C) | (hi & 3);
  return fn2 <= 0x2E && !(fn2 >= 0x10 && fn2 <= 0x17) && fn2 != 0x1D && fn2 != 0x1F && fn2 != 0x2B;
}
// Lower instructions that read MAC/status (FMEQ/FMAND/FMOR, FSxxx).
inline bool lowerReadsFlags(uint32_t lo) {
  const uint32_t opc = lo >> 25;
  return opc == 0x18 || opc == 0x1A || opc == 0x1B || (opc >= 0x14 && opc <= 0x17);
}
inline bool lowerBranches(uint32_t lo) { return (lo >> 25) >= 0x20 && (lo >> 25) <= 0x2F; }
// Lower instructions that touch no vector register.
inline bool lowerVectorFree(uint32_t lo) {
  const uint32_t opc = lo >> 25;
  if (opc >= 0x04 && opc <= 0x2F) return true;
  if (opc != 0x40) return false;
  if ((lo & 63) >= 0x30 && (lo & 63) <= 0x35) return true;
  const uint32_t fn2 = ((lo >> 4) & 0x7C) | (lo & 3);
  if (fn2 == 0x30 || fn2 == 0x31) return ((lo >> 21) & 0xF) == 0 || ((lo >> 16) & 31) == 0;  // MOVE/MR32 to nothing
  return fn2 == 0x3B || fn2 == 0x3E || fn2 == 0x3F || fn2 == 0x68 || fn2 == 0x69 || fn2 == 0x6C || fn2 == 0x7B;
}
}  // namespace vuops

using namespace vuops;

// Executes an upper (FMAC) instruction; false if `op` is not one. Always inlined: with `op` known
// at compile time (compiled VU blocks and macro-mode code) only that instruction's code remains.
__forceinline bool vuUpper(VuRegs r, uint32_t op) {
  const uint32_t dest = (op >> 21) & 0xF;  // bit 3 = x ... bit 0 = w
  const uint32_t ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  u128* vf = r.vf;
  uint32_t* vi = r.vi;
  u128& acc = *r.acc;
  auto on = [&](int i) { return (dest >> (3 - i)) & 1; };

  // Writes `res` (saturated) under the dest mask and updates the MAC/status zero and sign flags,
  // which programs read back (FMAND, CFC2) to compare vectors. Four lanes at once.
  const __m128i mask = kDestMask[dest];
  auto storev = [&](u128& dst, __m128 res, bool flags) {
    const __m128 v = clampv(res);
    if (&dst != &vf[0]) {
      const __m128 old = _mm_loadu_ps(dst.f);
      _mm_storeu_ps(dst.f, _mm_blendv_ps(old, v, _mm_castsi128_ps(mask)));
    }
    if (flags) setFlags(vi, v, _mm_castsi128_ps(mask));
  };
  auto store = [&](u128& dst, const float res[4], bool flags) { storev(dst, _mm_loadu_ps(res), flags); };
  enum class Op { Add, Sub, Mul, Madd, Msub, Max, Min };
  auto arith = [&](Op o, u128& dst, __m128 y) {
    const __m128 x = _mm_loadu_ps(vf[fs].f);
    __m128 res;
    switch (o) {
      case Op::Add: res = _mm_add_ps(x, y); break;
      case Op::Sub: res = _mm_sub_ps(x, y); break;
      case Op::Mul: res = _mm_mul_ps(x, y); break;
      case Op::Madd: res = _mm_add_ps(_mm_loadu_ps(acc.f), _mm_mul_ps(x, y)); break;
      case Op::Msub: res = _mm_sub_ps(_mm_loadu_ps(acc.f), _mm_mul_ps(x, y)); break;
      case Op::Max: res = _mm_max_ps(y, x); break;  // operand order as std::max(x, y)
      default: res = _mm_min_ps(y, x); break;       // as std::min(x, y)
    }
    storev(dst, res, o != Op::Max && o != Op::Min);
  };
  const __m128 iAt = _mm_set1_ps(asFloat(vi[kVuI])), qAt = _mm_set1_ps(asFloat(vi[kVuQ]));
  const __m128 bcAt = _mm_set1_ps(vf[ft].f[op & 3]);
  const __m128 ftAt = _mm_loadu_ps(vf[ft].f);

  const uint32_t funct = op & 63;
  if (funct < 0x3C) {
    switch (funct) {
      case 0x00: case 0x01: case 0x02: case 0x03: arith(Op::Add, vf[fd], bcAt); return true;
      case 0x04: case 0x05: case 0x06: case 0x07: arith(Op::Sub, vf[fd], bcAt); return true;
      case 0x08: case 0x09: case 0x0A: case 0x0B: arith(Op::Madd, vf[fd], bcAt); return true;
      case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(Op::Msub, vf[fd], bcAt); return true;
      case 0x10: case 0x11: case 0x12: case 0x13: arith(Op::Max, vf[fd], bcAt); return true;
      case 0x14: case 0x15: case 0x16: case 0x17: arith(Op::Min, vf[fd], bcAt); return true;
      case 0x18: case 0x19: case 0x1A: case 0x1B: arith(Op::Mul, vf[fd], bcAt); return true;
      case 0x1C: arith(Op::Mul, vf[fd], qAt); return true;
      case 0x1D: arith(Op::Max, vf[fd], iAt); return true;
      case 0x1E: arith(Op::Mul, vf[fd], iAt); return true;
      case 0x1F: arith(Op::Min, vf[fd], iAt); return true;
      case 0x20: arith(Op::Add, vf[fd], qAt); return true;
      case 0x21: arith(Op::Madd, vf[fd], qAt); return true;
      case 0x22: arith(Op::Add, vf[fd], iAt); return true;
      case 0x23: arith(Op::Madd, vf[fd], iAt); return true;
      case 0x24: arith(Op::Sub, vf[fd], qAt); return true;
      case 0x25: arith(Op::Msub, vf[fd], qAt); return true;
      case 0x26: arith(Op::Sub, vf[fd], iAt); return true;
      case 0x27: arith(Op::Msub, vf[fd], iAt); return true;
      case 0x28: arith(Op::Add, vf[fd], ftAt); return true;
      case 0x29: arith(Op::Madd, vf[fd], ftAt); return true;
      case 0x2A: arith(Op::Mul, vf[fd], ftAt); return true;
      case 0x2B: arith(Op::Max, vf[fd], ftAt); return true;
      case 0x2C: arith(Op::Sub, vf[fd], ftAt); return true;
      case 0x2D: arith(Op::Msub, vf[fd], ftAt); return true;
      case 0x2E: {  // OPMSUB: fd.xyz = ACC.xyz - fs.yzx * ft.zxy
        const float res[4] = {acc.f[0] - vf[fs].f[1] * vf[ft].f[2], acc.f[1] - vf[fs].f[2] * vf[ft].f[0],
                              acc.f[2] - vf[fs].f[0] * vf[ft].f[1], 0};
        store(vf[fd], res, true);
        return true;
      }
      case 0x2F: arith(Op::Min, vf[fd], ftAt); return true;
    }
    return false;
  }

  const uint32_t fn2 = ((op >> 4) & 0x7C) | (op & 3);
  switch (fn2) {
    case 0x00: case 0x01: case 0x02: case 0x03: arith(Op::Add, acc, bcAt); return true;
    case 0x04: case 0x05: case 0x06: case 0x07: arith(Op::Sub, acc, bcAt); return true;
    case 0x08: case 0x09: case 0x0A: case 0x0B: arith(Op::Madd, acc, bcAt); return true;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(Op::Msub, acc, bcAt); return true;
    case 0x10: case 0x11: case 0x12: case 0x13: {  // ITOFn: fixed point to float
      static constexpr float kScale[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
      const u128 src = vf[fs];
      for (int i = 0; i < 4; ++i)
        if (on(i) && ft) vf[ft].f[i] = float(src.sw[i]) * kScale[fn2 & 3];
      return true;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {  // FTOIn: float to fixed point, truncating, saturating
      static constexpr float kScale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
      const u128 src = vf[fs];
      for (int i = 0; i < 4; ++i) {
        if (!on(i) || !ft) continue;
        const double v = double(src.f[i]) * kScale[fn2 & 3];
        vf[ft].sw[i] = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : int32_t(v);
      }
      return true;
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: arith(Op::Mul, acc, bcAt); return true;
    case 0x1C: arith(Op::Mul, acc, qAt); return true;
    case 0x1D: {  // ABS
      const float res[4] = {std::fabs(vf[fs].f[0]), std::fabs(vf[fs].f[1]), std::fabs(vf[fs].f[2]), std::fabs(vf[fs].f[3])};
      store(vf[ft], res, false);
      return true;
    }
    case 0x1E: arith(Op::Mul, acc, iAt); return true;
    case 0x1F: {  // CLIPw.xyz: 6 judgement bits per call, the last four calls kept
      const float w = std::fabs(vf[ft].f[3]);
      uint32_t b = 0;
      for (int i = 0; i < 3; ++i) {
        if (vf[fs].f[i] > w) b |= 1u << (2 * i);
        if (vf[fs].f[i] < -w) b |= 2u << (2 * i);
      }
      vi[kVuClip] = ((vi[kVuClip] << 6) | b) & 0xFFFFFF;
      return true;
    }
    case 0x20: arith(Op::Add, acc, qAt); return true;
    case 0x21: arith(Op::Madd, acc, qAt); return true;
    case 0x22: arith(Op::Add, acc, iAt); return true;
    case 0x23: arith(Op::Madd, acc, iAt); return true;
    case 0x24: arith(Op::Sub, acc, qAt); return true;
    case 0x25: arith(Op::Msub, acc, qAt); return true;
    case 0x26: arith(Op::Sub, acc, iAt); return true;
    case 0x27: arith(Op::Msub, acc, iAt); return true;
    case 0x28: arith(Op::Add, acc, ftAt); return true;
    case 0x29: arith(Op::Madd, acc, ftAt); return true;
    case 0x2A: arith(Op::Mul, acc, ftAt); return true;
    case 0x2C: arith(Op::Sub, acc, ftAt); return true;
    case 0x2D: arith(Op::Msub, acc, ftAt); return true;
    case 0x2E: {  // OPMULA: ACC.xyz = fs.yzx * ft.zxy
      const float res[4] = {vf[fs].f[1] * vf[ft].f[2], vf[fs].f[2] * vf[ft].f[0], vf[fs].f[0] * vf[ft].f[1], 0};
      store(acc, res, true);
      return true;
    }
    case 0x2F: return true;  // NOP
  }
  return false;
}

template <int O, int B, bool ToAcc>
__forceinline __m128 VuMicro::upperArithStore(VuMicro& m, const Pair& p, __m128& mask) {
  u128* vf = m.r_.vf;
  const __m128 x = _mm_loadu_ps(vf[p.fs].f);
  __m128 y;
  if constexpr (B == kBc) y = _mm_set1_ps(vf[p.ft].f[p.bc]);
  else if constexpr (B == kFt) y = _mm_loadu_ps(vf[p.ft].f);
  else if constexpr (B == kI) y = _mm_set1_ps(asFloat(m.r_.vi[kVuI]));
  else y = _mm_set1_ps(asFloat(m.r_.vi[kVuQ]));
  __m128 res;
  if constexpr (O == kAdd) res = _mm_add_ps(x, y);
  else if constexpr (O == kSub) res = _mm_sub_ps(x, y);
  else if constexpr (O == kMul) res = _mm_mul_ps(x, y);
  else if constexpr (O == kMadd) res = _mm_add_ps(_mm_loadu_ps(m.r_.acc->f), _mm_mul_ps(x, y));
  else if constexpr (O == kMsub) res = _mm_sub_ps(_mm_loadu_ps(m.r_.acc->f), _mm_mul_ps(x, y));
  else if constexpr (O == kMax) res = _mm_max_ps(y, x);  // operand order as std::max(x, y)
  else res = _mm_min_ps(y, x);
  const __m128 v = clampv(res);
  mask = _mm_castsi128_ps(kDestMask[p.dest]);
  float* dst = ToAcc ? m.r_.acc->f : vf[p.fd].f;
  if (ToAcc || p.fd) _mm_storeu_ps(dst, _mm_blendv_ps(_mm_loadu_ps(dst), v, mask));
  return v;
}

template <int O, int B, bool ToAcc>
void VuMicro::upperArith(VuMicro& m, const Pair& p) {
  __m128 mask;
  const __m128 v = upperArithStore<O, B, ToAcc>(m, p, mask);
  if constexpr (O != kMax && O != kMin)
    if (p.flags) m.delayFlags(macFlags(v, mask));
}

template <int O, int B, bool ToAcc>
uint32_t VuMicro::upperArithMac(VuMicro& m, const Pair& p) {
  __m128 mask;
  const __m128 v = upperArithStore<O, B, ToAcc>(m, p, mask);
  return macFlags(v, mask);
}

inline uint32_t VuMicro::upperGenericMac(VuMicro& m, const Pair& p) {
  uint32_t* vi = m.r_.vi;
  const uint32_t mac = vi[kVuMac], status = vi[kVuStatus];
  if (!vuUpper(m.r_, p.hi)) m.fault = std::format("upper instruction 0x{:08X}", p.hi);
  const uint32_t result = vi[kVuMac];
  vi[kVuMac] = mac, vi[kVuStatus] = status;
  return result;
}

__forceinline void VuMicro::upperGeneric(VuMicro& m, const Pair& p) {
  uint32_t* vi = m.r_.vi;
  const uint32_t mac = vi[kVuMac], status = vi[kVuStatus];
  if (!vuUpper(m.r_, p.hi)) m.fault = std::format("upper instruction 0x{:08X}", p.hi);
  if (upperSetsFlags(p.hi)) {  // its flags arrive later like everyone's
    const uint32_t result = vi[kVuMac];
    vi[kVuMac] = mac, vi[kVuStatus] = status;
    m.delayFlags(result);
  }
}

// FMAC results reach the MAC and status flags four cycles (pairs) after the instruction; flag
// reads in between see the older values, which pipelined programs count on.
inline void VuMicro::delayFlags(uint32_t mac) {
  PendingFlags& f = pendingFlags_[cycle_ & 3];
  if (f.valid) writeMac(r_.vi, f.mac);  // only reachable if the program ended and restarted
  f = {cycle_ + kFlagLatency, mac, true};
}

// Each pair: only the result pushed four pairs ago can be due (one push per pair).
inline void VuMicro::settleDue() {
  PendingFlags& f = pendingFlags_[cycle_ & 3];
  if (f.valid && f.at <= cycle_) {
    writeMac(r_.vi, f.mac);
    f.valid = false;
  }
}

inline void VuMicro::settleAt(uint64_t cycle) {
  PendingFlags& f = pendingFlags_[cycle & 3];
  if (f.valid && f.at <= cycle) {
    writeMac(r_.vi, f.mac);
    f.valid = false;
  }
}

inline void VuMicro::settleUpTo(uint64_t cycle) {
  for (int k = 0; k < 4; ++k) {  // oldest first
    PendingFlags& f = pendingFlags_[(cycle + uint64_t(k)) & 3];
    if (f.valid && f.at <= cycle) {
      writeMac(r_.vi, f.mac);
      f.valid = false;
    }
  }
}

inline void VuMicro::settleFlags(bool all) {
  for (int k = 0; k < 4; ++k) {
    // Oldest first, so the latest result wins.
    PendingFlags& f = pendingFlags_[(cycle_ + uint64_t(k)) & 3];
    if (f.valid && (all || f.at <= cycle_)) {
      writeMac(r_.vi, f.mac);
      f.valid = false;
    }
  }
}

__forceinline void VuMicro::lowerLq(VuMicro& m, const Pair& p, uint32_t) {
  const uint8_t* q = m.data_ + ((m.r_.vi[p.is] + p.imm) & (m.dataBytes_ / 16 - 1)) * 16;
  u128& d = m.r_.vf[p.lft];
  _mm_storeu_ps(d.f, _mm_blendv_ps(_mm_loadu_ps(d.f), _mm_loadu_ps(reinterpret_cast<const float*>(q)), _mm_castsi128_ps(kDestMask[p.ldest])));
}
__forceinline void VuMicro::lowerSq(VuMicro& m, const Pair& p, uint32_t) {
  uint8_t* q = m.data_ + ((m.r_.vi[p.it] + p.imm) & (m.dataBytes_ / 16 - 1)) * 16;
  float* d = reinterpret_cast<float*>(q);
  _mm_storeu_ps(d, _mm_blendv_ps(_mm_loadu_ps(d), _mm_loadu_ps(m.r_.vf[p.lfs].f), _mm_castsi128_ps(kDestMask[p.ldest])));
}
__forceinline void VuMicro::lowerLqi(VuMicro& m, const Pair& p, uint32_t) {
  uint32_t* vi = m.r_.vi;
  const uint8_t* q = m.data_ + (vi[p.is] & (m.dataBytes_ / 16 - 1)) * 16;
  u128& d = m.r_.vf[p.lft];
  _mm_storeu_ps(d.f, _mm_blendv_ps(_mm_loadu_ps(d.f), _mm_loadu_ps(reinterpret_cast<const float*>(q)), _mm_castsi128_ps(kDestMask[p.ldest])));
  if (p.is) vi[p.is] = (vi[p.is] + 1) & 0xFFFF;
}
__forceinline void VuMicro::lowerSqi(VuMicro& m, const Pair& p, uint32_t) {
  uint32_t* vi = m.r_.vi;
  float* d = reinterpret_cast<float*>(m.data_ + (vi[p.it] & (m.dataBytes_ / 16 - 1)) * 16);
  _mm_storeu_ps(d, _mm_blendv_ps(_mm_loadu_ps(d), _mm_loadu_ps(m.r_.vf[p.lfs].f), _mm_castsi128_ps(kDestMask[p.ldest])));
  if (p.it) vi[p.it] = (vi[p.it] + 1) & 0xFFFF;
}
__forceinline void VuMicro::lowerIaddiu(VuMicro& m, const Pair& p, uint32_t) {
  if (p.it) m.r_.vi[p.it] = (m.r_.vi[p.is] + p.imm) & 0xFFFF;
}
__forceinline void VuMicro::lowerIsubiu(VuMicro& m, const Pair& p, uint32_t) {
  if (p.it) m.r_.vi[p.it] = (m.r_.vi[p.is] - p.imm) & 0xFFFF;
}
__forceinline void VuMicro::lowerIadd(VuMicro& m, const Pair& p, uint32_t) {
  const uint32_t id = (p.lo >> 6) & 15;
  if (id) m.r_.vi[id] = (m.r_.vi[p.is] + m.r_.vi[p.it]) & 0xFFFF;
}
__forceinline void VuMicro::lowerIaddi(VuMicro& m, const Pair& p, uint32_t) {
  if (p.it) m.r_.vi[p.it] = (m.r_.vi[p.is] + p.imm) & 0xFFFF;
}
__forceinline void VuMicro::lowerMove(VuMicro& m, const Pair& p, uint32_t) {
  u128& d = m.r_.vf[p.lft];
  _mm_storeu_ps(d.f, _mm_blendv_ps(_mm_loadu_ps(d.f), _mm_loadu_ps(m.r_.vf[p.lfs].f), _mm_castsi128_ps(kDestMask[p.ldest])));
}

template <int Scale>
void VuMicro::upperFtoi(VuMicro& m, const Pair& p) {  // float to fixed point (4, 12, 15 fraction bits)
  if (!p.ft) return;
  const __m128 x = _mm_mul_ps(_mm_loadu_ps(m.r_.vf[p.fs].f), _mm_set1_ps(float(1 << Scale)));
  __m128i v = _mm_cvttps_epi32(x);  // INT_MIN when out of range (and for top-exponent patterns)
  v = _mm_blendv_epi8(v, _mm_set1_epi32(INT32_MAX), _mm_castps_si128(_mm_cmpge_ps(x, _mm_set1_ps(2147483648.0f))));
  float* d = m.r_.vf[p.ft].f;
  _mm_storeu_ps(d, _mm_blendv_ps(_mm_loadu_ps(d), _mm_castsi128_ps(v), _mm_castsi128_ps(kDestMask[p.dest])));
}

template <int Scale>
void VuMicro::upperItof(VuMicro& m, const Pair& p) {  // fixed point to float
  if (!p.ft) return;
  const __m128 x = _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(m.r_.vf[p.fs].f))),
                              _mm_set1_ps(1.0f / float(1 << Scale)));
  float* d = m.r_.vf[p.ft].f;
  _mm_storeu_ps(d, _mm_blendv_ps(_mm_loadu_ps(d), x, _mm_castsi128_ps(kDestMask[p.dest])));
}

__forceinline void VuMicro::upperClip(VuMicro& m, const Pair& p) {  // CLIPw.xyz: 6 judgement bits, the last four kept
  const float w = std::fabs(m.r_.vf[p.ft].f[3]);
  const __m128 x = _mm_loadu_ps(m.r_.vf[p.fs].f);
  const int above = _mm_movemask_ps(_mm_cmpgt_ps(x, _mm_set1_ps(w))), below = _mm_movemask_ps(_mm_cmplt_ps(x, _mm_set1_ps(-w)));
  const uint32_t b = (above & 1) | (below & 1) << 1 | (above & 2) << 1 | (below & 2) << 2 | (above & 4) << 2 | (below & 4) << 3;
  uint32_t& clip = m.r_.vi[kVuClip];
  clip = ((clip << 6) | b) & 0xFFFFFF;
}

__forceinline void VuMicro::upperAbs(VuMicro& m, const Pair& p) {
  const __m128 v = clampv(_mm_andnot_ps(_mm_set1_ps(-0.0f), _mm_loadu_ps(m.r_.vf[p.fs].f)));
  if (!p.ft) return;
  float* d = m.r_.vf[p.ft].f;
  _mm_storeu_ps(d, _mm_blendv_ps(_mm_loadu_ps(d), v, _mm_castsi128_ps(kDestMask[p.dest])));
}

__forceinline void VuMicro::lowerBranch(VuMicro& m, const Pair& p, uint32_t pc) {
  const uint32_t* vi = m.r_.vi;
  const uint32_t target = pc + 8 + uint32_t(p.imm) * 8;
  switch (p.lo >> 25) {
    case 0x20: m.branchTo(target); return;  // B
    case 0x21:                             // BAL
      if (p.it) m.r_.vi[p.it] = ((pc + 16) / 8) & 0xFFFF;
      m.branchTo(target);
      return;
    case 0x24: m.branchTo((vi[p.is] & 0xFFFF) * 8); return;  // JR
    case 0x25: {                                             // JALR
      const uint32_t to = (vi[p.is] & 0xFFFF) * 8;
      if (p.it) m.r_.vi[p.it] = ((pc + 16) / 8) & 0xFFFF;
      m.branchTo(to);
      return;
    }
    case 0x28: if ((vi[p.it] & 0xFFFF) == (vi[p.is] & 0xFFFF)) m.branchTo(target); return;  // IBEQ
    case 0x29: if ((vi[p.it] & 0xFFFF) != (vi[p.is] & 0xFFFF)) m.branchTo(target); return;  // IBNE
    case 0x2C: if (int16_t(vi[p.is]) < 0) m.branchTo(target); return;                       // IBLTZ
    case 0x2D: if (int16_t(vi[p.is]) > 0) m.branchTo(target); return;                       // IBGTZ
    case 0x2E: if (int16_t(vi[p.is]) <= 0) m.branchTo(target); return;                      // IBLEZ
    case 0x2F: if (int16_t(vi[p.is]) >= 0) m.branchTo(target); return;                      // IBGEZ
  }
}

// Integer and flag instructions of the first decode table (as in lower()).
__forceinline void VuMicro::lowerInteger(VuMicro& m, const Pair& p, uint32_t) {
  uint32_t* vi = m.r_.vi;
  const uint32_t lo = p.lo;
  auto setI = [&](uint32_t reg, uint32_t v) {
    if (reg) vi[reg] = v & 0xFFFF;
  };
  auto qword = [&](uint32_t index) { return m.data_ + (index & (m.dataBytes_ / 16 - 1)) * 16; };
  switch (lo >> 25) {
    case 0x04: {  // ILW
      const int lane = p.ldest & 8 ? 0 : p.ldest & 4 ? 1 : p.ldest & 2 ? 2 : 3;
      uint32_t v;
      std::memcpy(&v, qword(vi[p.is] + p.imm) + 4 * lane, 4);
      setI(p.it, v);
      return;
    }
    case 0x05: {  // ISW
      uint8_t* q = qword(vi[p.is] + p.imm);
      const uint32_t v = vi[p.it] & 0xFFFF;
      for (int i = 0; i < 4; ++i)
        if ((p.ldest >> (3 - i)) & 1) std::memcpy(q + 4 * i, &v, 4);
      return;
    }
    case 0x10: vi[1] = (vi[kVuClip] & 0xFFFFFF) == (lo & 0xFFFFFF); return;          // FCEQ
    case 0x11: vi[kVuClip] = lo & 0xFFFFFF; return;                                  // FCSET
    case 0x12: vi[1] = (vi[kVuClip] & lo & 0xFFFFFF) != 0; return;                   // FCAND
    case 0x13: vi[1] = ((vi[kVuClip] | lo) & 0xFFFFFF) == 0xFFFFFF; return;          // FCOR
    case 0x18: setI(p.it, (vi[kVuMac] & 0xFFFF) == (vi[p.is] & 0xFFFF)); return;     // FMEQ
    case 0x1A: setI(p.it, vi[kVuMac] & vi[p.is]); return;                             // FMAND
    case 0x1B: setI(p.it, (vi[kVuMac] | vi[p.is]) == 0xFFFF); return;                 // FMOR
    case 0x1C: setI(p.it, vi[kVuClip] & 0xFFF); return;                               // FCGET
  }
  const uint32_t id = (lo >> 6) & 15;
  switch (lo & 63) {
    case 0x31: setI(id, vi[p.is] - vi[p.it]); return;  // ISUB
    case 0x34: setI(id, vi[p.is] & vi[p.it]); return;  // IAND
    case 0x35: setI(id, vi[p.is] | vi[p.it]); return;  // IOR
  }
}

__forceinline void VuMicro::lowerMfir(VuMicro& m, const Pair& p, uint32_t) {
  const int32_t v = int16_t(m.r_.vi[p.is]);
  float* d = m.r_.vf[p.lft].f;
  _mm_storeu_ps(d, _mm_blendv_ps(_mm_loadu_ps(d), _mm_castsi128_ps(_mm_set1_epi32(v)), _mm_castsi128_ps(kDestMask[p.ldest])));
}

__forceinline void VuMicro::lowerMtir(VuMicro& m, const Pair& p, uint32_t) {
  if (p.it) m.r_.vi[p.it] = m.r_.vf[p.lfs].uw[(p.lo >> 21) & 3] & 0xFFFF;
}

inline void VuMicro::lowerDiv(VuMicro& m, const Pair& p, uint32_t) {  // DIV / SQRT / RSQRT: Q arrives later
  vuLowerCommon(m.r_, p.lo, m.data_, m.dataBytes_ / 16, &m.pendingQ_);
  m.qDelay_ = qLatency(p.lo);
}

inline void VuMicro::lowerXgkick(VuMicro& m, const Pair& p, uint32_t) {
  if (m.xgkick) m.xgkick(m.r_.vi[p.is] & 0xFFFF);
}

// Start of every pair: counters, flag results arriving, Q and P results arriving.
inline void VuMicro::pairStart() {
  ++instructions;
  ++cycle_;
  settleDue();
  if (qDelay_ > 0 && --qDelay_ == 0) r_.vi[kVuQ] = pendingQ_;
  if (pDelay_ > 0 && --pDelay_ == 0) p_ = pendingP_;
}

// A pair stalls as a whole: an upper instruction next to WAITQ/WAITP already sees the result.
inline void VuMicro::pairStall(bool waitQ, bool waitP) {
  if (!((waitQ && qDelay_ > 0) || (waitP && pDelay_ > 0))) return;
  const int stall = std::max(waitQ ? qDelay_ : 0, waitP ? pDelay_ : 0);
  if (qDelay_ > 0 && (waitQ || qDelay_ <= stall)) r_.vi[kVuQ] = pendingQ_, qDelay_ = 0;
  else if (qDelay_ > 0) qDelay_ -= stall;
  if (pDelay_ > 0 && (waitP || pDelay_ <= stall)) p_ = pendingP_, pDelay_ = 0;
  else if (pDelay_ > 0) pDelay_ -= stall;
  cycle_ += uint64_t(stall);  // flag results keep arriving meanwhile
  settleFlags(false);
}
