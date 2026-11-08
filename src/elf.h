#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Memory;

struct ElfSegment {
  uint32_t vaddr = 0, fileSize = 0, memSize = 0, flags = 0;
};

struct ElfImage {
  uint32_t entry = 0;
  std::vector<ElfSegment> segments;
};

// Loads the PT_LOAD segments of a 32-bit little-endian MIPS ELF into EE memory.
bool loadElf(const std::vector<uint8_t>& file, Memory& mem, ElfImage& out, std::string& error);
