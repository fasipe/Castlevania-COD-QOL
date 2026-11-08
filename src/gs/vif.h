#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gs/dmac.h"

class VuMicro;

// VIF0/VIF1: decode the VIF command stream that DMA feeds them. They unpack data into VU data
// memory, upload micro programs, start the VU (MSCAL) and, for VIF1, pass GIF packets to PATH2
// (DIRECT). Commands may be split across transfers, so incoming words are queued until a whole
// command is available.
class Vif : public DmaSink {
public:
  Vif(int unit, uint8_t* vuCode, uint32_t codeBytes, uint8_t* vuData, uint32_t dataBytes, VuMicro& vu)
      : unit_(unit), code_(vuCode), codeBytes_(codeBytes), data_(vuData), dataBytes_(dataBytes), vu_(vu) {}

  void dmaWrite(const u128* data, uint32_t qwc) override;
  void dmaTag(uint64_t upper) override;
  void fifoWrite(const void* qword) { dmaWrite(static_cast<const u128*>(qword), 1); }

  bool read(uint32_t phys, uint64_t& value);  // VIFn registers
  bool write(uint32_t phys, uint64_t value);

  uint32_t top() const { return top_; }
  uint32_t itop() const { return itop_; }

  std::function<void(const u128* data, uint32_t qwc)> direct;  // VIF1: GIF PATH2 packets (DIRECT)
  std::string fault;
  uint64_t commands = 0, unpackedQwords = 0, microCalls = 0;

private:
  void process();
  bool execute(uint32_t code, size_t& pos);  // false if the command's data is not all here yet
  void unpack(uint32_t code, const uint32_t* words);
  void callMicro(uint32_t addr);

  int unit_;
  uint8_t* code_;
  uint32_t codeBytes_;
  uint8_t* data_;
  uint32_t dataBytes_;
  VuMicro& vu_;
  std::vector<uint32_t> fifo_;

  uint32_t cl_ = 1, wl_ = 1, mode_ = 0, mask_ = 0, row_[4] = {}, col_[4] = {};
  uint32_t base_ = 0, ofst_ = 0, tops_ = 0, top_ = 0, itops_ = 0, itop_ = 0, dbf_ = 0, mark_ = 0;
  uint32_t lastCode_ = 0, err_ = 0, stat_ = 0;
};
