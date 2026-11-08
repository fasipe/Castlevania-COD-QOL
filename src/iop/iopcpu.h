#pragma once
#include <cstdint>
#include <string>
#include <vector>

// IOP processor (MIPS R3000A) interpreter. It runs the game's own IOP modules (the Konami sound
// driver and its libraries); everything those modules import from the IOP kernel is provided in
// C++ (see IopKernel), reached when execution arrives at an import stub.
struct IopState {
  uint32_t gpr[32] = {};
  uint32_t hi = 0, lo = 0;
  uint32_t pc = 0, npc = 4;
};

class IopCpu {
public:
  static constexpr uint32_t kRamSize = 2 * 1024 * 1024;

  explicit IopCpu(std::vector<uint8_t>& ram) : ram_(ram), hleIndex_(kRamSize / 4) {}

  // Runs until `budget` instructions have executed or the current thread blocks/switches
  // (the kernel sets `yield`). Returns the number of instructions executed.
  uint64_t run(uint64_t budget);

  uint8_t* ptr(uint32_t addr, uint32_t size);  // host pointer into IOP memory, nullptr if unmapped
  uint32_t read32(uint32_t addr);
  void write32(uint32_t addr, uint32_t v);

  // An import stub: arriving at `addr` calls handler `id` (then returns to $ra).
  void setHle(uint32_t addr, uint32_t id) { hleIndex_[(addr & (kRamSize - 1)) >> 2] = id; }
  uint32_t hleAt(uint32_t addr) const { return (addr & 0x1FFFFFFF) < kRamSize * 4 ? hleIndex_[(addr & (kRamSize - 1)) >> 2] : 0; }

  IopState s;
  bool yield = false;
  std::string fault;
  uint64_t executed = 0;
  class IopKernel* kernel = nullptr;

private:
  void step();
  template <class T>
  T load(uint32_t addr);
  template <class T>
  void store(uint32_t addr, T v);

  std::vector<uint8_t>& ram_;
  std::vector<uint32_t> hleIndex_;
  uint8_t scratch_[1024] = {};
};
