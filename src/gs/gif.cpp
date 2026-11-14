#include "gs/gif.h"

#include <algorithm>
#include <cstring>

#include "ee/cpu.h"
#include "gs/gs.h"

template <bool ToGs>
void Gif::step(PathState& p, const uint64_t q[2], Gs* gs, uint64_t& packets) {
  if (!p.active) {  // a new GIF tag
    p.nloop = uint32_t(q[0] & 0x7FFF);
    p.eop = (q[0] >> 15) & 1;
    p.flg = uint32_t(q[0] >> 58) & 3;
    p.nreg = uint32_t(q[0] >> 60) & 0xF;
    if (p.nreg == 0) p.nreg = 16;
    p.regs = q[1];
    p.reg = 0;
    ++packets;
    if constexpr (ToGs)
      if ((q[0] >> 46) & 1) gs->writeReg(0x00, (q[0] >> 47) & 0x7FF);  // PRE: set PRIM first
    p.active = p.nloop > 0;
    return;
  }
  switch (p.flg) {
    case 0: {  // PACKED: one register per qword
      if constexpr (ToGs) gs->writePacked(uint32_t(p.regs >> (p.reg * 4)) & 0xF, q);
      if (++p.reg == p.nreg) {
        p.reg = 0;
        if (--p.nloop == 0) p.active = false;
      }
      break;
    }
    case 1:  // REGLIST: two registers per qword (64-bit data each)
      for (int half = 0; half < 2 && p.active; ++half) {
        const uint32_t reg = uint32_t(p.regs >> (p.reg * 4)) & 0xF;
        if constexpr (ToGs)
          if (reg != 0xE && reg != 0xF) gs->writeReg(reg, q[half]);
        if (++p.reg == p.nreg) {
          p.reg = 0;
          if (--p.nloop == 0) p.active = false;  // an odd final register leaves padding
        }
      }
      break;
    default:  // IMAGE (and the reserved mode): NLOOP qwords of transfer data
      if constexpr (ToGs) gs->writeImage(q, 2);
      if (--p.nloop == 0) p.active = false;
      break;
  }
}

void Gif::feed(int path, const u128* data, uint32_t qwc) {
  PathState& p = path_[path];
  for (uint32_t i = 0; i < qwc; ++i) {
    uint64_t q[2];
    std::memcpy(q, &data[i], 16);
    step<true>(p, q, &gs_, packets);
  }
}

void Gif::kick(const uint8_t* vuData, uint32_t qwords, uint32_t addr) {
  PathState& p = path_[1];
  p.active = false;
  for (uint32_t n = 0; n < qwords; ++n) {
    const bool startsTag = !p.active;
    feed(1, reinterpret_cast<const u128*>(vuData + ((addr + n) % qwords) * 16), 1);
    if (!p.active && p.eop && (!startsTag || p.nloop == 0)) return;
  }
}

// Tag to tag: each tag says how many data qwords follow it (what step() would consume).
uint32_t Gif::kickLength(const uint8_t* vuData, uint32_t qwords, uint32_t addr) {
  uint64_t n = 0;
  while (n < qwords) {
    uint64_t tag;
    std::memcpy(&tag, vuData + ((addr + n) % qwords) * 16, 8);
    const uint64_t nloop = tag & 0x7FFF, flg = (tag >> 58) & 3, nreg = ((tag >> 60) & 0xF) ? (tag >> 60) & 0xF : 16;
    n += 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
    if ((tag >> 15) & 1) return uint32_t(std::min<uint64_t>(n, qwords));  // EOP
  }
  return qwords;
}
