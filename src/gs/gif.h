#pragma once
#include <cstdint>

#include "gs/dmac.h"

class Gs;

// GIF: unpacks GIF packets (a tag, then PACKED, REGLIST or IMAGE data) into GS register
// writes. Each path (1 = VU1 XGKICK, 2 = VIF1 DIRECT, 3 = GIF DMA) keeps its own position, so a
// packet may arrive split across several transfers.
class Gif : public DmaSink {
public:
  explicit Gif(Gs& gs) : gs_(gs) {}

  void dmaWrite(const u128* data, uint32_t qwc) override { feed(3, data, qwc); }
  void feed(int path, const u128* data, uint32_t qwc);
  // XGKICK: sends packets from VU1 data memory starting at qword `addr` until one ends with EOP.
  void kick(const uint8_t* vuData, uint32_t qwords, uint32_t addr);
  // The qwords kick() would take from there.
  static uint32_t kickLength(const uint8_t* vuData, uint32_t qwords, uint32_t addr);

  uint64_t packets = 0;

private:
  struct PathState {
    uint32_t nloop = 0, nreg = 0, reg = 0, flg = 0;
    uint64_t regs = 0;
    bool eop = false, active = false;
  };
  // One qword of a path: a tag or data. Without a GS (kickLength) only the position moves.
  template <bool ToGs>
  static void step(PathState& p, const uint64_t q[2], Gs* gs, uint64_t& packets);
  Gs& gs_;
  PathState path_[4];
};
