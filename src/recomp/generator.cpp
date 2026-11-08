#include "recomp/generator.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "ee/memory.h"
#include "ee/vu_ops.h"

namespace fs = std::filesystem;

namespace {
struct Range {
  uint32_t lo, hi;  // inclusive byte addresses
};
struct Function {
  uint32_t entry;
  std::vector<Range> ranges;
};

uint32_t field(uint32_t op, int lo, int n) { return (op >> lo) & ((1u << n) - 1); }

// ---- instruction classes ---------------------------------------------------------------------

bool isBranchOrJump(uint32_t op) {
  const uint32_t opc = op >> 26;
  switch (opc) {
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17:
      return true;
    case 0x00: return (op & 63) == 0x08 || (op & 63) == 0x09;  // JR, JALR
    case 0x01: {
      const uint32_t rt = field(op, 16, 5);
      return rt <= 3 || (rt >= 0x10 && rt <= 0x13);
    }
    case 0x10: case 0x11: case 0x12: return field(op, 21, 5) == 8;  // BCnx
  }
  return false;
}

// Instructions recompiled code leaves to the interpreter: they switch threads, change the
// interrupt state, return from exceptions, or have no translation.
bool isTerminator(uint32_t op) {
  const uint32_t opc = op >> 26;
  switch (opc) {
    case 0x00: return (op & 63) == 0x0C || (op & 63) == 0x0D;  // SYSCALL, BREAK
    case 0x10: {                                               // COP0
      const uint32_t rs = field(op, 21, 5);
      return rs != 0;  // MFC0 is harmless; MTC0, BC0x, TLB ops, ERET, EI, DI end the block
    }
    case 0x12: return field(op, 21, 5) == 8;  // BC2x: VU0 status
    case 0x13: case 0x1D: case 0x30: case 0x32: case 0x34: case 0x35: case 0x38: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D:
      return true;  // not R5900 instructions
  }
  return false;
}

std::string hex(uint32_t v) { return std::format("0x{:08X}u", v); }

// C++ for the MMI instructions with an exact SSE equivalent (same results as Cpu::mmi*);
// empty if there is none.
std::string emitMmi(uint32_t op) {
  const uint32_t rs = field(op, 21, 5), rt = field(op, 16, 5), rd = field(op, 11, 5), sa = field(op, 6, 5);
  const std::string a = std::format("rc::ld(s, {})", rs), b = std::format("rc::ld(s, {})", rt);
  auto bin = [&](const char* f, const std::string& x, const std::string& y) { return std::format("{}({}, {})", f, x, y); };
  std::string v;
  switch (op & 63) {
    case 0x08:  // MMI0
      switch (sa) {
        case 0x00: v = bin("_mm_add_epi32", a, b); break;     // PADDW
        case 0x01: v = bin("_mm_sub_epi32", a, b); break;     // PSUBW
        case 0x02: v = bin("_mm_cmpgt_epi32", a, b); break;   // PCGTW
        case 0x03: v = bin("_mm_max_epi32", a, b); break;     // PMAXW
        case 0x04: v = bin("_mm_add_epi16", a, b); break;     // PADDH
        case 0x05: v = bin("_mm_sub_epi16", a, b); break;     // PSUBH
        case 0x06: v = bin("_mm_cmpgt_epi16", a, b); break;   // PCGTH
        case 0x07: v = bin("_mm_max_epi16", a, b); break;     // PMAXH
        case 0x08: v = bin("_mm_add_epi8", a, b); break;      // PADDB
        case 0x09: v = bin("_mm_sub_epi8", a, b); break;      // PSUBB
        case 0x0A: v = bin("_mm_cmpgt_epi8", a, b); break;    // PCGTB
        case 0x12: v = bin("_mm_unpacklo_epi32", b, a); break;  // PEXTLW
        case 0x13: v = std::format("_mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps({}), _mm_castsi128_ps({}), 0x88))", b, a); break;  // PPACW
        case 0x14: v = bin("_mm_adds_epi16", a, b); break;    // PADDSH
        case 0x15: v = bin("_mm_subs_epi16", a, b); break;    // PSUBSH
        case 0x16: v = bin("_mm_unpacklo_epi16", b, a); break;  // PEXTLH
        case 0x17: v = bin("rc::pack16", a, b); break;        // PPACH
        case 0x18: v = bin("_mm_adds_epi8", a, b); break;     // PADDSB
        case 0x19: v = bin("_mm_subs_epi8", a, b); break;     // PSUBSB
        case 0x1A: v = bin("_mm_unpacklo_epi8", b, a); break;   // PEXTLB
        case 0x1B: v = bin("rc::pack8", a, b); break;         // PPACB
      }
      break;
    case 0x28:  // MMI1
      switch (sa) {
        case 0x02: v = bin("_mm_cmpeq_epi32", a, b); break;   // PCEQW
        case 0x03: v = bin("_mm_min_epi32", a, b); break;     // PMINW
        case 0x06: v = bin("_mm_cmpeq_epi16", a, b); break;   // PCEQH
        case 0x07: v = bin("_mm_min_epi16", a, b); break;     // PMINH
        case 0x0A: v = bin("_mm_cmpeq_epi8", a, b); break;    // PCEQB
        case 0x12: v = bin("_mm_unpackhi_epi32", b, a); break;  // PEXTUW
        case 0x14: v = bin("_mm_adds_epu16", a, b); break;    // PADDUH
        case 0x15: v = bin("_mm_subs_epu16", a, b); break;    // PSUBUH
        case 0x16: v = bin("_mm_unpackhi_epi16", b, a); break;  // PEXTUH
        case 0x18: v = bin("_mm_adds_epu8", a, b); break;     // PADDUB
        case 0x19: v = bin("_mm_subs_epu8", a, b); break;     // PSUBUB
        case 0x1A: v = bin("_mm_unpackhi_epi8", b, a); break;   // PEXTUB
        case 0x1B: v = std::format("rc::qfsrv(s, {}, {})", a, b); break;  // QFSRV
      }
      break;
    case 0x09:  // MMI2
      switch (sa) {
        case 0x08: v = "_mm_loadu_si128(reinterpret_cast<const __m128i*>(&s.hi))"; break;  // PMFHI
        case 0x09: v = "_mm_loadu_si128(reinterpret_cast<const __m128i*>(&s.lo))"; break;  // PMFLO
        case 0x0A: v = std::format("_mm_unpacklo_epi16({}, _mm_srli_si128({}, 8))", b, a); break;  // PINTH
        case 0x0E: v = bin("_mm_unpacklo_epi64", b, a); break;  // PCPYLD
        case 0x12: v = bin("_mm_and_si128", a, b); break;     // PAND
        case 0x13: v = bin("_mm_xor_si128", a, b); break;     // PXOR
      }
      break;
    case 0x29:  // MMI3
      switch (sa) {
        case 0x08: return std::format("s.hi = s.gpr[{}];", rs);  // PMTHI
        case 0x09: return std::format("s.lo = s.gpr[{}];", rs);  // PMTLO
        case 0x0E: v = bin("_mm_unpackhi_epi64", a, b); break;  // PCPYUD
        case 0x12: v = bin("_mm_or_si128", a, b); break;      // POR
        case 0x13: v = std::format("_mm_xor_si128(_mm_or_si128({}, {}), _mm_set1_epi32(-1))", a, b); break;  // PNOR
      }
      break;
    case 0x34: v = std::format("_mm_slli_epi16({}, {})", b, sa & 15); break;  // PSLLH
    case 0x36: v = std::format("_mm_srli_epi16({}, {})", b, sa & 15); break;  // PSRLH
    case 0x37: v = std::format("_mm_srai_epi16({}, {})", b, sa & 15); break;  // PSRAH
    case 0x3C: v = std::format("_mm_slli_epi32({}, {})", b, sa); break;       // PSLLW
    case 0x3E: v = std::format("_mm_srli_epi32({}, {})", b, sa); break;       // PSRLW
    case 0x3F: v = std::format("_mm_srai_epi32({}, {})", b, sa); break;       // PSRAW
  }
  if (v.empty()) return {};
  return rd ? std::format("rc::st(s, {}, {});", rd, v) : std::string(";");
}

// C++ for an FPU instruction as Cpu::cop1 executes it; empty if the interpreter must (BC1x are
// branches, and not seen here).
std::string emitFpu(uint32_t op) {
  const uint32_t rs = field(op, 21, 5), ft = field(op, 16, 5), fs = field(op, 11, 5), fd = field(op, 6, 5);
  const std::string F = std::format("s.fpr[{}]", fs), T = std::format("s.fpr[{}]", ft), D = std::format("s.fpr[{}]", fd);
  switch (rs) {
    case 0x00: return ft ? std::format("RC_R({}) = rc::sx32(std::bit_cast<uint32_t>({}));", ft, F) : "";  // MFC1
    case 0x02: return ft ? std::format("RC_R({}) = rc::sx32({});", ft, fs == 31 ? std::string("s.fcr31") : fs == 0 ? std::string("0x2E00u") : std::string("0u")) : "";  // CFC1
    case 0x04: return std::format("{} = std::bit_cast<float>(RC_W({}));", F, ft);  // MTC1
    case 0x06: return fs == 31 ? std::format("s.fcr31 = RC_W({});", ft) : std::string(";");  // CTC1
    case 0x10:
      switch (op & 63) {
        case 0x00: return std::format("{} = rc::fclamp({} + {});", D, F, T);
        case 0x01: return std::format("{} = rc::fclamp({} - {});", D, F, T);
        case 0x02: return std::format("{} = rc::fclamp({} * {});", D, F, T);
        case 0x03: return std::format("{} = rc::fdiv({}, {});", D, F, T);
        case 0x04: return std::format("{} = rc::fsqrt({});", D, T);
        case 0x05: return std::format("{} = std::fabs({});", D, F);
        case 0x06: return std::format("{} = {};", D, F);
        case 0x07: return std::format("{} = -{};", D, F);
        case 0x16: return std::format("{} = rc::frsqrt({}, {});", D, F, T);
        case 0x18: return std::format("s.acc = rc::fclamp({} + {});", F, T);
        case 0x19: return std::format("s.acc = rc::fclamp({} - {});", F, T);
        case 0x1A: return std::format("s.acc = rc::fclamp({} * {});", F, T);
        case 0x1C: return std::format("{} = rc::fclamp(s.acc + {} * {});", D, F, T);
        case 0x1D: return std::format("{} = rc::fclamp(s.acc - {} * {});", D, F, T);
        case 0x1E: return std::format("s.acc = rc::fclamp(s.acc + {} * {});", F, T);
        case 0x1F: return std::format("s.acc = rc::fclamp(s.acc - {} * {});", F, T);
        case 0x24: return std::format("{} = rc::ftoi({});", D, F);
        case 0x28: return std::format("{} = std::max({}, {});", D, F, T);
        case 0x29: return std::format("{} = std::min({}, {});", D, F, T);
        case 0x30: return "s.fcr31 &= ~(1u << 23);";
        case 0x32: return std::format("rc::fcond(s, {} == {});", F, T);
        case 0x34: return std::format("rc::fcond(s, {} < {});", F, T);
        case 0x36: return std::format("rc::fcond(s, {} <= {});", F, T);
      }
      return {};
    case 0x14:
      if ((op & 63) == 0x20) return std::format("{} = float(int32_t(std::bit_cast<uint32_t>({})));", D, F);
      return {};
  }
  return {};
}

// C++ for a non-control instruction; it may fall back to the interpreter.
std::string emitPlain(uint32_t op, uint32_t pc) {
  const uint32_t opc = op >> 26, rs = field(op, 21, 5), rt = field(op, 16, 5), rd = field(op, 11, 5), sa = field(op, 6, 5);
  const int32_t imm = int16_t(op & 0xFFFF);
  const uint32_t uimm = op & 0xFFFF;
  const std::string ea = std::format("(RC_W({}) + {})", rs, hex(uint32_t(imm)));
  const std::string fallback = std::format("rc::interp(c, {}, {});", hex(op), hex(pc));
  auto R = [](uint32_t i) { return std::format("RC_R({})", i); };
  auto W = [](uint32_t i) { return std::format("RC_W({})", i); };
  auto set = [&](uint32_t reg, const std::string& expr) {
    return reg == 0 ? std::string() : std::format("{} = {};", R(reg), expr);
  };
  switch (opc) {
    case 0x00:
      switch (op & 63) {
        case 0x00: return set(rd, std::format("rc::sx32({} << {})", W(rt), sa));
        case 0x02: return set(rd, std::format("rc::sx32({} >> {})", W(rt), sa));
        case 0x03: return set(rd, std::format("rc::sx32(uint32_t(int32_t({}) >> {}))", W(rt), sa));
        case 0x04: return set(rd, std::format("rc::sx32({} << ({} & 31))", W(rt), W(rs)));
        case 0x06: return set(rd, std::format("rc::sx32({} >> ({} & 31))", W(rt), W(rs)));
        case 0x07: return set(rd, std::format("rc::sx32(uint32_t(int32_t({}) >> ({} & 31)))", W(rt), W(rs)));
        case 0x0A: return rd ? std::format("if ({} == 0) {} = {};", R(rt), R(rd), R(rs)) : "";  // MOVZ
        case 0x0B: return rd ? std::format("if ({} != 0) {} = {};", R(rt), R(rd), R(rs)) : "";  // MOVN
        case 0x0F: return "";                                                                  // SYNC
        case 0x10: return set(rd, "s.hi.ud[0]");
        case 0x11: return std::format("s.hi.ud[0] = {};", R(rs));
        case 0x12: return set(rd, "s.lo.ud[0]");
        case 0x13: return std::format("s.lo.ud[0] = {};", R(rs));
        case 0x14: return set(rd, std::format("{} << ({} & 63)", R(rt), W(rs)));
        case 0x16: return set(rd, std::format("{} >> ({} & 63)", R(rt), W(rs)));
        case 0x17: return set(rd, std::format("uint64_t(int64_t({}) >> ({} & 63))", R(rt), W(rs)));
        case 0x20: case 0x21: return set(rd, std::format("rc::sx32({} + {})", W(rs), W(rt)));
        case 0x22: case 0x23: return set(rd, std::format("rc::sx32({} - {})", W(rs), W(rt)));
        case 0x24: return set(rd, std::format("{} & {}", R(rs), R(rt)));
        case 0x25: return set(rd, std::format("{} | {}", R(rs), R(rt)));
        case 0x26: return set(rd, std::format("{} ^ {}", R(rs), R(rt)));
        case 0x27: return set(rd, std::format("~({} | {})", R(rs), R(rt)));
        case 0x2A: return set(rd, std::format("uint64_t(int64_t({}) < int64_t({}))", R(rs), R(rt)));
        case 0x2B: return set(rd, std::format("uint64_t({} < {})", R(rs), R(rt)));
        case 0x2C: case 0x2D: return set(rd, std::format("{} + {}", R(rs), R(rt)));
        case 0x2E: case 0x2F: return set(rd, std::format("{} - {}", R(rs), R(rt)));
        case 0x38: return set(rd, std::format("{} << {}", R(rt), sa));
        case 0x3A: return set(rd, std::format("{} >> {}", R(rt), sa));
        case 0x3B: return set(rd, std::format("uint64_t(int64_t({}) >> {})", R(rt), sa));
        case 0x3C: return set(rd, std::format("{} << {}", R(rt), sa + 32));
        case 0x3E: return set(rd, std::format("{} >> {}", R(rt), sa + 32));
        case 0x3F: return set(rd, std::format("uint64_t(int64_t({}) >> {})", R(rt), sa + 32));
      }
      return fallback;  // MULT/DIV and friends, MFSA/MTSA, traps
    case 0x08: case 0x09: return set(rt, std::format("rc::sx32({} + {})", W(rs), hex(uint32_t(imm))));
    case 0x0A: return set(rt, std::format("uint64_t(int64_t({}) < {}ll)", R(rs), imm));
    case 0x0B: return set(rt, std::format("uint64_t({} < uint64_t({}ll))", R(rs), imm));
    case 0x0C: return set(rt, std::format("{} & {}", R(rs), uimm));
    case 0x0D: return set(rt, std::format("{} | {}", R(rs), uimm));
    case 0x0E: return set(rt, std::format("{} ^ {}", R(rs), uimm));
    case 0x0F: return set(rt, std::format("rc::sx32({})", hex(uimm << 16)));
    case 0x18: case 0x19: return set(rt, std::format("{} + uint64_t({}ll)", R(rs), imm));
    case 0x20: return set(rt, std::format("uint64_t(int64_t(m.read<int8_t>({})))", ea));
    case 0x21: return set(rt, std::format("uint64_t(int64_t(m.read<int16_t>({})))", ea));
    case 0x23: return set(rt, std::format("rc::sx32(m.read<uint32_t>({}))", ea));
    case 0x24: return set(rt, std::format("m.read<uint8_t>({})", ea));
    case 0x25: return set(rt, std::format("m.read<uint16_t>({})", ea));
    case 0x27: return set(rt, std::format("m.read<uint32_t>({})", ea));
    case 0x37: return set(rt, std::format("m.read<uint64_t>({})", ea));
    case 0x1E: return rt ? std::format("m.read128({} & ~15u, &s.gpr[{}]);", ea, rt) : "";
    case 0x28: return std::format("m.write<uint8_t>({}, uint8_t({}));", ea, R(rt));
    case 0x29: return std::format("m.write<uint16_t>({}, uint16_t({}));", ea, R(rt));
    case 0x2B: return std::format("m.write<uint32_t>({}, {});", ea, W(rt));
    case 0x3F: return std::format("m.write<uint64_t>({}, {});", ea, R(rt));
    case 0x1F: return std::format("m.write128({} & ~15u, &s.gpr[{}]);", ea, rt);
    case 0x31: return std::format("s.fpr[{}] = std::bit_cast<float>(m.read<uint32_t>({}));", rt, ea);
    case 0x39: return std::format("m.write<uint32_t>({}, std::bit_cast<uint32_t>(s.fpr[{}]));", ea, rt);
    case 0x2F: case 0x33: return "";  // CACHE, PREF
    case 0x1C:
      if (std::string code = emitMmi(op); !code.empty()) return code;
      return fallback;
    case 0x10:  // COP0
      if (rs == 0x00) return set(rt, rd == 9 ? std::string("rc::sx32(uint32_t(c.executed))") : std::format("rc::sx32(s.cop0[{}])", rd));
      if (rs == 0x04) return std::format("s.cop0[{}] = {};", rd, W(rt));
      return fallback;
    case 0x11: if (std::string code = emitFpu(op); !code.empty()) return code;
      return fallback;
    case 0x12:  // COP2
      if (rs == 0x01) return rt ? std::format("s.gpr[{}] = s.vf[{}];", rt, rd) : "";   // QMFC2
      if (rs == 0x02) return set(rt, std::format("rc::sx32(s.vi[{}])", rd));           // CFC2
      if (rs == 0x05) return rd ? std::format("s.vf[{}] = s.gpr[{}];", rd, rt) : "";   // QMTC2
      if (rs == 0x06) return rd ? std::format("s.vi[{}] = {};", rd, W(rt)) : "";       // CTC2
      if (rs >= 0x10) {
        u128 vf[32]{}, acc{};
        uint32_t vi[32]{};
        if (vuUpper(VuRegs{vf, vi, &acc}, op)) return std::format("rc::macroUpper(s, {});", hex(op));
      }
      return fallback;
    case 0x36:  // LQC2
      return std::format("m.read128({} & ~15u, &s.vf[{}]);{}", ea, rt, rt == 0 ? " s.vf[0] = {}; s.vf[0].f[3] = 1.0f;" : "");
    case 0x3E: return std::format("m.write128({} & ~15u, &s.vf[{}]);", ea, rt);  // SQC2
  }
  return fallback;  // unaligned loads/stores, MMI, REGIMM extras
}

class Emitter {
public:
  Emitter(Memory& mem, const Function& fn) : mem_(mem), fn_(fn) {}

  bool inBody(uint32_t pc) const {
    for (const Range& r : fn_.ranges)
      if (pc >= r.lo && pc <= r.hi) return true;
    return false;
  }
  uint32_t word(uint32_t pc) const {
    uint32_t v = 0;
    if (const uint8_t* p = mem_.hostPtr(pc, 4)) std::memcpy(&v, p, 4);
    return v;
  }
  // A branch is compiled together with its delay slot unless the slot can't be (or is itself
  // a jump target); then the interpreter runs the pair.
  bool compilable(uint32_t pc) const {
    if (!inBody(pc + 4) || labels_.count(pc + 4)) return false;
    const uint32_t slot = word(pc + 4);
    return !isBranchOrJump(slot) && !isTerminator(slot);
  }

  std::string run(std::vector<uint32_t>& entries) {
    collectLabels();
    std::string out;
    out += std::format("static uint32_t f_{:08X}(Cpu& c, uint32_t entry) {{\n", fn_.entry);
    out += "  CpuState& s = c.s;\n  Memory& m = c.mem;\n  uint64_t n = 0;\n  (void)s; (void)m;\n  switch (entry) {\n";
    for (uint32_t l : labels_) out += std::format("    case {}: goto L_{:08X};\n", hex(l), l);
    out += "    default: RC_EXIT(entry);\n  }\n";
    for (const Range& r : fn_.ranges) {
      for (uint32_t pc = r.lo; pc <= r.hi;) {
        if (labels_.count(pc)) out += std::format("L_{:08X}:\n", pc);
        const uint32_t op = word(pc);
        if (isTerminator(op) || (isBranchOrJump(op) && !compilable(pc))) {
          out += std::format("  RC_EXIT({});\n", hex(pc));
          pc += 4;
          continue;
        }
        if (isBranchOrJump(op)) {
          out += emitBranch(op, pc);
          pc += 8;
          continue;
        }
        const std::string code = emitPlain(op, pc);
        out += "  ++n;";
        if (!code.empty()) out += " " + code;
        out += "\n";
        pc += 4;
      }
      out += std::format("  RC_EXIT({});\n", hex(r.hi + 1));
    }
    out += "}\n\n";
    entries.assign(labels_.begin(), labels_.end());
    return out;
  }

private:
  // Every place execution may enter: the function start, branch targets inside the body, the
  // addresses calls return to, and the instruction after anything the interpreter executes.
  void collectLabels() {
    labels_.insert(fn_.entry);
    for (const Range& r : fn_.ranges) {
      for (uint32_t pc = r.lo; pc <= r.hi; pc += 4) {
        const uint32_t op = word(pc);
        if (isTerminator(op)) {
          if (inBody(pc + 4)) labels_.insert(pc + 4);
          continue;
        }
        if (!isBranchOrJump(op)) continue;
        uint32_t target;
        if (staticTarget(op, pc, target) && inBody(target)) labels_.insert(target);
        // pc + 8 is where a call returns to, and where execution resumes when the interpreter
        // ran a branch the recompiled code could not take.
        if (inBody(pc + 8)) labels_.insert(pc + 8);
      }
    }
  }

  static bool staticTarget(uint32_t op, uint32_t pc, uint32_t& target) {
    const uint32_t opc = op >> 26;
    if (opc == 0x02 || opc == 0x03) {
      target = (pc & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
      return true;
    }
    if (opc == 0x00) return false;  // JR/JALR
    target = pc + 4 + (uint32_t(int32_t(int16_t(op & 0xFFFF))) << 2);
    return true;
  }

  // A short backward loop that stores nothing is usually the game waiting for something
  // (a flag an interrupt sets); the CPU's idle detection lets emulated time jump ahead there.
  bool pollingLoop(uint32_t target, uint32_t pc) const {
    if (pc - target > 64) return false;
    for (uint32_t a = target; a <= pc + 4; a += 4) {
      const uint32_t op = word(a), opc = op >> 26;
      if ((opc >= 0x28 && opc <= 0x2F) || opc == 0x1F || opc == 0x39 || opc == 0x3D || opc == 0x3F) return false;
      if (opc == 0x03 || (opc == 0x00 && (op & 63) == 0x09) || (opc == 0x00 && (op & 63) == 0x0C)) return false;
    }
    return true;
  }

  std::string jumpTo(uint32_t target, uint32_t pc) const {
    if (!labels_.count(target)) return std::format("RC_EXIT({});", hex(target));
    if (target <= pc && pollingLoop(target, pc))
      return std::format("{{ if (rc::due(c, n) || rc::idle(c, n, {}, {})) RC_EXIT({}); goto L_{:08X}; }}", hex(pc),
                         pc + 8 - target, hex(target), target);
    if (target <= pc) return std::format("{{ if (rc::due(c, n)) RC_EXIT({}); goto L_{:08X}; }}", hex(target), target);
    return std::format("goto L_{:08X};", target);
  }

  std::string emitBranch(uint32_t op, uint32_t pc) const {
    const uint32_t opc = op >> 26, rs = field(op, 21, 5), rt = field(op, 16, 5), rd = field(op, 11, 5);
    const uint32_t slotOp = word(pc + 4);
    std::string slot = emitPlain(slotOp, pc + 4);
    slot = "++n; " + slot;
    std::string out;
    uint32_t target = 0;
    staticTarget(op, pc, target);
    auto R = [](uint32_t i) { return std::format("RC_R({})", i); };
    auto cond = [&](const std::string& c, bool likely, bool link) {
      std::string s = "  { ++n; const bool t_ = " + c + ";";
      if (link) s += std::format(" RC_R(31) = rc::sx32({});", hex(pc + 8));
      if (likely) {
        s += " if (t_) { " + slot + " " + jumpTo(target, pc) + " } }\n";
      } else {
        s += " " + slot + " if (t_) " + jumpTo(target, pc) + " }\n";
      }
      return s;
    };
    switch (opc) {
      case 0x02: return "  { ++n; " + slot + " " + jumpTo(target, pc) + " }\n";
      case 0x03:
        return std::format("  {{ ++n; RC_R(31) = rc::sx32({}); {} RC_EXIT({}); }}\n", hex(pc + 8), slot, hex(target));
      case 0x04: return cond(R(rs) + " == " + R(rt), false, false);
      case 0x05: return cond(R(rs) + " != " + R(rt), false, false);
      case 0x06: return cond("int64_t(" + R(rs) + ") <= 0", false, false);
      case 0x07: return cond("int64_t(" + R(rs) + ") > 0", false, false);
      case 0x14: return cond(R(rs) + " == " + R(rt), true, false);
      case 0x15: return cond(R(rs) + " != " + R(rt), true, false);
      case 0x16: return cond("int64_t(" + R(rs) + ") <= 0", true, false);
      case 0x17: return cond("int64_t(" + R(rs) + ") > 0", true, false);
      case 0x01: {
        const std::string lt = "int64_t(" + R(rs) + ") < 0", ge = "int64_t(" + R(rs) + ") >= 0";
        switch (rt) {
          case 0x00: return cond(lt, false, false);
          case 0x01: return cond(ge, false, false);
          case 0x02: return cond(lt, true, false);
          case 0x03: return cond(ge, true, false);
          case 0x10: return cond(lt, false, true);
          case 0x11: return cond(ge, false, true);
          case 0x12: return cond(lt, true, true);
          case 0x13: return cond(ge, true, true);
        }
        break;
      }
      case 0x11: {  // BC1F/BC1T/BC1FL/BC1TL
        const std::string c = std::format("((s.fcr31 >> 23) & 1) == {}", rt & 1);
        return cond(c, rt & 2, false);
      }
      case 0x00:
        if ((op & 63) == 0x08)  // JR
          return std::format("  {{ ++n; const uint32_t t_ = RC_W({}); {} RC_EXIT(t_); }}\n", rs, slot);
        // JALR: the target is read before the link register is written
        return std::format("  {{ ++n; const uint32_t t_ = RC_W({}); {} {} RC_EXIT(t_); }}\n", rs,
                           rd ? std::format("RC_R({}) = rc::sx32({});", rd, hex(pc + 8)) : "", slot);
    }
    return std::format("  RC_EXIT({});\n", hex(pc));
  }

  Memory& mem_;
  const Function& fn_;
  std::set<uint32_t> labels_;
};

bool readFunctions(const fs::path& path, std::vector<Function>& out, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot read " + path.string();
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string entry, range;
    if (!(ls >> entry)) continue;
    Function f{uint32_t(std::stoul(entry, nullptr, 16)), {}};
    while (ls >> range) {
      const size_t dash = range.find('-');
      f.ranges.push_back({uint32_t(std::stoul(range.substr(0, dash), nullptr, 16)),
                          uint32_t(std::stoul(range.substr(dash + 1), nullptr, 16))});
    }
    out.push_back(std::move(f));
  }
  return true;
}
}  // namespace

// Code the function list misses (functions only reached through pointer tables, which Ghidra
// does not find) would run on the interpreter. Every stretch of the text segment no listed
// function covers is cut at its returns (JR RA plus delay slot) into extra functions; zero
// words (padding) are skipped. A stretch that is really data only yields code nothing enters.
void addGapFunctions(Memory& mem, std::vector<Function>& functions, uint32_t lo, uint32_t hi) {
  std::vector<Range> covered;
  for (const Function& f : functions) covered.insert(covered.end(), f.ranges.begin(), f.ranges.end());
  std::sort(covered.begin(), covered.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
  auto word = [&](uint32_t pc) {
    uint32_t v = 0;
    if (const uint8_t* p = mem.hostPtr(pc, 4)) std::memcpy(&v, p, 4);
    return v;
  };
  size_t added = 0;
  auto fill = [&](uint32_t from, uint32_t to) {  // [from, to)
    for (uint32_t pc = from; pc < to;) {
      if (word(pc) == 0) {
        pc += 4;
        continue;
      }
      uint32_t end = pc;
      while (end + 4 < to && word(end) != 0x03E00008u) end += 4;
      end = std::min(end + 4, to - 4);  // the delay slot
      functions.push_back({pc, {{pc, end + 3}}});
      ++added;
      pc = end + 4;
    }
  };
  uint32_t pc = lo;
  for (const Range& r : covered) {
    if (r.lo > pc) fill(pc, std::min(r.lo, hi));
    pc = std::max(pc, (r.hi + 4) & ~3u);
    if (pc >= hi) break;
  }
  if (pc < hi) fill(pc, hi);
  std::printf("%zu extra functions in gaps of the function list\n", added);
}

bool generateRecompiledCode(Memory& mem, const fs::path& functionsFile, const fs::path& onlyList,
                            const fs::path& outDir, std::string& error) {
  std::vector<Function> functions;
  if (!readFunctions(functionsFile, functions, error)) return false;
  // Only code in the game's own text segment is translated (not the kernel-memory copies).
  constexpr uint32_t kTextLo = 0x00400000, kTextHi = 0x0092CAE8;
  std::erase_if(functions, [&](const Function& f) { return f.entry < kTextLo || f.entry >= kTextHi; });
  addGapFunctions(mem, functions, kTextLo, kTextHi);
  if (!onlyList.empty()) {
    std::unordered_set<uint32_t> keep;
    std::ifstream in(onlyList);
    std::string tok;
    while (in >> tok)
      if (tok.rfind("0x", 0) == 0 || tok.rfind("0X", 0) == 0) keep.insert(uint32_t(std::stoul(tok, nullptr, 16)));
    std::erase_if(functions, [&](const Function& f) { return !keep.count(f.entry); });
  }

  fs::create_directories(outDir);
  for (const auto& e : fs::directory_iterator(outDir))
    if (e.path().extension() == ".cpp") fs::remove(e.path());

  constexpr size_t kBytesPerFile = 600 * 1024;
  std::string body;
  std::vector<std::pair<uint32_t, uint32_t>> fileEntries;  // (entry pc, function entry)
  int fileIndex = 0;
  size_t totalEntries = 0;
  auto flush = [&] {
    if (body.empty()) return;
    std::string text = "// Generated by codport --recompile. Do not edit.\n#include <bit>\n#include \"recomp/runtime.h\"\n\n";
    text += "namespace {\n" + body + "}  // namespace\n\n";
    text += std::format("extern const rc::Entry rc_entries_{}[] = {{\n", fileIndex);
    for (auto [pc, fn] : fileEntries) text += std::format("  {{{}, f_{:08X}}},\n", hex(pc), fn);
    text += "};\n";
    text += std::format("extern const size_t rc_count_{} = {};\n", fileIndex, fileEntries.size());
    std::ofstream(outDir / std::format("gen_{:03}.cpp", fileIndex)) << text;
    totalEntries += fileEntries.size();
    body.clear();
    fileEntries.clear();
    ++fileIndex;
  };
  for (const Function& f : functions) {
    std::vector<uint32_t> entries;
    Emitter em(mem, f);
    body += em.run(entries);
    for (uint32_t pc : entries) fileEntries.emplace_back(pc, f.entry);
    if (body.size() >= kBytesPerFile) flush();
  }
  flush();

  std::string table = "// Generated by codport --recompile. Do not edit.\n#include <vector>\n#include \"recomp/runtime.h\"\n\n";
  for (int i = 0; i < fileIndex; ++i)
    table += std::format("extern const rc::Entry rc_entries_{}[];\nextern const size_t rc_count_{};\n", i, i);
  table += "\nvoid registerRecompiled(std::vector<rc::Entry>& out) {\n";
  for (int i = 0; i < fileIndex; ++i)
    table += std::format("  out.insert(out.end(), rc_entries_{0}, rc_entries_{0} + rc_count_{0});\n", i);
  table += "}\n";
  std::ofstream(outDir / "table.cpp") << table;
  std::printf("recompiled %zu functions (%zu entry points) into %d files in %s\n", functions.size(), totalEntries,
              fileIndex, outDir.string().c_str());
  return true;
}
