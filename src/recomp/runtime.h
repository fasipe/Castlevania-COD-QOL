#pragma once
// Support for the statically recompiled game code (recomp/gen/*.cpp, written by
// `codport --recompile`). Each recompiled function runs from one of its entry points until it
// has to leave (a call, a return, an instruction the interpreter must execute, or a loop that
// has run long enough for interrupts to be checked) and returns the address to continue at.
#include <cstdint>
#include <cstring>
#include <immintrin.h>

#include "ee/cpu.h"
#include "ee/memory.h"
#include "ee/vu_ops.h"

namespace rc {

using Fn = uint32_t (*)(Cpu& c, uint32_t entry);

struct Entry {
  uint32_t pc;
  Fn fn;
};

inline uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

// 128-bit registers for the MMI (multimedia) instructions, which map onto SSE.
inline __m128i ld(const CpuState& s, uint32_t r) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(&s.gpr[r])); }
inline void st(CpuState& s, uint32_t r, __m128i v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(&s.gpr[r]), v); }
// The low 16 (or 8) bits of each 32-bit (16-bit) lane of b, then of a (PPACH, PPACB).
inline __m128i pack16(__m128i a, __m128i b) {
  const __m128i m = _mm_set1_epi32(0xFFFF);
  return _mm_packus_epi32(_mm_and_si128(b, m), _mm_and_si128(a, m));
}
inline __m128i pack8(__m128i a, __m128i b) {
  const __m128i m = _mm_set1_epi16(0xFF);
  return _mm_packus_epi16(_mm_and_si128(b, m), _mm_and_si128(a, m));
}
// QFSRV: the low 128 bits of (a:b) shifted right by SA bits.
inline __m128i qfsrv(const CpuState& s, __m128i a, __m128i b) {
  uint64_t w[4];
  _mm_storeu_si128(reinterpret_cast<__m128i*>(w), b);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(w + 2), a);
  const unsigned shift = s.sa & 127, q = shift / 64, bit = shift % 64;
  uint64_t d[2];
  for (unsigned i = 0; i < 2; ++i) {
    const uint64_t lo = w[i + q], hi = (i + q + 1 < 4) ? w[i + q + 1] : 0;
    d[i] = bit ? (lo >> bit) | (hi << (64 - bit)) : lo;
  }
  return _mm_loadu_si128(reinterpret_cast<const __m128i*>(d));
}

// Leaves recompiled code: the CPU continues at `target`; `n` instructions were executed.
inline uint32_t leave(Cpu& c, uint64_t n, uint32_t target) {
  c.s.pc = target;
  c.s.npc = target + 4;
  c.executed += n;
  c.mem.hw.cycles += n;
  return target;
}

// True when a loop should hand control back so timers and interrupts are serviced.
inline bool due(const Cpu& c, uint64_t n) { return c.executed + n >= c.pollAt; }

// Called on the backward branch of a loop that stores nothing: the CPU's idle detection may
// move emulated time to the next event, and then the loop is left so it can be delivered.
inline bool idle(Cpu& c, uint64_t& n, uint32_t pc, int32_t length) {
  c.executed += n;
  c.mem.hw.cycles += n;
  n = 0;
  c.idleCheck(pc, length);
  return c.pollAt <= c.executed;
}

// The FPU as Cpu::cop1: no infinities or NaNs, overflow saturates to the largest finite value.
inline float fclamp(float f) {
  const uint32_t b = std::bit_cast<uint32_t>(f);
  if ((b & 0x7F800000u) == 0x7F800000u) return std::bit_cast<float>((b & 0x80000000u) | 0x7F7FFFFFu);
  return f;
}
inline float fdiv(float a, float b) {
  return b == 0.0f ? (std::signbit(a) != std::signbit(b) ? -FLT_MAX : FLT_MAX) : fclamp(a / b);
}
inline float fsqrt(float a) { return std::sqrt(std::fabs(a)); }
inline float frsqrt(float a, float b) { return fclamp(a / std::sqrt(std::fabs(b))); }
inline float ftoi(float v) {  // CVT.W.S: truncates, saturates
  const int32_t i = v >= 2147483520.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : int32_t(v);
  return std::bit_cast<float>(uint32_t(i));
}
inline void fcond(CpuState& s, bool c) { s.fcr31 = c ? (s.fcr31 | (1u << 23)) : (s.fcr31 & ~(1u << 23)); }

// A macro-mode upper instruction: `op` is known, so only its own code is left.
template <uint32_t K>
__forceinline void macroUpper(CpuState& s, uint32_t op) { vuUpperK<K>(VuRegs{s.vf, s.vi, &s.vacc}, op); }

// Runs one instruction through the interpreter (rare instructions, identical semantics).
inline void interp(Cpu& c, uint32_t op, uint32_t pc) {
  c.execute(op, pc);
  c.s.gpr[0] = {};
}

}  // namespace rc

#define RC_R(i) s.gpr[i].ud[0]
#define RC_W(i) static_cast<uint32_t>(s.gpr[i].ud[0])
#define RC_EXIT(target) return rc::leave(c, n, (target))
