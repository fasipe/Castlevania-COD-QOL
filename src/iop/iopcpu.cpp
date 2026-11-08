#include "iop/iopcpu.h"

#include <cstring>
#include <format>

#include "iop/iopkernel.h"

namespace {
inline uint32_t sx16(uint32_t v) { return uint32_t(int32_t(int16_t(v))); }
}  // namespace

uint8_t* IopCpu::ptr(uint32_t addr, uint32_t size) {
  const uint32_t phys = addr & 0x1FFFFFFF;
  if (phys < 0x00800000) {  // RAM is mirrored four times
    const uint32_t a = phys & (kRamSize - 1);
    return a + size <= kRamSize ? ram_.data() + a : nullptr;
  }
  if (phys >= 0x1F800000 && phys + size <= 0x1F800400) return scratch_ + (phys - 0x1F800000);
  return nullptr;
}

template <class T>
T IopCpu::load(uint32_t addr) {
  if (const uint8_t* p = ptr(addr, sizeof(T))) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
  }
  if (fault.empty()) fault = std::format("IOP read{} at 0x{:08X} (pc 0x{:08X})", sizeof(T) * 8, addr, s.pc);
  return 0;
}

template <class T>
void IopCpu::store(uint32_t addr, T v) {
  if (uint8_t* p = ptr(addr, sizeof(T))) {
    std::memcpy(p, &v, sizeof(T));
    return;
  }
  if (fault.empty()) fault = std::format("IOP write{} at 0x{:08X} (pc 0x{:08X})", sizeof(T) * 8, addr, s.pc);
}

uint32_t IopCpu::read32(uint32_t addr) { return load<uint32_t>(addr); }
void IopCpu::write32(uint32_t addr, uint32_t v) { store<uint32_t>(addr, v); }

uint64_t IopCpu::run(uint64_t budget) {
  yield = false;
  uint64_t done = 0;
  while (done < budget && !yield && fault.empty()) {
    if (const uint32_t id = hleAt(s.pc); id && s.npc == s.pc + 4) {
      // Import stub: the kernel function runs and returns to the caller.
      s.pc = s.gpr[31];
      s.npc = s.pc + 4;
      kernel->call(id);
      ++done;
      continue;
    }
    step();
    ++done;
  }
  executed += done;
  return done;
}

void IopCpu::step() {
  const uint32_t pc = s.pc;
  const uint32_t op = load<uint32_t>(pc);
  s.pc = s.npc;
  s.npc += 4;
  uint32_t* r = s.gpr;
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  const uint32_t imm = sx16(op & 0xFFFF), uimm = op & 0xFFFF;
  const uint32_t ea = r[rs] + imm;
  auto branch = [&](bool cond) {
    if (cond) s.npc = pc + 4 + (imm << 2);
  };
  auto set = [&](uint32_t reg, uint32_t v) {
    if (reg) r[reg] = v;
  };
  switch (op >> 26) {
    case 0x00:
      switch (op & 63) {
        case 0x00: set(rd, r[rt] << sa); break;
        case 0x02: set(rd, r[rt] >> sa); break;
        case 0x03: set(rd, uint32_t(int32_t(r[rt]) >> sa)); break;
        case 0x04: set(rd, r[rt] << (r[rs] & 31)); break;
        case 0x06: set(rd, r[rt] >> (r[rs] & 31)); break;
        case 0x07: set(rd, uint32_t(int32_t(r[rt]) >> (r[rs] & 31))); break;
        case 0x08: s.npc = r[rs]; break;
        case 0x09: {
          const uint32_t target = r[rs];
          set(rd, pc + 8);
          s.npc = target;
          break;
        }
        case 0x0C: case 0x0D:
          if (fault.empty()) fault = std::format("IOP {} at 0x{:08X}", (op & 63) == 0x0C ? "syscall" : "break", pc);
          break;
        case 0x10: set(rd, s.hi); break;
        case 0x11: s.hi = r[rs]; break;
        case 0x12: set(rd, s.lo); break;
        case 0x13: s.lo = r[rs]; break;
        case 0x18: {
          const int64_t p = int64_t(int32_t(r[rs])) * int32_t(r[rt]);
          s.lo = uint32_t(p), s.hi = uint32_t(uint64_t(p) >> 32);
          break;
        }
        case 0x19: {
          const uint64_t p = uint64_t(r[rs]) * r[rt];
          s.lo = uint32_t(p), s.hi = uint32_t(p >> 32);
          break;
        }
        case 0x1A: {
          const int32_t n = int32_t(r[rs]), d = int32_t(r[rt]);
          if (d == 0) {
            s.lo = n < 0 ? 1 : 0xFFFFFFFF, s.hi = uint32_t(n);
          } else if (n == INT32_MIN && d == -1) {
            s.lo = uint32_t(n), s.hi = 0;
          } else {
            s.lo = uint32_t(n / d), s.hi = uint32_t(n % d);
          }
          break;
        }
        case 0x1B:
          if (r[rt] == 0) {
            s.lo = 0xFFFFFFFF, s.hi = r[rs];
          } else {
            s.lo = r[rs] / r[rt], s.hi = r[rs] % r[rt];
          }
          break;
        case 0x20: case 0x21: set(rd, r[rs] + r[rt]); break;
        case 0x22: case 0x23: set(rd, r[rs] - r[rt]); break;
        case 0x24: set(rd, r[rs] & r[rt]); break;
        case 0x25: set(rd, r[rs] | r[rt]); break;
        case 0x26: set(rd, r[rs] ^ r[rt]); break;
        case 0x27: set(rd, ~(r[rs] | r[rt])); break;
        case 0x2A: set(rd, int32_t(r[rs]) < int32_t(r[rt])); break;
        case 0x2B: set(rd, r[rs] < r[rt]); break;
        default:
          if (fault.empty()) fault = std::format("IOP SPECIAL 0x{:08X} at 0x{:08X}", op, pc);
      }
      break;
    case 0x01: {
      const bool lt = int32_t(r[rs]) < 0;
      const bool link = (rt & 0x1E) == 0x10;
      if (link) r[31] = pc + 8;
      branch((rt & 1) ? !lt : lt);
      break;
    }
    case 0x02: s.npc = (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2); break;
    case 0x03:
      r[31] = pc + 8;
      s.npc = (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2);
      break;
    case 0x04: branch(r[rs] == r[rt]); break;
    case 0x05: branch(r[rs] != r[rt]); break;
    case 0x06: branch(int32_t(r[rs]) <= 0); break;
    case 0x07: branch(int32_t(r[rs]) > 0); break;
    case 0x08: case 0x09: set(rt, r[rs] + imm); break;
    case 0x0A: set(rt, int32_t(r[rs]) < int32_t(imm)); break;
    case 0x0B: set(rt, r[rs] < imm); break;
    case 0x0C: set(rt, r[rs] & uimm); break;
    case 0x0D: set(rt, r[rs] | uimm); break;
    case 0x0E: set(rt, r[rs] ^ uimm); break;
    case 0x0F: set(rt, uimm << 16); break;
    case 0x10: {  // COP0: status register reads/writes and RFE are harmless here
      const uint32_t cop = rs;
      if (cop == 0) set(rt, 0);
      break;
    }
    case 0x20: set(rt, uint32_t(int32_t(int8_t(load<uint8_t>(ea))))); break;
    case 0x21: set(rt, sx16(load<uint16_t>(ea))); break;
    case 0x22: {  // LWL
      const uint32_t sh = (ea & 3) * 8, w = load<uint32_t>(ea & ~3u);
      set(rt, (r[rt] & (0x00FFFFFFu >> sh)) | (w << (24 - sh)));
      break;
    }
    case 0x23: set(rt, load<uint32_t>(ea)); break;
    case 0x24: set(rt, load<uint8_t>(ea)); break;
    case 0x25: set(rt, load<uint16_t>(ea)); break;
    case 0x26: {  // LWR
      const uint32_t sh = (ea & 3) * 8, w = load<uint32_t>(ea & ~3u);
      set(rt, (r[rt] & ~(0xFFFFFFFFu >> sh)) | (w >> sh));
      break;
    }
    case 0x28: store<uint8_t>(ea, uint8_t(r[rt])); break;
    case 0x29: store<uint16_t>(ea, uint16_t(r[rt])); break;
    case 0x2A: {  // SWL
      const uint32_t sh = (ea & 3) * 8, w = load<uint32_t>(ea & ~3u);
      store<uint32_t>(ea & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (r[rt] >> (24 - sh)));
      break;
    }
    case 0x2B: store<uint32_t>(ea, r[rt]); break;
    case 0x2E: {  // SWR
      const uint32_t sh = (ea & 3) * 8, w = load<uint32_t>(ea & ~3u);
      store<uint32_t>(ea & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (r[rt] << sh));
      break;
    }
    default:
      if (fault.empty()) fault = std::format("IOP opcode 0x{:08X} at 0x{:08X}", op, pc);
  }
}
