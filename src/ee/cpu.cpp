#include "ee/cpu.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <format>
#include <limits>

#include "ee/kernel.h"

namespace {
constexpr uint64_t kPollLimit = 2'000'000;

inline uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
inline float asFloat(uint32_t v) { return std::bit_cast<float>(v); }
inline uint32_t asBits(float f) { return std::bit_cast<uint32_t>(f); }

// The EE FPU has no infinities or NaNs: overflow saturates to the largest finite value.
inline float clampf(float f) {
  const uint32_t b = std::bit_cast<uint32_t>(f);
  if ((b & 0x7F800000u) == 0x7F800000u) return std::bit_cast<float>((b & 0x80000000u) | 0x7F7FFFFFu);
  return f;
}

template <class T>
T sat(int64_t v) {
  return static_cast<T>(std::clamp<int64_t>(v, std::numeric_limits<T>::min(), std::numeric_limits<T>::max()));
}

// Masks/shifts for the unaligned load/store pairs (little-endian).
constexpr uint32_t kLwlMask[4] = {0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000};
constexpr uint32_t kLwrMask[4] = {0x00000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00};
constexpr uint32_t kSwlMask[4] = {0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000};
constexpr uint32_t kSwrMask[4] = {0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF};
inline uint64_t ldlMask(unsigned i) { return i == 7 ? 0 : (~0ull >> ((i + 1) * 8)); }
inline uint64_t ldrMask(unsigned i) { return i == 0 ? 0 : (~0ull << ((8 - i) * 8)); }
inline uint64_t sdlMask(unsigned i) { return i == 7 ? 0 : (~0ull << ((i + 1) * 8)); }
inline uint64_t sdrMask(unsigned i) { return i == 0 ? 0 : (~0ull >> ((8 - i) * 8)); }
}  // namespace

const char* toString(StopReason r) {
  switch (r) {
    case StopReason::None: return "running";
    case StopReason::Unimplemented: return "unimplemented instruction or syscall";
    case StopReason::Break: return "break/trap";
    case StopReason::Fault: return "memory fault";
    case StopReason::Exit: return "game exited";
    case StopReason::Idle: return "idle: every thread is waiting";
    case StopReason::Polling: return "busy-waiting on a hardware register";
    case StopReason::Limit: return "instruction limit reached";
  }
  return "?";
}

void CpuState::reset(uint32_t entry) {
  *this = CpuState{};
  pc = entry;
  npc = entry + 4;
  vf[0].f[3] = 1.0f;
  vi[20] = 0x3F800000;  // VU0 R register
  cop0[12] = 0x70030C11;  // Status of a user thread: IE and EIE set, EXL/ERL clear
  cop0[15] = 0x2E20;      // PRId: R5900
}

void Cpu::halt(StopReason reason, std::string detail) {
  if (stop != StopReason::None) return;
  stop = reason;
  stopDetail = std::move(detail);
}

void Cpu::unimplemented(uint32_t op, uint32_t pc, const char* what) {
  halt(StopReason::Unimplemented, std::format("{} (opcode 0x{:08X} at 0x{:08X})", what, op, pc));
}

void Cpu::run(uint64_t maxInstructions) {
  while (stop == StopReason::None) {
    if (executed >= maxInstructions) {
      halt(StopReason::Limit, std::format("{} instructions", executed));
      break;
    }
    if (executed >= pollAt) {
      if (profile) ++(*profile)[s.pc & ~0x3Fu];  // where the game spends its time (--profile)
      kernel->poll();
      pollAt = executed + 64;
      if (stop != StopReason::None) break;
    }
    const uint32_t pc = s.pc;
    currentPc = pc;
    // Recompiled code takes over at its entry points (never in the middle of a delay slot).
    if (s.npc == pc + 4) {
      const uint32_t index = (pc - kRecompBase) >> 2;
      if (index < recomp.size() && (pc & 3) == 0 && recomp[index]) {
        ++recompCalls;
        const uint64_t before = executed;
        recomp[index](*this, pc);
        if (!mem.fault.empty()) halt(StopReason::Fault, std::format("{} in recompiled code entered at 0x{:08X}", mem.fault, pc));
        // An entry at an instruction the recompiled code leaves to the interpreter returns
        // without progress; the interpreter runs it below.
        if (executed != before || s.pc != pc) continue;
      }
    }
    const uint8_t* p = (pc & 3) ? nullptr : mem.hostPtr(pc, 4);
    if (!p) {
      halt(StopReason::Fault, std::format("cannot execute at 0x{:08X}", pc));
      break;
    }
    uint32_t op;
    std::memcpy(&op, p, 4);
    s.pc = s.npc;
    s.npc += 4;
    execute(op, pc);
    s.gpr[0] = {};
    ++executed;
    ++interpreted;
    ++mem.hw.cycles;
    if (!mem.fault.empty()) halt(StopReason::Fault, std::format("{} at pc=0x{:08X}", mem.fault, pc));
    if (mem.pollStreak > kPollLimit)
      halt(StopReason::Polling, std::format("register 0x{:08X} read {} times in a row (pc=0x{:08X})", mem.pollAddr,
                                            mem.pollStreak, pc));
  }
}

void Cpu::branch(bool taken, uint32_t pc, int32_t imm, bool likely) {
  if (taken) {
    s.npc = pc + 4 + (static_cast<uint32_t>(imm) << 2);
    if (imm < 0) idleCheck(pc, -imm * 4);
  } else if (likely) {  // branch-likely not taken: the delay slot is nullified
    s.pc = s.npc;
    s.npc += 4;
  }
}

// A short backward loop that keeps running with the same register values and without storing
// anything can only be waiting for an interrupt or another thread (e.g. a VBlank flag). Once it
// has spun a while, time skips ahead to the next interrupt instead of executing the loop
// millions of times.
void Cpu::idleCheck(uint32_t pc, int32_t length) {
  constexpr int kMaxLoopBytes = 64, kSpinsBeforeSkip = 32;
  uint64_t state = 0;
  for (int i = 1; i < 32; ++i) state = (state ^ s.gpr[i].ud[0]) * 0x100000001B3ull;
  if (length > kMaxLoopBytes || pc != spinPc_ || mem.writes != spinWrites_ || state != spinState_) {
    spinPc_ = pc;
    spinWrites_ = mem.writes;
    spinState_ = state;
    spinCount_ = 0;
    return;
  }
  if (++spinCount_ < kSpinsBeforeSkip) return;
  spinCount_ = 0;
  const uint64_t next = mem.hw.nextEvent();
  if (next != Hardware::kNever && next > mem.hw.cycles) {
    idleSkipped += next - mem.hw.cycles;
    mem.hw.cycles = next;
    pollAt = executed;  // deliver the interrupt right away
  }
}

void Cpu::noteCall(uint32_t target, uint32_t ret) {
  if (calls[target]++ == 0) callOrder.push_back(target);
  if (s.callstack.size() < 512) s.callstack.emplace_back(target, ret);
}

void Cpu::noteReturn(uint32_t target) {
  auto& cs = s.callstack;
  for (size_t i = cs.size(); i > 0 && cs.size() - i < 8; --i) {
    if (cs[i - 1].second == target) {
      cs.resize(i - 1);
      return;
    }
  }
}

void Cpu::mult(int p, uint32_t rs, uint32_t rt, uint32_t rd, bool isSigned, bool accumulate) {
  const uint32_t a = static_cast<uint32_t>(s.gpr[rs].ud[0]), b = static_cast<uint32_t>(s.gpr[rt].ud[0]);
  uint64_t r = isSigned ? static_cast<uint64_t>(int64_t(int32_t(a)) * int32_t(b)) : uint64_t(a) * b;
  if (accumulate) r += (uint64_t(uint32_t(s.hi.ud[p])) << 32) | uint32_t(s.lo.ud[p]);
  s.lo.ud[p] = sx32(static_cast<uint32_t>(r));
  s.hi.ud[p] = sx32(static_cast<uint32_t>(r >> 32));
  s.gpr[rd].ud[0] = s.lo.ud[p];
}

void Cpu::div(int p, uint32_t rs, uint32_t rt, bool isSigned) {
  const uint32_t a = static_cast<uint32_t>(s.gpr[rs].ud[0]), b = static_cast<uint32_t>(s.gpr[rt].ud[0]);
  if (isSigned) {
    const int32_t n = int32_t(a), d = int32_t(b);
    if (d == 0) {
      s.lo.ud[p] = sx32(n < 0 ? 1 : 0xFFFFFFFF);
      s.hi.ud[p] = sx32(a);
    } else if (n == INT32_MIN && d == -1) {
      s.lo.ud[p] = sx32(a);
      s.hi.ud[p] = 0;
    } else {
      s.lo.ud[p] = sx32(uint32_t(n / d));
      s.hi.ud[p] = sx32(uint32_t(n % d));
    }
  } else if (b == 0) {
    s.lo.ud[p] = sx32(0xFFFFFFFF);
    s.hi.ud[p] = sx32(a);
  } else {
    s.lo.ud[p] = sx32(a / b);
    s.hi.ud[p] = sx32(a % b);
  }
}

void Cpu::execute(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31;
  const int32_t imm = static_cast<int16_t>(op & 0xFFFF);
  const uint32_t uimm = op & 0xFFFF;
  auto R = [&](uint32_t i) -> uint64_t& { return s.gpr[i].ud[0]; };
  auto W = [&](uint32_t i) { return static_cast<uint32_t>(s.gpr[i].ud[0]); };
  const uint32_t ea = W(rs) + static_cast<uint32_t>(imm);

  switch (op >> 26) {
    case 0x00: special(op, pc); return;
    case 0x01: regimm(op, pc); return;
    case 0x02: s.npc = (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2); return;  // J
    case 0x03: {                                                             // JAL
      const uint32_t target = (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2);
      R(31) = sx32(pc + 8);
      s.npc = target;
      noteCall(target, pc + 8);
      return;
    }
    case 0x04: branch(R(rs) == R(rt), pc, imm, false); return;
    case 0x05: branch(R(rs) != R(rt), pc, imm, false); return;
    case 0x06: branch(int64_t(R(rs)) <= 0, pc, imm, false); return;
    case 0x07: branch(int64_t(R(rs)) > 0, pc, imm, false); return;
    case 0x08:  // ADDI (overflow exceptions are never relied on by compiled code)
    case 0x09: R(rt) = sx32(W(rs) + static_cast<uint32_t>(imm)); return;
    case 0x0A: R(rt) = int64_t(R(rs)) < int64_t(imm); return;
    case 0x0B: R(rt) = R(rs) < static_cast<uint64_t>(int64_t(imm)); return;
    case 0x0C: R(rt) = R(rs) & uimm; return;
    case 0x0D: R(rt) = R(rs) | uimm; return;
    case 0x0E: R(rt) = R(rs) ^ uimm; return;
    case 0x0F: R(rt) = sx32(uimm << 16); return;
    case 0x10: cop0(op, pc); return;
    case 0x11: cop1(op, pc); return;
    case 0x12: cop2(op, pc); return;
    case 0x14: branch(R(rs) == R(rt), pc, imm, true); return;
    case 0x15: branch(R(rs) != R(rt), pc, imm, true); return;
    case 0x16: branch(int64_t(R(rs)) <= 0, pc, imm, true); return;
    case 0x17: branch(int64_t(R(rs)) > 0, pc, imm, true); return;
    case 0x18:
    case 0x19: R(rt) = R(rs) + static_cast<uint64_t>(int64_t(imm)); return;  // DADDI, DADDIU
    case 0x1A: {                                                             // LDL
      const unsigned i = ea & 7;
      R(rt) = (R(rt) & ldlMask(i)) | (mem.read<uint64_t>(ea & ~7u) << (56 - i * 8));
      return;
    }
    case 0x1B: {  // LDR
      const unsigned i = ea & 7;
      R(rt) = (R(rt) & ldrMask(i)) | (mem.read<uint64_t>(ea & ~7u) >> (i * 8));
      return;
    }
    case 0x1C: mmi(op, pc); return;
    case 0x1E: mem.read128(ea & ~15u, &s.gpr[rt]); return;   // LQ
    case 0x1F: mem.write128(ea & ~15u, &s.gpr[rt]); return;  // SQ
    case 0x20: R(rt) = static_cast<uint64_t>(int64_t(mem.read<int8_t>(ea))); return;
    case 0x21: R(rt) = static_cast<uint64_t>(int64_t(mem.read<int16_t>(ea))); return;
    case 0x22: {  // LWL
      const unsigned i = ea & 3;
      R(rt) = sx32((W(rt) & kLwlMask[i]) | (mem.read<uint32_t>(ea & ~3u) << (24 - i * 8)));
      return;
    }
    case 0x23: R(rt) = sx32(mem.read<uint32_t>(ea)); return;
    case 0x24: R(rt) = mem.read<uint8_t>(ea); return;
    case 0x25: R(rt) = mem.read<uint16_t>(ea); return;
    case 0x26: {  // LWR
      const unsigned i = ea & 3;
      const uint32_t v = (W(rt) & kLwrMask[i]) | (mem.read<uint32_t>(ea & ~3u) >> (i * 8));
      if (i == 0)
        R(rt) = sx32(v);
      else
        R(rt) = (R(rt) & 0xFFFFFFFF00000000ull) | v;
      return;
    }
    case 0x27: R(rt) = mem.read<uint32_t>(ea); return;
    case 0x28: mem.write<uint8_t>(ea, static_cast<uint8_t>(R(rt))); return;
    case 0x29: mem.write<uint16_t>(ea, static_cast<uint16_t>(R(rt))); return;
    case 0x2A: {  // SWL
      const unsigned i = ea & 3;
      const uint32_t m = mem.read<uint32_t>(ea & ~3u);
      mem.write<uint32_t>(ea & ~3u, (W(rt) >> (24 - i * 8)) | (m & kSwlMask[i]));
      return;
    }
    case 0x2B: mem.write<uint32_t>(ea, W(rt)); return;
    case 0x2C: {  // SDL
      const unsigned i = ea & 7;
      const uint64_t m = mem.read<uint64_t>(ea & ~7u);
      mem.write<uint64_t>(ea & ~7u, (R(rt) >> (56 - i * 8)) | (m & sdlMask(i)));
      return;
    }
    case 0x2D: {  // SDR
      const unsigned i = ea & 7;
      const uint64_t m = mem.read<uint64_t>(ea & ~7u);
      mem.write<uint64_t>(ea & ~7u, (R(rt) << (i * 8)) | (m & sdrMask(i)));
      return;
    }
    case 0x2E: {  // SWR
      const unsigned i = ea & 3;
      const uint32_t m = mem.read<uint32_t>(ea & ~3u);
      mem.write<uint32_t>(ea & ~3u, (W(rt) << (i * 8)) | (m & kSwrMask[i]));
      return;
    }
    case 0x2F: return;  // CACHE
    case 0x31: s.fpr[rt] = asFloat(mem.read<uint32_t>(ea)); return;  // LWC1
    case 0x33: return;                                                // PREF
    case 0x36:                                                        // LQC2
      mem.read128(ea & ~15u, &s.vf[rt]);
      s.vf[0] = {};
      s.vf[0].f[3] = 1.0f;
      return;
    case 0x37: R(rt) = mem.read<uint64_t>(ea); return;                    // LD
    case 0x39: mem.write<uint32_t>(ea, asBits(s.fpr[rt])); return;        // SWC1
    case 0x3E: mem.write128(ea & ~15u, &s.vf[rt]); return;                // SQC2
    case 0x3F: mem.write<uint64_t>(ea, R(rt)); return;                    // SD
  }
  unimplemented(op, pc, "opcode");
}

void Cpu::special(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  auto R = [&](uint32_t i) -> uint64_t& { return s.gpr[i].ud[0]; };
  auto W = [&](uint32_t i) { return static_cast<uint32_t>(s.gpr[i].ud[0]); };
  auto trap = [&](bool cond) {
    if (cond) halt(StopReason::Break, std::format("trap at 0x{:08X}", pc));
  };

  switch (op & 63) {
    case 0x00: R(rd) = sx32(W(rt) << sa); return;
    case 0x02: R(rd) = sx32(W(rt) >> sa); return;
    case 0x03: R(rd) = sx32(uint32_t(int32_t(W(rt)) >> sa)); return;
    case 0x04: R(rd) = sx32(W(rt) << (W(rs) & 31)); return;
    case 0x06: R(rd) = sx32(W(rt) >> (W(rs) & 31)); return;
    case 0x07: R(rd) = sx32(uint32_t(int32_t(W(rt)) >> (W(rs) & 31))); return;
    case 0x08: {  // JR
      const uint32_t target = W(rs);
      s.npc = target;
      if (rs == 31) noteReturn(target);
      return;
    }
    case 0x09: {  // JALR
      const uint32_t target = W(rs);
      R(rd) = sx32(pc + 8);
      s.npc = target;
      noteCall(target, pc + 8);
      return;
    }
    case 0x0A: if (R(rt) == 0) R(rd) = R(rs); return;  // MOVZ
    case 0x0B: if (R(rt) != 0) R(rd) = R(rs); return;  // MOVN
    case 0x0C: kernel->syscall(); return;
    case 0x0D: halt(StopReason::Break, std::format("break {} at 0x{:08X}", (op >> 6) & 0xFFFFF, pc)); return;
    case 0x0F: return;  // SYNC
    case 0x10: R(rd) = s.hi.ud[0]; return;
    case 0x11: s.hi.ud[0] = R(rs); return;
    case 0x12: R(rd) = s.lo.ud[0]; return;
    case 0x13: s.lo.ud[0] = R(rs); return;
    case 0x14: R(rd) = R(rt) << (W(rs) & 63); return;
    case 0x16: R(rd) = R(rt) >> (W(rs) & 63); return;
    case 0x17: R(rd) = static_cast<uint64_t>(int64_t(R(rt)) >> (W(rs) & 63)); return;
    case 0x18: mult(0, rs, rt, rd, true, false); return;
    case 0x19: mult(0, rs, rt, rd, false, false); return;
    case 0x1A: div(0, rs, rt, true); return;
    case 0x1B: div(0, rs, rt, false); return;
    case 0x20:
    case 0x21: R(rd) = sx32(W(rs) + W(rt)); return;
    case 0x22:
    case 0x23: R(rd) = sx32(W(rs) - W(rt)); return;
    case 0x24: R(rd) = R(rs) & R(rt); return;
    case 0x25: R(rd) = R(rs) | R(rt); return;
    case 0x26: R(rd) = R(rs) ^ R(rt); return;
    case 0x27: R(rd) = ~(R(rs) | R(rt)); return;
    case 0x28: R(rd) = s.sa; return;   // MFSA
    case 0x29: s.sa = W(rs); return;   // MTSA
    case 0x2A: R(rd) = int64_t(R(rs)) < int64_t(R(rt)); return;
    case 0x2B: R(rd) = R(rs) < R(rt); return;
    case 0x2C:
    case 0x2D: R(rd) = R(rs) + R(rt); return;
    case 0x2E:
    case 0x2F: R(rd) = R(rs) - R(rt); return;
    case 0x30: trap(int64_t(R(rs)) >= int64_t(R(rt))); return;
    case 0x31: trap(R(rs) >= R(rt)); return;
    case 0x32: trap(int64_t(R(rs)) < int64_t(R(rt))); return;
    case 0x33: trap(R(rs) < R(rt)); return;
    case 0x34: trap(R(rs) == R(rt)); return;
    case 0x36: trap(R(rs) != R(rt)); return;
    case 0x38: R(rd) = R(rt) << sa; return;
    case 0x3A: R(rd) = R(rt) >> sa; return;
    case 0x3B: R(rd) = static_cast<uint64_t>(int64_t(R(rt)) >> sa); return;
    case 0x3C: R(rd) = R(rt) << (sa + 32); return;
    case 0x3E: R(rd) = R(rt) >> (sa + 32); return;
    case 0x3F: R(rd) = static_cast<uint64_t>(int64_t(R(rt)) >> (sa + 32)); return;
  }
  unimplemented(op, pc, "SPECIAL");
}

void Cpu::regimm(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31;
  const int32_t imm = static_cast<int16_t>(op & 0xFFFF);
  const int64_t v = static_cast<int64_t>(s.gpr[rs].ud[0]);
  const uint32_t w = static_cast<uint32_t>(s.gpr[rs].ud[0]);
  auto link = [&] { s.gpr[31].ud[0] = sx32(pc + 8); };
  switch (rt) {
    case 0x00: branch(v < 0, pc, imm, false); return;
    case 0x01: branch(v >= 0, pc, imm, false); return;
    case 0x02: branch(v < 0, pc, imm, true); return;
    case 0x03: branch(v >= 0, pc, imm, true); return;
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0E: return;  // trap-immediate: never taken in practice
    case 0x10: link(); branch(v < 0, pc, imm, false); return;
    case 0x11: link(); branch(v >= 0, pc, imm, false); return;
    case 0x12: link(); branch(v < 0, pc, imm, true); return;
    case 0x13: link(); branch(v >= 0, pc, imm, true); return;
    case 0x18: s.sa = ((w & 15) ^ (imm & 15)) * 8; return;   // MTSAB
    case 0x19: s.sa = ((w & 7) ^ (imm & 7)) * 16; return;    // MTSAH
  }
  unimplemented(op, pc, "REGIMM");
}

void Cpu::mmi(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  const u128 a = s.gpr[rs], b = s.gpr[rt];
  u128 d{};
  switch (op & 63) {
    case 0x00: mult(0, rs, rt, rd, true, true); return;    // MADD
    case 0x01: mult(0, rs, rt, rd, false, true); return;   // MADDU
    case 0x04:                                             // PLZCW
      for (int i = 0; i < 2; ++i) {
        uint32_t v = a.uw[i];
        if (v & 0x80000000) v = ~v;
        d.uw[i] = (v == 0 ? 32 : std::countl_zero(v)) - 1;
      }
      d.ud[1] = s.gpr[rd].ud[1];
      break;
    case 0x08: mmi0(op, pc); return;
    case 0x09: mmi2(op, pc); return;
    case 0x10: s.gpr[rd].ud[0] = s.hi.ud[1]; return;  // MFHI1
    case 0x11: s.hi.ud[1] = a.ud[0]; return;           // MTHI1
    case 0x12: s.gpr[rd].ud[0] = s.lo.ud[1]; return;  // MFLO1
    case 0x13: s.lo.ud[1] = a.ud[0]; return;           // MTLO1
    case 0x18: mult(1, rs, rt, rd, true, false); return;
    case 0x19: mult(1, rs, rt, rd, false, false); return;
    case 0x1A: div(1, rs, rt, true); return;
    case 0x1B: div(1, rs, rt, false); return;
    case 0x20: mult(1, rs, rt, rd, true, true); return;
    case 0x21: mult(1, rs, rt, rd, false, true); return;
    case 0x28: mmi1(op, pc); return;
    case 0x29: mmi3(op, pc); return;
    case 0x30:  // PMFHL
      if (sa == 0) {
        d.uw[0] = s.lo.uw[0]; d.uw[1] = s.hi.uw[0]; d.uw[2] = s.lo.uw[2]; d.uw[3] = s.hi.uw[2];
      } else if (sa == 1) {
        d.uw[0] = s.lo.uw[1]; d.uw[1] = s.hi.uw[1]; d.uw[2] = s.lo.uw[3]; d.uw[3] = s.hi.uw[3];
      } else {
        unimplemented(op, pc, "PMFHL variant");
        return;
      }
      break;
    case 0x31:  // PMTHL.LW
      if (sa != 0) {
        unimplemented(op, pc, "PMTHL variant");
        return;
      }
      s.lo.uw[0] = a.uw[0]; s.hi.uw[0] = a.uw[1]; s.lo.uw[2] = a.uw[2]; s.hi.uw[2] = a.uw[3];
      return;
    case 0x34: for (int i = 0; i < 8; ++i) d.uh[i] = uint16_t(b.uh[i] << (sa & 15)); break;  // PSLLH
    case 0x36: for (int i = 0; i < 8; ++i) d.uh[i] = uint16_t(b.uh[i] >> (sa & 15)); break;  // PSRLH
    case 0x37: for (int i = 0; i < 8; ++i) d.sh[i] = int16_t(b.sh[i] >> (sa & 15)); break;   // PSRAH
    case 0x3C: for (int i = 0; i < 4; ++i) d.uw[i] = b.uw[i] << sa; break;                   // PSLLW
    case 0x3E: for (int i = 0; i < 4; ++i) d.uw[i] = b.uw[i] >> sa; break;                   // PSRLW
    case 0x3F: for (int i = 0; i < 4; ++i) d.sw[i] = b.sw[i] >> sa; break;                   // PSRAW
    default: unimplemented(op, pc, "MMI"); return;
  }
  s.gpr[rd] = d;
}

void Cpu::mmi0(uint32_t op, uint32_t pc) {
  const u128 a = s.gpr[(op >> 21) & 31], b = s.gpr[(op >> 16) & 31];
  u128 d{};
  switch ((op >> 6) & 31) {
    case 0x00: for (int i = 0; i < 4; ++i) d.uw[i] = a.uw[i] + b.uw[i]; break;                      // PADDW
    case 0x01: for (int i = 0; i < 4; ++i) d.uw[i] = a.uw[i] - b.uw[i]; break;                      // PSUBW
    case 0x02: for (int i = 0; i < 4; ++i) d.uw[i] = a.sw[i] > b.sw[i] ? ~0u : 0; break;           // PCGTW
    case 0x03: for (int i = 0; i < 4; ++i) d.sw[i] = std::max(a.sw[i], b.sw[i]); break;             // PMAXW
    case 0x04: for (int i = 0; i < 8; ++i) d.uh[i] = uint16_t(a.uh[i] + b.uh[i]); break;            // PADDH
    case 0x05: for (int i = 0; i < 8; ++i) d.uh[i] = uint16_t(a.uh[i] - b.uh[i]); break;            // PSUBH
    case 0x06: for (int i = 0; i < 8; ++i) d.uh[i] = a.sh[i] > b.sh[i] ? 0xFFFF : 0; break;         // PCGTH
    case 0x07: for (int i = 0; i < 8; ++i) d.sh[i] = std::max(a.sh[i], b.sh[i]); break;             // PMAXH
    case 0x08: for (int i = 0; i < 16; ++i) d.ub[i] = uint8_t(a.ub[i] + b.ub[i]); break;            // PADDB
    case 0x09: for (int i = 0; i < 16; ++i) d.ub[i] = uint8_t(a.ub[i] - b.ub[i]); break;            // PSUBB
    case 0x0A: for (int i = 0; i < 16; ++i) d.ub[i] = a.sb[i] > b.sb[i] ? 0xFF : 0; break;          // PCGTB
    case 0x10: for (int i = 0; i < 4; ++i) d.sw[i] = sat<int32_t>(int64_t(a.sw[i]) + b.sw[i]); break;  // PADDSW
    case 0x11: for (int i = 0; i < 4; ++i) d.sw[i] = sat<int32_t>(int64_t(a.sw[i]) - b.sw[i]); break;  // PSUBSW
    case 0x12: d.uw[0] = b.uw[0]; d.uw[1] = a.uw[0]; d.uw[2] = b.uw[1]; d.uw[3] = a.uw[1]; break;      // PEXTLW
    case 0x13: d.uw[0] = b.uw[0]; d.uw[1] = b.uw[2]; d.uw[2] = a.uw[0]; d.uw[3] = a.uw[2]; break;      // PPACW
    case 0x14: for (int i = 0; i < 8; ++i) d.sh[i] = sat<int16_t>(int64_t(a.sh[i]) + b.sh[i]); break;  // PADDSH
    case 0x15: for (int i = 0; i < 8; ++i) d.sh[i] = sat<int16_t>(int64_t(a.sh[i]) - b.sh[i]); break;  // PSUBSH
    case 0x16: for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[i]; d.uh[2 * i + 1] = a.uh[i]; } break;  // PEXTLH
    case 0x17: for (int i = 0; i < 4; ++i) { d.uh[i] = b.uh[2 * i]; d.uh[4 + i] = a.uh[2 * i]; } break;  // PPACH
    case 0x18: for (int i = 0; i < 16; ++i) d.sb[i] = sat<int8_t>(int64_t(a.sb[i]) + b.sb[i]); break;  // PADDSB
    case 0x19: for (int i = 0; i < 16; ++i) d.sb[i] = sat<int8_t>(int64_t(a.sb[i]) - b.sb[i]); break;  // PSUBSB
    case 0x1A: for (int i = 0; i < 8; ++i) { d.ub[2 * i] = b.ub[i]; d.ub[2 * i + 1] = a.ub[i]; } break;  // PEXTLB
    case 0x1B: for (int i = 0; i < 8; ++i) { d.ub[i] = b.ub[2 * i]; d.ub[8 + i] = a.ub[2 * i]; } break;  // PPACB
    default: unimplemented(op, pc, "MMI0"); return;
  }
  s.gpr[(op >> 11) & 31] = d;
}

void Cpu::mmi1(uint32_t op, uint32_t pc) {
  const u128 a = s.gpr[(op >> 21) & 31], b = s.gpr[(op >> 16) & 31];
  u128 d{};
  switch ((op >> 6) & 31) {
    case 0x01: for (int i = 0; i < 4; ++i) d.sw[i] = sat<int32_t>(b.sw[i] < 0 ? -int64_t(b.sw[i]) : b.sw[i]); break;  // PABSW
    case 0x02: for (int i = 0; i < 4; ++i) d.uw[i] = a.uw[i] == b.uw[i] ? ~0u : 0; break;        // PCEQW
    case 0x03: for (int i = 0; i < 4; ++i) d.sw[i] = std::min(a.sw[i], b.sw[i]); break;          // PMINW
    case 0x05: for (int i = 0; i < 8; ++i) d.sh[i] = sat<int16_t>(b.sh[i] < 0 ? -int64_t(b.sh[i]) : b.sh[i]); break;  // PABSH
    case 0x06: for (int i = 0; i < 8; ++i) d.uh[i] = a.uh[i] == b.uh[i] ? 0xFFFF : 0; break;     // PCEQH
    case 0x07: for (int i = 0; i < 8; ++i) d.sh[i] = std::min(a.sh[i], b.sh[i]); break;          // PMINH
    case 0x0A: for (int i = 0; i < 16; ++i) d.ub[i] = a.ub[i] == b.ub[i] ? 0xFF : 0; break;      // PCEQB
    case 0x10: for (int i = 0; i < 4; ++i) d.uw[i] = uint32_t(std::min<uint64_t>(uint64_t(a.uw[i]) + b.uw[i], 0xFFFFFFFF)); break;  // PADDUW
    case 0x11: for (int i = 0; i < 4; ++i) d.uw[i] = a.uw[i] > b.uw[i] ? a.uw[i] - b.uw[i] : 0; break;  // PSUBUW
    case 0x12: d.uw[0] = b.uw[2]; d.uw[1] = a.uw[2]; d.uw[2] = b.uw[3]; d.uw[3] = a.uw[3]; break;    // PEXTUW
    case 0x14: for (int i = 0; i < 8; ++i) d.uh[i] = uint16_t(std::min(a.uh[i] + b.uh[i], 0xFFFF)); break;  // PADDUH
    case 0x15: for (int i = 0; i < 8; ++i) d.uh[i] = a.uh[i] > b.uh[i] ? uint16_t(a.uh[i] - b.uh[i]) : 0; break;  // PSUBUH
    case 0x16: for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[4 + i]; d.uh[2 * i + 1] = a.uh[4 + i]; } break;  // PEXTUH
    case 0x18: for (int i = 0; i < 16; ++i) d.ub[i] = uint8_t(std::min(a.ub[i] + b.ub[i], 0xFF)); break;  // PADDUB
    case 0x19: for (int i = 0; i < 16; ++i) d.ub[i] = a.ub[i] > b.ub[i] ? uint8_t(a.ub[i] - b.ub[i]) : 0; break;  // PSUBUB
    case 0x1A: for (int i = 0; i < 8; ++i) { d.ub[2 * i] = b.ub[8 + i]; d.ub[2 * i + 1] = a.ub[8 + i]; } break;  // PEXTUB
    case 0x1B: {  // QFSRV: rd = low 128 bits of (rs:rt) >> SA
      const uint64_t w[4] = {b.ud[0], b.ud[1], a.ud[0], a.ud[1]};
      const unsigned shift = s.sa & 127, q = shift / 64, bit = shift % 64;
      for (unsigned i = 0; i < 2; ++i) {
        const uint64_t lo = w[i + q], hi = (i + q + 1 < 4) ? w[i + q + 1] : 0;
        d.ud[i] = bit ? (lo >> bit) | (hi << (64 - bit)) : lo;
      }
      break;
    }
    default: unimplemented(op, pc, "MMI1"); return;
  }
  s.gpr[(op >> 11) & 31] = d;
}

void Cpu::mmi2(uint32_t op, uint32_t pc) {
  const u128 a = s.gpr[(op >> 21) & 31], b = s.gpr[(op >> 16) & 31];
  u128 d{};
  switch ((op >> 6) & 31) {
    case 0x02:  // PSLLVW
      d.ud[0] = sx32(b.uw[0] << (a.uw[0] & 31));
      d.ud[1] = sx32(b.uw[2] << (a.uw[2] & 31));
      break;
    case 0x03:  // PSRLVW
      d.ud[0] = sx32(b.uw[0] >> (a.uw[0] & 31));
      d.ud[1] = sx32(b.uw[2] >> (a.uw[2] & 31));
      break;
    case 0x08: d = s.hi; break;  // PMFHI
    case 0x09: d = s.lo; break;  // PMFLO
    case 0x0A:                   // PINTH
      for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[i]; d.uh[2 * i + 1] = a.uh[4 + i]; }
      break;
    case 0x0C:  // PMULTW
      for (int i = 0; i < 2; ++i) {
        const int64_t p = int64_t(a.sw[2 * i]) * b.sw[2 * i];
        s.lo.ud[i] = sx32(uint32_t(p));
        s.hi.ud[i] = sx32(uint32_t(uint64_t(p) >> 32));
        d.sd[i] = p;
      }
      break;
    case 0x0E: d.ud[0] = b.ud[0]; d.ud[1] = a.ud[0]; break;  // PCPYLD
    case 0x12: d.ud[0] = a.ud[0] & b.ud[0]; d.ud[1] = a.ud[1] & b.ud[1]; break;  // PAND
    case 0x13: d.ud[0] = a.ud[0] ^ b.ud[0]; d.ud[1] = a.ud[1] ^ b.ud[1]; break;  // PXOR
    case 0x1A:  // PEXEH
      d.uh[0] = b.uh[2]; d.uh[1] = b.uh[1]; d.uh[2] = b.uh[0]; d.uh[3] = b.uh[3];
      d.uh[4] = b.uh[6]; d.uh[5] = b.uh[5]; d.uh[6] = b.uh[4]; d.uh[7] = b.uh[7];
      break;
    case 0x1B:  // PREVH
      for (int i = 0; i < 4; ++i) { d.uh[i] = b.uh[3 - i]; d.uh[4 + i] = b.uh[7 - i]; }
      break;
    case 0x1C: {  // PMULTH
      int32_t p[8];
      for (int i = 0; i < 8; ++i) p[i] = int32_t(a.sh[i]) * b.sh[i];
      s.lo.sw[0] = p[0]; s.lo.sw[1] = p[1]; s.hi.sw[0] = p[2]; s.hi.sw[1] = p[3];
      s.lo.sw[2] = p[4]; s.lo.sw[3] = p[5]; s.hi.sw[2] = p[6]; s.hi.sw[3] = p[7];
      d.sw[0] = p[0]; d.sw[1] = p[2]; d.sw[2] = p[4]; d.sw[3] = p[6];
      break;
    }
    case 0x1E: d.uw[0] = b.uw[2]; d.uw[1] = b.uw[1]; d.uw[2] = b.uw[0]; d.uw[3] = b.uw[3]; break;  // PEXEW
    case 0x1F: d.uw[0] = b.uw[1]; d.uw[1] = b.uw[2]; d.uw[2] = b.uw[0]; d.uw[3] = b.uw[3]; break;  // PROT3W
    default: unimplemented(op, pc, "MMI2"); return;
  }
  s.gpr[(op >> 11) & 31] = d;
}

void Cpu::mmi3(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31;
  const u128 a = s.gpr[rs], b = s.gpr[(op >> 16) & 31];
  u128 d{};
  switch ((op >> 6) & 31) {
    case 0x03:  // PSRAVW
      d.ud[0] = sx32(uint32_t(b.sw[0] >> (a.uw[0] & 31)));
      d.ud[1] = sx32(uint32_t(b.sw[2] >> (a.uw[2] & 31)));
      break;
    case 0x08: s.hi = a; return;  // PMTHI
    case 0x09: s.lo = a; return;  // PMTLO
    case 0x0A:                    // PINTEH
      for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[2 * i]; d.uh[2 * i + 1] = a.uh[2 * i]; }
      break;
    case 0x0C:  // PMULTUW
      for (int i = 0; i < 2; ++i) {
        const uint64_t p = uint64_t(a.uw[2 * i]) * b.uw[2 * i];
        s.lo.ud[i] = sx32(uint32_t(p));
        s.hi.ud[i] = sx32(uint32_t(p >> 32));
        d.ud[i] = p;
      }
      break;
    case 0x0E: d.ud[0] = a.ud[1]; d.ud[1] = b.ud[1]; break;  // PCPYUD
    case 0x12: d.ud[0] = a.ud[0] | b.ud[0]; d.ud[1] = a.ud[1] | b.ud[1]; break;  // POR
    case 0x13: d.ud[0] = ~(a.ud[0] | b.ud[0]); d.ud[1] = ~(a.ud[1] | b.ud[1]); break;  // PNOR
    case 0x1A:  // PEXCH
      d.uh[0] = b.uh[0]; d.uh[1] = b.uh[2]; d.uh[2] = b.uh[1]; d.uh[3] = b.uh[3];
      d.uh[4] = b.uh[4]; d.uh[5] = b.uh[6]; d.uh[6] = b.uh[5]; d.uh[7] = b.uh[7];
      break;
    case 0x1B: for (int i = 0; i < 4; ++i) { d.uh[i] = b.uh[0]; d.uh[4 + i] = b.uh[4]; } break;  // PCPYH
    case 0x1E: d.uw[0] = b.uw[0]; d.uw[1] = b.uw[2]; d.uw[2] = b.uw[1]; d.uw[3] = b.uw[3]; break;  // PEXCW
    default: unimplemented(op, pc, "MMI3"); return;
  }
  s.gpr[(op >> 11) & 31] = d;
}

void Cpu::cop0(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  switch (rs) {
    case 0x00:  // MFC0
      s.gpr[rt].ud[0] = sx32(rd == 9 ? static_cast<uint32_t>(executed) : s.cop0[rd]);
      return;
    case 0x04: s.cop0[rd] = static_cast<uint32_t>(s.gpr[rt].ud[0]); return;  // MTC0
    case 0x08: {  // BC0x: CPCOND0 reports DMA completion, which is always instant for now
      const bool cond = true, wantTrue = rt & 1, likely = rt & 2;
      branch(cond == wantTrue, pc, static_cast<int16_t>(op & 0xFFFF), likely);
      return;
    }
    case 0x10:
      switch (op & 63) {
        case 0x01: case 0x02: case 0x06: case 0x08: return;  // TLB ops: the kernel already set up the TLB
        case 0x18: {  // ERET (no delay slot). libkernl uses it to drop back from kernel mode.
          uint32_t& status = s.cop0[12];
          const uint32_t target = (status & 4) ? s.cop0[30] : s.cop0[14];  // ErrorEPC if ERL, else EPC
          status &= (status & 4) ? ~4u : ~2u;
          s.pc = target;
          s.npc = target + 4;
          return;
        }
        case 0x38: s.cop0[12] |= 0x10000; return;            // EI
        case 0x39: s.cop0[12] &= ~0x10000u; return;          // DI
      }
      break;
  }
  unimplemented(op, pc, "COP0");
}

void Cpu::cop1(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31, ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  float* f = s.fpr;
  constexpr uint32_t kCond = 1u << 23;
  switch (rs) {
    case 0x00: s.gpr[ft].ud[0] = sx32(asBits(f[fs])); return;                       // MFC1
    case 0x02: s.gpr[ft].ud[0] = sx32(fs == 31 ? s.fcr31 : (fs == 0 ? 0x2E00 : 0)); return;  // CFC1
    case 0x04: f[fs] = asFloat(static_cast<uint32_t>(s.gpr[ft].ud[0])); return;      // MTC1
    case 0x06: if (fs == 31) s.fcr31 = static_cast<uint32_t>(s.gpr[ft].ud[0]); return;  // CTC1
    case 0x08: {  // BC1F/BC1T/BC1FL/BC1TL
      const bool cond = (s.fcr31 & kCond) != 0;
      branch(cond == bool(ft & 1), pc, static_cast<int16_t>(op & 0xFFFF), ft & 2);
      return;
    }
    case 0x10:  // S format
      switch (op & 63) {
        case 0x00: f[fd] = clampf(f[fs] + f[ft]); return;
        case 0x01: f[fd] = clampf(f[fs] - f[ft]); return;
        case 0x02: f[fd] = clampf(f[fs] * f[ft]); return;
        case 0x03: f[fd] = f[ft] == 0.0f ? (std::signbit(f[fs]) != std::signbit(f[ft]) ? -FLT_MAX : FLT_MAX)
                                         : clampf(f[fs] / f[ft]);
          return;
        case 0x04: f[fd] = std::sqrt(std::fabs(f[ft])); return;
        case 0x05: f[fd] = std::fabs(f[fs]); return;
        case 0x06: f[fd] = f[fs]; return;
        case 0x07: f[fd] = -f[fs]; return;
        case 0x16: f[fd] = clampf(f[fs] / std::sqrt(std::fabs(f[ft]))); return;  // RSQRT
        case 0x18: s.acc = clampf(f[fs] + f[ft]); return;                       // ADDA
        case 0x19: s.acc = clampf(f[fs] - f[ft]); return;                       // SUBA
        case 0x1A: s.acc = clampf(f[fs] * f[ft]); return;                       // MULA
        case 0x1C: f[fd] = clampf(s.acc + f[fs] * f[ft]); return;               // MADD
        case 0x1D: f[fd] = clampf(s.acc - f[fs] * f[ft]); return;               // MSUB
        case 0x1E: s.acc = clampf(s.acc + f[fs] * f[ft]); return;               // MADDA
        case 0x1F: s.acc = clampf(s.acc - f[fs] * f[ft]); return;               // MSUBA
        case 0x24: {                                                            // CVT.W.S (truncates, saturates)
          const float v = f[fs];
          const int32_t i = v >= 2147483520.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : int32_t(v);
          f[fd] = asFloat(uint32_t(i));
          return;
        }
        case 0x28: f[fd] = std::max(f[fs], f[ft]); return;
        case 0x29: f[fd] = std::min(f[fs], f[ft]); return;
        case 0x30: s.fcr31 &= ~kCond; return;  // C.F
        case 0x32: s.fcr31 = f[fs] == f[ft] ? (s.fcr31 | kCond) : (s.fcr31 & ~kCond); return;
        case 0x34: s.fcr31 = f[fs] < f[ft] ? (s.fcr31 | kCond) : (s.fcr31 & ~kCond); return;
        case 0x36: s.fcr31 = f[fs] <= f[ft] ? (s.fcr31 | kCond) : (s.fcr31 & ~kCond); return;
      }
      break;
    case 0x14:  // W format
      if ((op & 63) == 0x20) {
        f[fd] = static_cast<float>(static_cast<int32_t>(asBits(f[fs])));
        return;
      }
      break;
  }
  unimplemented(op, pc, "COP1");
}

void Cpu::cop2(uint32_t op, uint32_t pc) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  switch (rs) {
    case 0x01: s.gpr[rt] = s.vf[rd]; return;                                          // QMFC2
    case 0x02: s.gpr[rt].ud[0] = sx32(s.vi[rd]); return;                              // CFC2
    case 0x05:                                                                        // QMTC2
      if (rd != 0) s.vf[rd] = s.gpr[rt];
      return;
    case 0x06:                                                                        // CTC2
      if (rd != 0) s.vi[rd] = static_cast<uint32_t>(s.gpr[rt].ud[0]);
      return;
    case 0x08: {  // BC2x: CPCOND2 is "VU0 busy", and VU0 never runs a micro program yet
      branch(bool(rt & 1) == false, pc, static_cast<int16_t>(op & 0xFFFF), rt & 2);
      return;
    }
  }
  if (rs >= 0x10) {
    cop2Macro(op, pc);
    return;
  }
  unimplemented(op, pc, "COP2");
}
