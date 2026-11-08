#include "ee/hardware.h"

#include <algorithm>

namespace {
// Timer MODE bits
constexpr uint32_t kCue = 1u << 7, kCmpe = 1u << 8, kOvfe = 1u << 9, kEquf = 1u << 10, kOvff = 1u << 11;
constexpr uint32_t kZret = 1u << 6;

// GS CSR bits
constexpr uint64_t kCsrVsint = 1u << 3, kCsrField = 1u << 13;
constexpr uint64_t kCsrIdRev = 0x551Bull << 16;

constexpr uint32_t kIntcStat = 0x1000F000, kIntcMask = 0x1000F010, kGsCsr = 0x12001000, kDmacStat = 0x1000E010;
constexpr uint32_t kGsImr = 0x12001010;

int timerIndex(uint32_t phys) {
  if (phys < 0x10000000 || phys >= 0x10002000) return -1;
  return int((phys - 0x10000000) >> 11);
}
}  // namespace

uint64_t Hardware::cyclesPerTick(const Timer& t) const {
  switch (t.mode & 3) {
    case 0: return 2;    // bus clock = EE clock / 2
    case 1: return 32;   // bus / 16
    case 2: return 512;  // bus / 256
    default: return lineCycles;  // HBlank
  }
}

uint64_t Hardware::ticksToEvent(const Timer& t) const {
  uint64_t best = kNever;
  if (t.mode & kCmpe) best = t.comp > t.count ? t.comp - t.count : 0x10000 - t.count + t.comp;
  if (t.mode & kOvfe) best = std::min<uint64_t>(best, 0x10000 - t.count);
  return best;
}

void Hardware::updateTimer(int n) {
  Timer& t = timers_[n];
  if (!(t.mode & kCue)) {
    t.last = cycles;
    return;
  }
  const uint64_t per = cyclesPerTick(t);
  uint64_t ticks = (cycles - t.last) / per;
  t.last += ticks * per;
  while (ticks > 0) {
    const uint64_t toCmp = t.comp > t.count ? t.comp - t.count : 0x10000 - t.count + t.comp;
    const uint64_t toOvf = 0x10000 - t.count;
    const uint64_t step = std::min({ticks, toCmp, toOvf});
    t.count += uint32_t(step);
    ticks -= step;
    if (step == toCmp) {  // an interrupt fires only when the flag goes from clear to set
      if ((t.mode & kCmpe) && !(t.mode & kEquf)) raise(kTimer0 + n);
      t.mode |= kEquf;
      if (t.mode & kZret) t.count = 0;
    }
    if (t.count >= 0x10000) {
      if ((t.mode & kOvfe) && !(t.mode & kOvff)) raise(kTimer0 + n);
      t.mode |= kOvff;
      t.count &= 0xFFFF;
    }
  }
}

// NTSC: 59.94 fields and 15734 lines per second, 22 lines of vertical blank.
void Hardware::setNtsc(bool ntsc) {
  if (ntsc == ntsc_) return;
  ntsc_ = ntsc;
  frameCycles = ntsc ? kEeHz * 1001 / 60000 : kEeHz / 50;
  lineCycles = ntsc ? kEeHz * 1001 / 15750000 : kEeHz / 15625;
  nextVblankStart_ = cycles + frameCycles;
  nextVblankEnd_ = nextVblankStart_ + lineCycles * (ntsc ? 22 : 25);
}

void Hardware::update() {
  for (int n = 0; n < 4; ++n) updateTimer(n);
  while (cycles >= nextVblankStart_ || cycles >= nextVblankEnd_) {
    if (nextVblankStart_ <= nextVblankEnd_) {
      raise(kVblankStart);
      gsCsr_ ^= kCsrField;
      gsEvent(kCsrVsint);
      ++frames;
      nextVblankStart_ += frameCycles;
      if (onVblank) onVblank();
    } else {
      raise(kVblankEnd);
      nextVblankEnd_ += frameCycles;
    }
  }
}

uint64_t Hardware::nextEvent() const {
  uint64_t next = kNever;
  if (intcMask & (1u << kVblankStart)) next = std::min(next, nextVblankStart_);
  if (intcMask & (1u << kVblankEnd)) next = std::min(next, nextVblankEnd_);
  for (const auto& source : eventSources) next = std::min(next, source());
  for (int n = 0; n < 4; ++n) {
    const Timer& t = timers_[n];
    if (!(t.mode & kCue) || !(intcMask & (1u << (kTimer0 + n)))) continue;
    const uint64_t ticks = ticksToEvent(t);
    if (ticks != kNever) next = std::min(next, t.last + ticks * cyclesPerTick(t));
  }
  return next;
}

bool Hardware::read(uint32_t phys, uint64_t& value) {
  if (phys == kIntcStat) {
    update();
    value = intcStat;
    return true;
  }
  if (phys == kIntcMask) {
    value = intcMask;
    return true;
  }
  if (phys == kGsCsr) {
    update();
    value = gsCsr_ | kCsrIdRev;
    return true;
  }
  if (phys == kGsImr) {
    value = gsImr;
    return true;
  }
  if (phys == kDmacStat) {
    value = dmacStat | (dmacMask << 16);
    return true;
  }
  const int n = timerIndex(phys);
  if (n < 0) return false;
  updateTimer(n);
  const Timer& t = timers_[n];
  switch (phys & 0x7FF) {
    case 0x00: value = t.count; return true;
    case 0x10: value = t.mode; return true;
    case 0x20: value = t.comp; return true;
    case 0x30: value = t.hold; return true;
  }
  return false;
}

bool Hardware::write(uint32_t phys, uint64_t value) {
  const uint32_t v = uint32_t(value);
  if (phys == kIntcStat) {  // write 1 to acknowledge
    intcStat &= ~v;
    return true;
  }
  if (phys == kIntcMask) {  // write 1 to toggle
    intcMask ^= v & 0x7FFF;
    return true;
  }
  if (phys == kGsCsr) {  // SIGNAL/FINISH/HSINT/VSINT/EDWINT: write 1 to clear
    gsCsr_ &= ~(value & 0x1F);
    return true;
  }
  if (phys == kGsImr) {
    setGsImr(value);
    return true;
  }
  if (phys == kDmacStat) {  // low half: write 1 to clear; high half: write 1 to toggle the mask
    dmacStat &= ~(v & 0xFFFF);
    dmacMask ^= (v >> 16) & 0xFFFF;
    return true;
  }
  const int n = timerIndex(phys);
  if (n < 0) return false;
  updateTimer(n);
  Timer& t = timers_[n];
  switch (phys & 0x7FF) {
    case 0x00: t.count = v & 0xFFFF; t.last = cycles; return true;
    case 0x10: t.mode = (v & 0x3FF) | (t.mode & (kEquf | kOvff) & ~v); t.last = cycles; return true;
    case 0x20: t.comp = v & 0xFFFF; return true;
    case 0x30: t.hold = v & 0xFFFF; return true;
  }
  return false;
}
