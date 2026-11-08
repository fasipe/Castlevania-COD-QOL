#pragma once
#include <cstdint>
#include <functional>
#include <string>

class Memory;
union u128;

// Receives the qwords a DMA channel moves from memory to a device (VIF0, VIF1, GIF, ...).
class DmaSink {
public:
  virtual ~DmaSink() = default;
  virtual void dmaWrite(const u128* data, uint32_t qwc) = 0;
  virtual void dmaTag(uint64_t) {}  // upper half of a chain tag, sent when CHCR.TTE is set
  virtual void dmaDone() {}         // the transfer has finished
};

// A device that moves data at its own pace (the IPU): its channels stay active until it has
// pulled (to the device) or pushed (from the device) everything the game set up.
class DmaDevice {
public:
  virtual ~DmaDevice() = default;
  virtual void dmaStarted(int channel) = 0;
};

// The EE DMA controller. A transfer runs to completion as soon as the game starts it (CHCR.STR);
// the channel then reports done in D_STAT, which raises the DMAC interrupt if enabled. Channels
// with a DmaDevice instead finish when the device has moved all the data.
class Dmac {
public:
  enum Channel { kVif0 = 0, kVif1 = 1, kGif = 2, kIpuFrom = 3, kIpuTo = 4, kSif0 = 5, kSif1 = 6,
                 kSif2 = 7, kSprFrom = 8, kSprTo = 9 };

  explicit Dmac(Memory& mem) : mem_(mem) {}

  bool read(uint32_t phys, uint64_t& value);
  bool write(uint32_t phys, uint64_t value);

  DmaSink* sinks[10] = {};
  std::function<void(uint32_t)> vif1TagAt;  // told where each chain tag of VIF1's channel is, before it goes on
  DmaDevice* devices[10] = {};
  bool active(int ch) const { return (ch_[ch].chcr & (1u << 8)) != 0; }
  // For device channels: up to `maxQw` qwords from memory (normal or source chain mode), and
  // `qw` qwords into memory at MADR (normal mode). Both return the qwords moved.
  uint32_t pull(int ch, void* dst, uint32_t maxQw);
  uint32_t push(int ch, const void* src, uint32_t qw);
  uint64_t transfers[10] = {};  // completed transfers per channel
  uint64_t qwords[10] = {};     // qwords moved per channel
  std::string fault;            // first unsupported request, for the run report

private:
  struct Chan {
    uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, asr[2] = {}, sadr = 0;
    bool lastTag = false;  // device chain: the current block is the last one
    uint32_t stack[2] = {};
    int sp = 0;
  };
  void finish(int ch);
  bool endsAfterCurrent(const Chan& c) const;
  bool nextTag(int ch);
  void start(int ch);
  void sendBlock(int ch, uint32_t addr, uint32_t qwc);
  void sprTransfer(int ch, Chan& c);
  void sprBlock(int ch, Chan& c, uint32_t qwc);
  int channelOf(uint32_t phys, uint32_t& reg) const;

  Memory& mem_;
  Chan ch_[10];
  uint32_t ctrl_ = 0, pcr_ = 0, sqwc_ = 0, rbsr_ = 0, rbor_ = 0, stadr_ = 0;
};
