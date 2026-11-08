#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "ee/hardware.h"

class Graphics;

// Emotion Engine address space: 32 MB RAM (with its uncached mirrors), 16 KB scratchpad,
// VU0/VU1 memory, and memory-mapped hardware registers. Registers without behaviour yet just
// keep the last value written; every access is recorded so each milestone can see what the
// game touches.
class Memory {
public:
  static constexpr uint32_t kRamSize = 32 * 1024 * 1024;
  static constexpr uint32_t kSprSize = 16 * 1024;
  static constexpr uint32_t kVuSize = 64 * 1024;  // 0x11000000-0x1100FFFF

  Memory();

  // Host pointer for RAM, scratchpad or VU memory; nullptr for MMIO or unmapped addresses.
  [[msvc::forceinline]] uint8_t* hostPtr(uint32_t addr, uint32_t size) {
    // Main RAM through any segment (0x2/0x3: uncached and accelerated mirrors of the first 32 MB).
    const uint32_t phys = addr & ((addr >> 29) == 1 ? 0x01FFFFFFu : 0x1FFFFFFFu);
    if (phys + uint64_t(size) <= kRamSize) [[likely]] return ram_.data() + phys;
    return hostPtrSlow(addr, size);
  }

  template <class T>
  T read(uint32_t addr) {
    if (uint8_t* p = hostPtr(addr, sizeof(T))) {
      T v;
      std::memcpy(&v, p, sizeof(T));
      return v;
    }
    return static_cast<T>(ioRead(addr, sizeof(T)));
  }

  template <class T>
  void write(uint32_t addr, T v) {
    ++writes;
    if (uint8_t* p = hostPtr(addr, sizeof(T))) {
      std::memcpy(p, &v, sizeof(T));
      return;
    }
    ioWrite(addr, static_cast<uint64_t>(v), sizeof(T));
  }

  void read128(uint32_t addr, void* dst);
  void write128(uint32_t addr, const void* src);

  struct IoStat {
    uint64_t reads = 0, writes = 0, value = 0;
  };
  std::unordered_map<uint32_t, IoStat> io;  // every MMIO register the game touched
  Hardware hw;                              // registers with real behaviour (timers, INTC, GS CSR)
  Graphics* gfx = nullptr;                  // DMAC, VIF, GIF and GS registers

  uint64_t writes = 0;           // stores so far (idle-loop detection)
  uint64_t vuCodeTouches = 0;    // accesses to VU1 micro memory (decoded micro programs go stale)
  std::string fault;             // set on access to unmapped memory; the CPU stops on it
  uint32_t pollAddr = 0;         // register read repeatedly with no other MMIO in between
  uint64_t pollStreak = 0;

private:
  uint8_t* hostPtrSlow(uint32_t addr, uint32_t size);
  uint64_t ioRead(uint32_t addr, unsigned size);
  void ioWrite(uint32_t addr, uint64_t value, unsigned size);

  std::vector<uint8_t> ram_, spr_, vu_;
};
