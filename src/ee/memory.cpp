#include "ee/memory.h"

#include <format>

#include "gs/graphics.h"

namespace {
bool isMmio(uint32_t phys) {
  return (phys >= 0x10000000 && phys < 0x10010000) ||  // EE registers (DMAC, INTC, timers, VIF, GIF, SIF)
         (phys >= 0x12000000 && phys < 0x12002000);    // GS privileged registers
}

}  // namespace

Memory::Memory() : ram_(kRamSize), spr_(kSprSize), vu_(kVuSize) {}

uint8_t* Memory::hostPtrSlow(uint32_t addr, uint32_t size) {
  if ((addr & 0xFFFFC000u) == 0x70000000u) return spr_.data() + (addr & (kSprSize - 1));
  const uint32_t seg = addr >> 28;
  const uint32_t phys = (seg == 2 || seg == 3) ? (addr & 0x01FFFFFFu) : (addr & 0x1FFFFFFFu);
  if (phys + uint64_t(size) <= kRamSize) return ram_.data() + phys;
  if (phys >= 0x11000000 && phys + uint64_t(size) <= 0x11000000 + kVuSize) {
    if (phys >= 0x11008000 && gfx) gfx->drain();  // VU1 memory belongs to the graphics thread
    if ((phys & 0xC000) == 0x8000) ++vuCodeTouches;  // VU1 micro memory (0x11008000)
    return vu_.data() + (phys - 0x11000000);
  }
  return nullptr;
}

void Memory::read128(uint32_t addr, void* dst) {
  if (uint8_t* p = hostPtr(addr, 16)) {
    std::memcpy(dst, p, 16);
    return;
  }
  if (gfx && gfx->read128(addr & 0x1FFFFFFF, dst)) return;
  uint64_t v[2] = {ioRead(addr, 8), ioRead(addr + 8, 8)};
  std::memcpy(dst, v, 16);
}

void Memory::write128(uint32_t addr, const void* src) {
  ++writes;
  if (uint8_t* p = hostPtr(addr, 16)) {
    std::memcpy(p, src, 16);
    return;
  }
  if (gfx && gfx->write128(addr & 0x1FFFFFFF, src)) return;
  uint64_t v[2];
  std::memcpy(v, src, 16);
  ioWrite(addr, v[0], 8);
  ioWrite(addr + 8, v[1], 8);
}

uint64_t Memory::ioRead(uint32_t addr, unsigned size) {
  const uint32_t phys = addr & 0x1FFFFFFF;
  if (!isMmio(phys)) {
    if (fault.empty()) fault = std::format("read{} from unmapped 0x{:08X}", size * 8, addr);
    return 0;
  }
  IoStat& st = io[phys];
  ++st.reads;
  uint64_t value;
  if (hw.read(phys, value) || (gfx && gfx->read(phys, value))) {  // live registers change over time, so reading them in a loop is fine
    st.value = value;
  } else {
    if (phys == pollAddr) {
      ++pollStreak;
    } else {
      pollAddr = phys;
      pollStreak = 1;
    }
    value = st.value;
  }
  return size == 8 ? value : value & ((1ull << (size * 8)) - 1);
}

void Memory::ioWrite(uint32_t addr, uint64_t value, unsigned size) {
  const uint32_t phys = addr & 0x1FFFFFFF;
  if (!isMmio(phys)) {
    if (fault.empty()) fault = std::format("write{} to unmapped 0x{:08X}", size * 8, addr);
    return;
  }
  pollAddr = 0;
  pollStreak = 0;
  IoStat& st = io[phys];
  ++st.writes;
  st.value = value;
  if (!hw.write(phys, value) && gfx) gfx->write(phys, value);
}
