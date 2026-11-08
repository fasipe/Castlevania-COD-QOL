#include "ee/vu.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <immintrin.h>

#include "ee/vu_ops.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include "timer.h"


bool vuLowerCommon(VuRegs r, uint32_t op, uint8_t* data, uint32_t dataQwords, uint32_t* q) {
  const uint32_t dest = (op >> 21) & 0xF;
  const uint32_t ft = (op >> 16) & 31, fs = (op >> 11) & 31;
  const int fsf = (op >> 21) & 3, ftf = (op >> 23) & 3;
  u128* vf = r.vf;
  uint32_t* vi = r.vi;
  auto on = [&](int i) { return (dest >> (3 - i)) & 1; };
  auto qword = [&](uint32_t index) { return data + (index & (dataQwords - 1)) * 16; };
  const uint32_t fn2 = ((op >> 4) & 0x7C) | (op & 3);
  switch (fn2) {
    case 0x30: {  // MOVE
      const u128 src = vf[fs];
      for (int i = 0; i < 4; ++i)
        if (on(i) && ft) vf[ft].uw[i] = src.uw[i];
      return true;
    }
    case 0x31: {  // MR32: rotate lanes
      const u128 src = vf[fs];
      for (int i = 0; i < 4; ++i)
        if (on(i) && ft) vf[ft].uw[i] = src.uw[(i + 1) & 3];
      return true;
    }
    case 0x34: case 0x35: case 0x36: case 0x37: {  // LQI, SQI, LQD, SQD
      const bool store128 = fn2 & 1, decrement = fn2 & 2;
      const uint32_t ireg = (store128 ? ft : fs) & 15, vreg = store128 ? fs : ft;
      if (decrement && ireg) vi[ireg] = (vi[ireg] - 1) & 0xFFFF;
      uint8_t* p = qword(vi[ireg]);
      for (int i = 0; i < 4; ++i) {
        if (!on(i)) continue;
        if (store128)
          std::memcpy(p + 4 * i, &vf[vreg].uw[i], 4);
        else if (vreg)
          std::memcpy(&vf[vreg].uw[i], p + 4 * i, 4);
      }
      if (!decrement && ireg) vi[ireg] = (vi[ireg] + 1) & 0xFFFF;
      return true;
    }
    case 0x38: *q = divide(vf[fs].f[fsf], vf[ft].f[ftf]); return true;              // DIV
    case 0x39: *q = asBits(std::sqrt(std::fabs(vf[ft].f[ftf]))); return true;       // SQRT
    case 0x3A: *q = divide(vf[fs].f[fsf], std::sqrt(std::fabs(vf[ft].f[ftf]))); return true;  // RSQRT
    case 0x3C: if (ft & 15) vi[ft & 15] = vf[fs].uw[fsf] & 0xFFFF; return true;     // MTIR
    case 0x3D: {                                                                     // MFIR
      const int32_t v = int16_t(vi[fs & 15]);
      for (int i = 0; i < 4; ++i)
        if (on(i) && ft) vf[ft].sw[i] = v;
      return true;
    }
    case 0x3E: case 0x3F: {  // ILWR / ISWR: 16-bit integer in the selected lane(s)
      uint8_t* p = qword(vi[fs & 15]);
      if (fn2 == 0x3E) {
        const int lane = dest & 8 ? 0 : dest & 4 ? 1 : dest & 2 ? 2 : 3;
        uint32_t v;
        std::memcpy(&v, p + 4 * lane, 4);
        if (ft & 15) vi[ft & 15] = v & 0xFFFF;
      } else {
        const uint32_t v = vi[ft & 15] & 0xFFFF;
        for (int i = 0; i < 4; ++i)
          if (on(i)) std::memcpy(p + 4 * i, &v, 4);
      }
      return true;
    }
    case 0x40: {  // RNEXT
      uint32_t rr = vi[kVuR];
      const uint32_t x = (rr >> 4) & 1, y = (rr >> 22) & 1;
      rr = (rr << 1) ^ x ^ y;
      vi[kVuR] = (rr & 0x7FFFFF) | 0x3F800000;
      [[fallthrough]];
    }
    case 0x41:  // RGET
      for (int i = 0; i < 4; ++i)
        if (on(i) && ft) vf[ft].uw[i] = vi[kVuR];
      return true;
    case 0x42: vi[kVuR] = 0x3F800000 | (vf[fs].uw[fsf] & 0x7FFFFF); return true;              // RINIT
    case 0x43: vi[kVuR] = 0x3F800000 | ((vi[kVuR] ^ vf[fs].uw[fsf]) & 0x7FFFFF); return true;  // RXOR
  }
  return false;
}

// ---- macro mode -----------------------------------------------------------------------------

void Cpu::cop2Macro(uint32_t op, uint32_t pc) {
  VuRegs r{s.vf, s.vi, &s.vacc};
  if (vuUpper(r, op)) return;
  uint32_t* vi = s.vi;
  const uint32_t ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  switch (op & 63) {
    case 0x30: if (fd & 15) vi[fd & 15] = (vi[fs & 15] + vi[ft & 15]) & 0xFFFF; return;  // VIADD
    case 0x31: if (fd & 15) vi[fd & 15] = (vi[fs & 15] - vi[ft & 15]) & 0xFFFF; return;  // VISUB
    case 0x32: {                                                                       // VIADDI
      const int32_t imm = int32_t(fd << 27) >> 27;
      if (ft & 15) vi[ft & 15] = (vi[fs & 15] + imm) & 0xFFFF;
      return;
    }
    case 0x34: if (fd & 15) vi[fd & 15] = vi[fs & 15] & vi[ft & 15]; return;  // VIAND
    case 0x35: if (fd & 15) vi[fd & 15] = vi[fs & 15] | vi[ft & 15]; return;  // VIOR
    case 0x38: case 0x39: {  // VCALLMS / VCALLMSR: run a VU0 micro program to completion
      const uint32_t addr = (op & 63) == 0x38 ? ((op >> 6) & 0x7FFF) * 8 : vi[27] * 8;
      static VuMicro vu0(r, mem.hostPtr(0x11000000, 4096), 4096, mem.hostPtr(0x11004000, 4096), 4096);
      vu0.codeChanged();  // the EE may have rewritten micro memory since the last call
      vu0.run(addr);
      if (!vu0.fault.empty()) halt(StopReason::Unimplemented, "VU0 micro: " + vu0.fault);
      return;
    }
  }
  if ((op & 63) >= 0x3C) {
    const uint32_t fn2 = ((op >> 4) & 0x7C) | (op & 3);
    if (fn2 == 0x3B) return;  // VWAITQ: results are immediate in macro mode
    if (vuLowerCommon(r, op, mem.hostPtr(0x11004000, 4096), 256, &vi[kVuQ])) return;
  }
  unimplemented(op, pc, "VU0 macro instruction");
}

// ---- micro mode -----------------------------------------------------------------------------


// True if MAC/status results of the upper instruction at `pc` may be read: by a flag-reading
// lower instruction kFlagLatency or more pairs later and before the next flag writer's results
// arrive, across a branch or the end of the program, or too far ahead to tell.
bool VuMicro::flagsNeeded(uint32_t pc) const {
  int nextWriter = -1;
  for (int n = 0; n < 64; ++n) {
    uint32_t lo, hi;
    std::memcpy(&lo, code_ + pc, 4);
    std::memcpy(&hi, code_ + pc + 4, 4);
    if (nextWriter >= 0 && n >= nextWriter + kFlagLatency) return false;
    if (!(hi & 0x80000000u) && lowerReadsFlags(lo) && n >= kFlagLatency) return true;
    if (!(hi & 0x80000000u) && lowerBranches(lo)) return true;
    if (hi & 0x40000000u) return true;
    if (n > 0 && nextWriter < 0 && upperSetsFlags(hi)) nextWriter = n;
    pc = (pc + 8) & (codeBytes_ - 1);
  }
  return true;
}

const VuMicro::Pair& VuMicro::decode(uint32_t pc) {
  if (pairs_.empty()) pairs_.assign(codeBytes_ / 8, Pair{});
  Pair& p = pairs_[pc / 8];
  uint32_t lo, hi;
  std::memcpy(&lo, code_ + pc, 4);
  std::memcpy(&hi, code_ + pc + 4, 4);
  if (p.generation == generation_) return p;
  p = Pair{};
  p.hi = hi, p.lo = lo, p.generation = generation_;
  p.fs = (hi >> 11) & 31, p.ft = (hi >> 16) & 31, p.fd = (hi >> 6) & 31, p.dest = (hi >> 21) & 0xF, p.bc = hi & 3;
  p.iBit = hi & 0x80000000u, p.eBit = hi & 0x40000000u;
  if (!p.iBit && (lo >> 25) == 0x40 && (lo & 63) >= 0x3C) {
    const uint32_t lfn2 = ((lo >> 4) & 0x7C) | (lo & 3);
    p.waitQ = lfn2 == 0x3B, p.waitP = lfn2 == 0x7B;
  }
  p.flags = upperSetsFlags(hi) && flagsNeeded(pc);

  // Upper half.
  struct Named {
    UpperFn fn;
    const char* name;
  };
#define A(o, b) Named{&VuMicro::upperArith<o, b, false>, "VuMicro::upperArith<vuops::" #o ", vuops::" #b ", false>"}
#define C(o, b) Named{&VuMicro::upperArith<o, b, true>, "VuMicro::upperArith<vuops::" #o ", vuops::" #b ", true>"}
#define N Named{nullptr, nullptr}
  static const Named kVector[0x30] = {
      A(kAdd, kBc), A(kAdd, kBc), A(kAdd, kBc), A(kAdd, kBc), A(kSub, kBc), A(kSub, kBc), A(kSub, kBc), A(kSub, kBc),
      A(kMadd, kBc), A(kMadd, kBc), A(kMadd, kBc), A(kMadd, kBc), A(kMsub, kBc), A(kMsub, kBc), A(kMsub, kBc), A(kMsub, kBc),
      A(kMax, kBc), A(kMax, kBc), A(kMax, kBc), A(kMax, kBc), A(kMin, kBc), A(kMin, kBc), A(kMin, kBc), A(kMin, kBc),
      A(kMul, kBc), A(kMul, kBc), A(kMul, kBc), A(kMul, kBc), A(kMul, kQ), A(kMax, kI), A(kMul, kI), A(kMin, kI),
      A(kAdd, kQ), A(kMadd, kQ), A(kAdd, kI), A(kMadd, kI), A(kSub, kQ), A(kMsub, kQ), A(kSub, kI), A(kMsub, kI),
      A(kAdd, kFt), A(kMadd, kFt), A(kMul, kFt), A(kMax, kFt), A(kSub, kFt), A(kMsub, kFt), N, A(kMin, kFt)};
  static const Named kAcc[0x30] = {
      C(kAdd, kBc), C(kAdd, kBc), C(kAdd, kBc), C(kAdd, kBc), C(kSub, kBc), C(kSub, kBc), C(kSub, kBc), C(kSub, kBc),
      C(kMadd, kBc), C(kMadd, kBc), C(kMadd, kBc), C(kMadd, kBc), C(kMsub, kBc), C(kMsub, kBc), C(kMsub, kBc), C(kMsub, kBc),
      N, N, N, N, N, N, N, N,
      C(kMul, kBc), C(kMul, kBc), C(kMul, kBc), C(kMul, kBc), C(kMul, kQ), N, C(kMul, kI), N,
      C(kAdd, kQ), C(kMadd, kQ), C(kAdd, kI), C(kMadd, kI), C(kSub, kQ), C(kMsub, kQ), C(kSub, kI), C(kMsub, kI),
      C(kAdd, kFt), C(kMadd, kFt), C(kMul, kFt), N, C(kSub, kFt), C(kMsub, kFt), N, Named{&VuMicro::upperNop, "VuMicro::upperNop"}};
#undef A
#undef C
#undef N
#define U(f) Named{&VuMicro::f, "VuMicro::" #f}
  static const Named kConvert[8] = {U(upperItof<0>), U(upperItof<4>), U(upperItof<12>), U(upperItof<15>),
                                    U(upperFtoi<0>), U(upperFtoi<4>), U(upperFtoi<12>), U(upperFtoi<15>)};
  const uint32_t funct = hi & 63, fn2 = ((hi >> 4) & 0x7C) | (hi & 3);
  Named upper = funct < 0x30 ? kVector[funct] : funct >= 0x3C && fn2 < 0x30 ? kAcc[fn2] : Named{nullptr, nullptr};
  if (funct >= 0x3C) {
    if (fn2 >= 0x10 && fn2 <= 0x17) upper = kConvert[fn2 - 0x10];
    if (fn2 == 0x1F) upper = U(upperClip);
    if (fn2 == 0x1D) upper = U(upperAbs);
  }
  if (!upper.fn) upper = U(upperGeneric);
#undef U
  p.upper = upper.fn, p.upperName = upper.name;
  if (p.upper == &VuMicro::upperGeneric) p.flags = true;  // the generic path always writes them

  // What the upper half writes: one vector register (and/or ACC, which lower halves never touch).
  const bool writesFd = funct <= 0x2F;
  const bool writesFt = funct >= 0x3C && ((fn2 >= 0x10 && fn2 <= 0x17) || fn2 == 0x1D);
  const uint32_t written = writesFd ? p.fd : writesFt ? p.ft : 0;

  // Lower half.
  p.is = (lo >> 11) & 15, p.it = (lo >> 16) & 15, p.lfs = (lo >> 11) & 31, p.lft = (lo >> 16) & 31;
  p.ldest = (lo >> 21) & 0xF;
#define L(f) (p.lower = &VuMicro::f, p.lowerName = "VuMicro::" #f)
  L(lowerGeneric);
  if (!p.iBit) {
    const uint32_t opc = lo >> 25;
    if (opc == 0x00 && p.lft) L(lowerLq), p.imm = int32_t(lo << 21) >> 21;
    else if (opc == 0x01) L(lowerSq), p.imm = int32_t(lo << 21) >> 21;
    else if ((opc >= 0x20 && opc <= 0x21) || (opc >= 0x24 && opc <= 0x25) || (opc >= 0x28 && opc <= 0x29) || (opc >= 0x2C && opc <= 0x2F))
      L(lowerBranch), p.imm = int32_t(lo << 21) >> 21;
    else if (opc == 0x04 || opc == 0x05) L(lowerInteger), p.imm = int32_t(lo << 21) >> 21;
    else if ((opc >= 0x10 && opc <= 0x13) || opc == 0x18 || opc == 0x1A || opc == 0x1B || opc == 0x1C) L(lowerInteger);
    else if (opc == 0x08) L(lowerIaddiu), p.imm = int32_t(((lo >> 10) & 0x7800) | (lo & 0x7FF));
    else if (opc == 0x09) L(lowerIsubiu), p.imm = int32_t(((lo >> 10) & 0x7800) | (lo & 0x7FF));
    else if (opc == 0x40) {
      const uint32_t lfn2 = ((lo >> 4) & 0x7C) | (lo & 3);
      if ((lo & 63) == 0x30) L(lowerIadd);
      else if ((lo & 63) == 0x32) L(lowerIaddi), p.imm = int32_t(lo << 21) >> 27;
      else if ((lo & 63) >= 0x3C && lfn2 == 0x30)
        (p.ldest && p.lft ? L(lowerMove) : L(lowerNop));
      else if ((lo & 63) >= 0x3C && lfn2 == 0x34 && p.lft) L(lowerLqi);
      else if ((lo & 63) >= 0x3C && lfn2 == 0x35) L(lowerSqi);
      else if ((lo & 63) == 0x31 || (lo & 63) == 0x34 || (lo & 63) == 0x35) L(lowerInteger);
      else if ((lo & 63) >= 0x3C && lfn2 == 0x3D) (p.lft && p.ldest ? L(lowerMfir) : L(lowerNop));
      else if ((lo & 63) >= 0x3C && lfn2 == 0x3C) L(lowerMtir);
      else if ((lo & 63) >= 0x3C && lfn2 >= 0x38 && lfn2 <= 0x3A) L(lowerDiv);
      else if ((lo & 63) >= 0x3C && (lfn2 == 0x3B || lfn2 == 0x7B)) L(lowerNop);  // WAITQ/WAITP: the pair already waited
      else if ((lo & 63) >= 0x3C && lfn2 == 0x6C) L(lowerXgkick);
    }
    if (written && !lowerVectorFree(lo)) p.setAside = uint8_t(written);
  }
#undef L
  return p;
}

// ---- compiled blocks ----------------------------------------------------------------------

uint64_t vuBlockHash(uint32_t pc, const uint8_t* code, int pairs) {
  uint64_t h = 0xCBF29CE484222325ull ^ pc;
  for (int i = 0; i < pairs * 8; ++i) h = (h ^ code[i]) * 0x100000001B3ull;
  return h;
}

int VuMicro::blockLength(uint32_t pc) const {
  int n = 0;
  for (uint32_t a = pc; a + 8 <= codeBytes_ && n < kMaxBlock;) {
    uint32_t lo, hi;
    std::memcpy(&lo, code_ + a, 4);
    std::memcpy(&hi, code_ + a + 4, 4);
    ++n;
    a += 8;
    if ((!(hi & 0x80000000u) && lowerBranches(lo)) || (hi & 0x40000000u)) {  // plus the delay slot
      if (a + 8 <= codeBytes_) ++n;
      break;
    }
  }
  return n;
}

namespace {
struct Collected {
  uint32_t pc;
  std::vector<uint32_t> words;
};
std::unordered_map<uint64_t, VuBlockEntry>& blockRegistry() {
  static std::unordered_map<uint64_t, VuBlockEntry> registry = [] {
    std::vector<VuBlockEntry> entries;
    registerVuBlocks(entries);
    std::unordered_map<uint64_t, VuBlockEntry> m;
    for (const VuBlockEntry& e : entries) m.emplace(e.hash, e);
    return m;
  }();
  return registry;
}
const char* collectPath() {
  static const char* path = std::getenv("CODPORT_VU_COLLECT");
  return path;
}
std::mutex gCollectMutex;
std::unordered_map<uint64_t, Collected> gCollected;
}  // namespace

VuMicro::BlockFn VuMicro::findBlock(uint32_t pc) {
  const int n = blockLength(pc);
  if (n == 0) return nullptr;
  const uint64_t hash = vuBlockHash(pc, code_ + pc, n);
  const auto& registry = blockRegistry();
  if (auto it = registry.find(hash); it != registry.end() && it->second.pc == pc && int(it->second.pairs) == n) return it->second.fn;
  if (collectPath()) {
    std::lock_guard lock(gCollectMutex);
    Collected& c = gCollected[hash];
    if (c.words.empty()) {
      c.pc = pc;
      c.words.resize(size_t(n) * 2);
      std::memcpy(c.words.data(), code_ + pc, size_t(n) * 8);
    }
  }
  return nullptr;
}

void vuCollectSave() {
  const char* path = collectPath();
  if (!path) return;
  std::lock_guard lock(gCollectMutex);
  // Keep what earlier runs collected.
  std::unordered_map<uint64_t, Collected> all;
  if (std::ifstream in(path); in) {
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream ls(line);
      Collected c;
      uint32_t n = 0;
      ls >> std::hex >> c.pc >> n;
      c.words.resize(size_t(n) * 2);
      for (uint32_t& w : c.words) ls >> w;
      if (ls && n) all.emplace(vuBlockHash(c.pc, reinterpret_cast<const uint8_t*>(c.words.data()), int(n)), std::move(c));
    }
  }
  for (auto& [hash, c] : gCollected) all.emplace(hash, c);
  std::ofstream out(path);
  for (const auto& [hash, c] : all) {
    out << std::hex << c.pc << ' ' << c.words.size() / 2;
    for (uint32_t w : c.words) out << ' ' << w;
    out << '\n';
  }
  std::printf("VU blocks: %zu collected in %s\n", all.size(), path);
}

namespace {
// The VUs round toward zero; programs depend on it where a value sits on a clipping boundary.
struct RoundTowardZero {
  unsigned old = _mm_getcsr();
  RoundTowardZero() { _mm_setcsr(old | 0x6000u); }
  ~RoundTowardZero() { _mm_setcsr(old); }
};
}  // namespace

void VuMicro::run(uint32_t startAddr) {
  ScopedTimer timer(runNanos);
  if (codeTouches && *codeTouches != seenTouches_) seenTouches_ = *codeTouches, codeChanged();
  const RoundTowardZero rounding;
  ++programs;
  uint32_t pc = startAddr & (codeBytes_ - 1);
  bool ending = false, entry = true, lastWasBranch = false;
  int sinceEntry = 0;
  branchPending_ = false;
  for (uint64_t guard = 0; guard < 50'000'000; ++guard) {
    // Blocks start at the program start, after a branch's delay slot and every kMaxBlock pairs.
    if (entry && !branchPending_ && !ending) {
      if (blocks_.empty()) blocks_.assign(codeBytes_ / 8, BlockCache{});
      BlockCache& b = blocks_[pc / 8];
      if (b.generation != generation_) b.generation = generation_, b.fn = findBlock(pc);
      if (b.fn) {
        pc = b.fn(*this) & (codeBytes_ - 1);
        if (!fault.empty()) {
          fault += std::format(" (in the compiled block before 0x{:04X})", pc);
          return;
        }
        if (blockEnded_) {
          blockEnded_ = false;
          tpc = pc;
          settleFlags(true);
          return;
        }
        lastWasBranch = false, sinceEntry = 0;
        continue;
      }
    }
    const Pair& p = decode(pc);
    pairStart();
    const bool delaySlot = lastWasBranch;

    const bool takeBranch = branchPending_;
    const uint32_t target = branchTarget_;
    branchPending_ = false;

    // The pair executes in parallel: the lower instruction sees registers as they were before
    // the upper one writes. The upper instruction writes at most one vector register (and ACC,
    // which lower instructions do not use), so that register is set aside while the lower one
    // runs; if both write it, the upper one wins.
    pairStall(p.waitQ, p.waitP);
    if (p.setAside) {
      u128& reg = r_.vf[p.setAside];
      const u128 before = reg;
      p.upper(*this, p);
      const u128 upperResult = reg;
      reg = before;
      p.lower(*this, p, pc);
      reg = upperResult;
    } else {
      p.upper(*this, p);
      if (p.iBit)  // the lower word is a float loaded into I
        r_.vi[kVuI] = p.lo;
      else
        p.lower(*this, p, pc);
    }
    r_.vi[0] = 0;
    if (!fault.empty()) {
      fault += std::format(" at 0x{:04X}", pc);
      return;
    }

    lastWasBranch = !p.iBit && lowerBranches(p.lo);
    entry = delaySlot || ++sinceEntry >= kMaxBlock;
    if (entry) sinceEntry = 0;
    pc = takeBranch ? target & (codeBytes_ - 1) : (pc + 8) & (codeBytes_ - 1);
    if (ending) {
      tpc = pc;
      settleFlags(true);
      return;
    }
    if (p.eBit) ending = true;  // E bit: stop after the next pair
  }
  fault = "micro program did not end";
}

int VuMicro::qLatency(uint32_t lo) {
  if ((lo >> 25) != 0x40 || (lo & 63) < 0x3C) return 0;
  const uint32_t fn2 = ((lo >> 4) & 0x7C) | (lo & 3);
  return fn2 == 0x3A ? 13 : fn2 == 0x38 || fn2 == 0x39 ? 7 : 0;  // RSQRT; DIV, SQRT
}

int VuMicro::pLatency(uint32_t lo) {
  if ((lo >> 25) != 0x40 || (lo & 63) < 0x3C) return 0;
  switch (((lo >> 4) & 0x7C) | (lo & 3)) {
    case 0x70: return 11;                        // ESADD
    case 0x71: case 0x72: case 0x79: return 18;  // ERSADD, ELENG, ERSQRT
    case 0x73: return 24;                        // ERLENG
    case 0x74: case 0x75: case 0x7D: return 54;  // EATANxy, EATANxz, EATAN
    case 0x76: case 0x78: case 0x7A: return 12;  // ESUM, ESQRT, ERCPR
    case 0x7C: return 29;                        // ESIN
    case 0x7E: return 44;                        // EEXP
  }
  return 0;
}

void VuMicro::lower(uint32_t op, uint32_t pc) {
  uint32_t* vi = r_.vi;
  u128* vf = r_.vf;
  const uint32_t it = (op >> 16) & 15, is = (op >> 11) & 15, ft = (op >> 16) & 31, fs = (op >> 11) & 31;
  const uint32_t dest = (op >> 21) & 0xF;
  const int32_t imm11 = int32_t(op << 21) >> 21;
  const uint32_t dataQwords = dataBytes_ / 16;
  auto qword = [&](uint32_t index) { return data_ + (index & (dataQwords - 1)) * 16; };
  auto setI = [&](uint32_t reg, uint32_t v) {
    if (reg) vi[reg] = v & 0xFFFF;
  };
  auto branchOff = [&](bool cond) {
    if (cond) branchTo(pc + 8 + uint32_t(imm11) * 8);
  };
  const uint32_t opc = op >> 25;
  switch (opc) {
    case 0x00: {  // LQ ft, imm(is)
      uint8_t* p = qword(vi[is] + imm11);
      for (int i = 0; i < 4; ++i)
        if (((dest >> (3 - i)) & 1) && ft) std::memcpy(&vf[ft].uw[i], p + 4 * i, 4);
      return;
    }
    case 0x01: {  // SQ fs, imm(it)
      uint8_t* p = qword(vi[it] + imm11);
      for (int i = 0; i < 4; ++i)
        if ((dest >> (3 - i)) & 1) std::memcpy(p + 4 * i, &vf[fs].uw[i], 4);
      return;
    }
    case 0x04: {  // ILW it, imm(is)
      const int lane = dest & 8 ? 0 : dest & 4 ? 1 : dest & 2 ? 2 : 3;
      uint32_t v;
      std::memcpy(&v, qword(vi[is] + imm11) + 4 * lane, 4);
      setI(it, v);
      return;
    }
    case 0x05: {  // ISW it, imm(is)
      uint8_t* p = qword(vi[is] + imm11);
      const uint32_t v = vi[it] & 0xFFFF;
      for (int i = 0; i < 4; ++i)
        if ((dest >> (3 - i)) & 1) std::memcpy(p + 4 * i, &v, 4);
      return;
    }
    case 0x08: setI(it, vi[is] + (((op >> 10) & 0x7800) | (op & 0x7FF))); return;  // IADDIU
    case 0x09: setI(it, vi[is] - (((op >> 10) & 0x7800) | (op & 0x7FF))); return;  // ISUBIU
    case 0x10: vi[1] = (vi[kVuClip] & 0xFFFFFF) == (op & 0xFFFFFF); return;          // FCEQ
    case 0x11: vi[kVuClip] = op & 0xFFFFFF; return;                                  // FCSET
    case 0x12: vi[1] = (vi[kVuClip] & op & 0xFFFFFF) != 0; return;                   // FCAND
    case 0x13: vi[1] = ((vi[kVuClip] | op) & 0xFFFFFF) == 0xFFFFFF; return;          // FCOR
    case 0x14: case 0x15: case 0x16: case 0x17: {  // FSEQ, FSSET, FSAND, FSOR
      const uint32_t imm12 = ((op >> 10) & 0x800) | (op & 0x7FF), st = vi[kVuStatus] & 0xFFF;
      if (opc == 0x14) setI(it, st == imm12);
      else if (opc == 0x15) vi[kVuStatus] = (vi[kVuStatus] & 0x3F) | (imm12 & 0xFC0);
      else if (opc == 0x16) setI(it, st & imm12);
      else setI(it, (st | imm12) == 0xFFF);
      return;
    }
    case 0x18: setI(it, (vi[kVuMac] & 0xFFFF) == (vi[is] & 0xFFFF)); return;  // FMEQ
    case 0x1A: setI(it, vi[kVuMac] & vi[is]); return;                          // FMAND
    case 0x1B: setI(it, (vi[kVuMac] | vi[is]) == 0xFFFF); return;              // FMOR
    case 0x1C: setI(it, vi[kVuClip] & 0xFFF); return;                          // FCGET
    case 0x20: branchOff(true); return;                                        // B
    case 0x21: setI(it, (pc + 16) / 8); branchOff(true); return;               // BAL
    case 0x24: branchTo((vi[is] & 0xFFFF) * 8); return;                        // JR
    case 0x25: {                                                               // JALR
      const uint32_t target = (vi[is] & 0xFFFF) * 8;
      setI(it, (pc + 16) / 8);
      branchTo(target);
      return;
    }
    case 0x28: branchOff((vi[it] & 0xFFFF) == (vi[is] & 0xFFFF)); return;  // IBEQ
    case 0x29: branchOff((vi[it] & 0xFFFF) != (vi[is] & 0xFFFF)); return;  // IBNE
    case 0x2C: branchOff(int16_t(vi[is]) < 0); return;                     // IBLTZ
    case 0x2D: branchOff(int16_t(vi[is]) > 0); return;                     // IBGTZ
    case 0x2E: branchOff(int16_t(vi[is]) <= 0); return;                    // IBLEZ
    case 0x2F: branchOff(int16_t(vi[is]) >= 0); return;                    // IBGEZ
    case 0x40: break;  // lower-op table below
    default:
      fault = std::format("lower instruction 0x{:08X} at 0x{:04X}", op, pc);
      return;
  }

  const uint32_t id = (op >> 6) & 15;
  switch (op & 63) {
    case 0x30: setI(id, vi[is] + vi[it]); return;  // IADD
    case 0x31: setI(id, vi[is] - vi[it]); return;  // ISUB
    case 0x32: setI(it, vi[is] + (int32_t(op << 21) >> 27)); return;  // IADDI imm5
    case 0x34: setI(id, vi[is] & vi[it]); return;  // IAND
    case 0x35: setI(id, vi[is] | vi[it]); return;  // IOR
  }
  if ((op & 63) < 0x3C) {
    fault = std::format("lower instruction 0x{:08X} at 0x{:04X}", op, pc);
    return;
  }
  const uint32_t fn2 = ((op >> 4) & 0x7C) | (op & 3);
  const int fsf = (op >> 21) & 3;
  auto efu = [&](float v) {
    pendingP_ = std::bit_cast<uint32_t>(clampf(v));
    pDelay_ = pLatency(op);
  };
  const float* x = vf[fs].f;
  switch (fn2) {
    case 0x38: case 0x39: case 0x3A: {  // DIV / SQRT / RSQRT: Q arrives a few cycles later
      vuLowerCommon(r_, op, data_, dataQwords, &pendingQ_);
      qDelay_ = qLatency(op);
      return;
    }
    case 0x3B:  // WAITQ
      if (qDelay_ > 0) vi[kVuQ] = pendingQ_, qDelay_ = 0;
      return;
    case 0x64: {  // MFP
      for (int i = 0; i < 4; ++i)
        if (((dest >> (3 - i)) & 1) && ft) vf[ft].uw[i] = p_;
      return;
    }
    case 0x68: setI(it, top ? top() : 0); return;    // XTOP
    case 0x69: setI(it, itop ? itop() : 0); return;  // XITOP
    case 0x6C:                                       // XGKICK
      if (xgkick) xgkick(vi[is] & 0xFFFF);
      return;
    case 0x70: efu(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); return;                     // ESADD
    case 0x71: efu(1.0f / (x[0] * x[0] + x[1] * x[1] + x[2] * x[2])); return;            // ERSADD
    case 0x72: efu(std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2])); return;          // ELENG
    case 0x73: efu(1.0f / std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2])); return;   // ERLENG
    case 0x74: efu(std::atan2(x[1], x[0])); return;                                      // EATANxy
    case 0x75: efu(std::atan2(x[2], x[0])); return;                                      // EATANxz
    case 0x76: efu(x[0] + x[1] + x[2] + x[3]); return;                                   // ESUM
    case 0x78: efu(std::sqrt(std::fabs(x[fsf]))); return;                                // ESQRT
    case 0x79: efu(1.0f / std::sqrt(std::fabs(x[fsf]))); return;                         // ERSQRT
    case 0x7A: efu(1.0f / x[fsf]); return;                                               // ERCPR
    case 0x7B:                                                                               // WAITP
      if (pDelay_ > 0) p_ = pendingP_, pDelay_ = 0;
      return;
    case 0x7C: efu(std::sin(x[fsf])); return;   // ESIN
    case 0x7D: efu(std::atan(x[fsf])); return;  // EATAN
    case 0x7E: efu(std::exp(-x[fsf])); return;  // EEXP
  }
  uint32_t q;
  if (vuLowerCommon(r_, op, data_, dataQwords, &q)) return;
  fault = std::format("lower instruction 0x{:08X} at 0x{:04X}", op, pc);
}
