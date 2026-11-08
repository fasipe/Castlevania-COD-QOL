#pragma once

#include <cstddef>
#include <cstdint>

#include "ee/cpu.h"

// The table entry of the recompiled function at a guest address: replace it to hook the function
// (keep the old value to call it). Empty if the address has no recompiled code.
inline Cpu::RecompFn& recompSlot(Cpu& cpu, uint32_t pc) {
  static Cpu::RecompFn none = nullptr;
  const size_t index = (pc - Cpu::kRecompBase) >> 2;
  return index < cpu.recomp.size() ? cpu.recomp[index] : none;
}
