#pragma once
#include <cstdint>
#include <functional>
#include <vector>

// EE hardware that drives time: the four EE timers, the interrupt controller (INTC) and the
// video timing (VBlank, GS CSR): PAL at boot, NTSC once the game picks 60 Hz (SetGsCrt). Time is
// counted in EE cycles; the interpreter advances it by one cycle per instruction and the kernel
// fast-forwards it when every thread is waiting.
class Hardware {
public:
  static constexpr uint64_t kEeHz = 294'912'000;
  static constexpr uint64_t kNever = ~0ull;
  // Field and line timing of the current video mode.
  uint64_t frameCycles = kEeHz / 50, lineCycles = kEeHz / 15625;  // PAL: 50 fields, 15625 lines per second
  void setNtsc(bool ntsc);
  bool ntsc() const { return ntsc_; }

  enum Cause : int { kGs = 0, kDmac = 1, kVblankStart = 2, kVblankEnd = 3, kTimer0 = 9 };

  // MMIO handlers; return false for registers this class does not own.
  bool read(uint32_t phys, uint64_t& value);
  bool write(uint32_t phys, uint64_t value);

  void update();                   // advance timers and video timing up to `cycles`
  uint64_t nextEvent() const;      // cycle of the next unmasked interrupt source, or kNever
  uint32_t pending() const { return intcStat & intcMask; }

  uint64_t cycles = 0;
  uint64_t frames = 0;
  uint32_t intcStat = 0, intcMask = 0;
  uint32_t dmacStat = 0, dmacMask = 0;  // D_STAT channel-done bits and their interrupt mask
  uint64_t gsImr = 0x7F00;               // GS interrupt mask (1 = masked), set via GsPutIMR
  void setGsImr(uint64_t imr) {
    if (beforeGsImr) beforeGsImr();
    gsImr = imr;
  }
  // Called before the IMR changes: GS events still being worked out on the graphics thread arrive
  // first, under the old mask (Graphics::flushEvents).
  std::function<void()> beforeGsImr;
  // Other units that will need the EE at some cycle (the IOP, the IPU finishing a command).
  std::vector<std::function<uint64_t()>> eventSources;
  std::function<void()> onVblank;  // called at each VBlank start (presenting the frame)

  // A GS event (CSR bit: SIGNAL=1, FINISH=2, HSINT=4, VSINT=8); raises INTC cause 0 if unmasked.
  void gsEvent(uint64_t bit) {
    gsCsr_ |= bit;
    if (!(gsImr & (bit << 8))) raise(kGs);
  }

private:
  struct Timer {
    uint32_t count = 0, mode = 0, comp = 0, hold = 0;
    uint64_t last = 0;  // cycle `count` was last brought up to date
  };
  uint64_t cyclesPerTick(const Timer& t) const;
  uint64_t ticksToEvent(const Timer& t) const;
  void updateTimer(int n);
  void raise(int cause) { intcStat |= 1u << cause; }

  Timer timers_[4];
  bool ntsc_ = false;
  uint64_t nextVblankStart_ = kEeHz / 50;
  uint64_t nextVblankEnd_ = kEeHz / 50 + kEeHz / 15625 * 25;
  uint64_t gsCsr_ = 0;
};
