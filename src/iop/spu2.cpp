#include "iop/spu2.h"

#include <algorithm>
#include <cstring>

namespace {
// libsd register numbers (entry >> 8)
enum : uint32_t {
  VP_VOLL = 0x00, VP_VOLR = 0x01, VP_PITCH = 0x02, VP_ADSR1 = 0x03, VP_ADSR2 = 0x04, VP_ENVX = 0x05,
  VP_VOLXL = 0x06, VP_VOLXR = 0x07,
  S_PMON = 0x13, S_NON = 0x14, S_KON = 0x15, S_KOFF = 0x16, S_ENDX = 0x17,
  VA_SSA = 0x20, VA_LSAX = 0x21, VA_NAX = 0x22,
};
constexpr uint32_t kBlockBytes = 16;

}  // namespace

Spu2::Spu2() : ram(kRamSize) {}

// sceSdInit's defaults: every voice goes to the dry and effect mixes, core 0's output into core 1
// at full volume.
void Spu2::init() {
  for (Core& c : core_) {
    c = Core{};
    for (uint32_t reg : {0x18u, 0x19u, 0x1Au, 0x1Bu}) c.switches[reg & 15] = 0xFFFFFF;  // VMIXL, VMIXEL, VMIXR, VMIXER
    c.params[0x0D] = c.params[0x0E] = 0x7FFF;                                           // AVOLL, AVOLR
  }
}

void Spu2::setParam(uint32_t entry, uint32_t value) {
  Core& c = core_[entry & 1];
  const uint32_t reg = (entry >> 8) & 0x3F;
  if (reg <= VP_VOLXR) {
    Voice& v = c.voice[((entry >> 1) & 31) % 24];
    switch (reg) {
      case VP_VOLL: v.volL = uint16_t(value); break;
      case VP_VOLR: v.volR = uint16_t(value); break;
      case VP_PITCH: v.pitch = uint16_t(value); break;
      case VP_ADSR1: v.adsr1 = uint16_t(value); break;
      case VP_ADSR2: v.adsr2 = uint16_t(value); break;
      case VP_ENVX: v.envx = value & 0x7FFF; break;
    }
    return;
  }
  c.params[reg & 31] = value;
}

uint32_t Spu2::getParam(uint32_t entry) const {
  const Core& c = core_[entry & 1];
  const uint32_t reg = (entry >> 8) & 0x3F;
  if (reg <= VP_VOLXR) {
    const Voice& v = c.voice[((entry >> 1) & 31) % 24];
    switch (reg) {
      case VP_VOLL: case VP_VOLXL: return v.volL;
      case VP_VOLR: case VP_VOLXR: return v.volR;
      case VP_PITCH: return v.pitch;
      case VP_ADSR1: return v.adsr1;
      case VP_ADSR2: return v.adsr2;
      case VP_ENVX: return v.envx;
    }
    return 0;
  }
  return c.params[reg & 31];
}

void Spu2::setSwitch(uint32_t entry, uint32_t value) {
  Core& c = core_[entry & 1];
  const uint32_t reg = (entry >> 8) & 0x3F;
  value &= 0xFFFFFF;
  switch (reg) {
    case S_KON:
      for (int i = 0; i < 24; ++i)
        if (value & (1u << i)) {
          keyOn(c.voice[i]);
          c.endx &= ~(1u << i);
        }
      return;
    case S_KOFF:
      for (int i = 0; i < 24; ++i)
        if (value & (1u << i)) keyOff(c.voice[i]);
      return;
    case S_ENDX: c.endx = value; return;  // writing clears flags on hardware; libsd rarely does
  }
  c.switches[reg & 15] = value;
}

uint32_t Spu2::getSwitch(uint32_t entry) {
  const Core& c = core_[entry & 1];
  const uint32_t reg = (entry >> 8) & 0x3F;
  if (reg == S_ENDX) return c.endx;
  if (reg == S_KON || reg == S_KOFF) return 0;
  return c.switches[reg & 15];
}

void Spu2::setAddr(uint32_t entry, uint32_t value) {
  Core& c = core_[entry & 1];
  const uint32_t reg = (entry >> 8) & 0x3F;
  value &= kRamSize - 1;
  if (reg >= VA_SSA && reg <= VA_NAX) {
    Voice& v = c.voice[((entry >> 1) & 31) % 24];
    if (reg == VA_SSA) v.ssa = value;
    if (reg == VA_LSAX) v.lsa = value, v.lsaSet = true;
    if (reg == VA_NAX) v.nax = value;
    return;
  }
  c.addrs[reg & 15] = value;
}

uint32_t Spu2::getAddr(uint32_t entry) const {
  const Core& c = core_[entry & 1];
  const uint32_t reg = (entry >> 8) & 0x3F;
  if (reg >= VA_SSA && reg <= VA_NAX) {
    const Voice& v = c.voice[((entry >> 1) & 31) % 24];
    return reg == VA_SSA ? v.ssa : reg == VA_LSAX ? v.lsa : v.nax;
  }
  return c.addrs[reg & 15];
}

namespace {
// Reverb presets (the PS1 SPU's, whose tables libsd shares): dAPF1 dAPF2 vIIR vCOMB1-4 vWALL vAPF1
// vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2 dLSAME dRSAME mLDIFF mRDIFF mLCOMB3 mRCOMB3
// mLCOMB4 mRCOMB4 dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2 vLIN vRIN; offsets in 8-byte units.
struct Preset {
  uint32_t size;  // work area bytes
  uint16_t r[32];
};
constexpr Preset kRoom = {0x26C0, {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6,
                                   0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000,
                                   0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000}};
constexpr Preset kStudioSmall = {0x1F40, {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0,
                                          0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E,
                                          0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026,
                                          0x8000, 0x8000}};
constexpr Preset kStudioMedium = {0x4840, {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0,
                                           0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E,
                                           0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080,
                                           0x8000, 0x8000}};
constexpr Preset kStudioLarge = {0x6FE0, {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0,
                                          0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9,
                                          0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA,
                                          0x8000, 0x8000}};
constexpr Preset kHall = {0xADE0, {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA,
                                   0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1,
                                   0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000}};
constexpr Preset kHalfEcho = {0x3C00, {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371,
                                       0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1,
                                       0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000}};
constexpr Preset kSpaceEcho = {0xF6C0, {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400,
                                        0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055,
                                        0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232,
                                        0x8000, 0x8000}};
constexpr Preset kEcho = {0x18040, {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFF,
                                    0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000,
                                    0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000}};
constexpr Preset kDelay = {0x18040, {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFF,
                                     0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000,
                                     0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000}};
// libsd modes: 1 room, 2-4 studio A/B/C, 5 hall, 6 space, 7 echo, 8 delay, 9 pipe.
constexpr const Preset* kPresets[10] = {nullptr,     &kRoom,      &kStudioSmall, &kStudioMedium, &kStudioLarge,
                                        &kHall,      &kSpaceEcho, &kEcho,        &kDelay,        &kHalfEcho};
}  // namespace

void Spu2::setEffect(int core, uint32_t mode, int16_t depthL, int16_t depthR) {
  Core& c = core_[core & 1];
  Reverb& r = c.reverb;
  const Preset* p = kPresets[(mode & 0xFF) < 10 ? mode & 0xFF : 0];
  r.volL = depthL, r.volR = depthR;
  if (!p) {
    r.preset = nullptr;
    return;
  }
  // The work area ends at the core's EEA (sceSdSetAddr SD_A_EEA) and is as large as the preset needs.
  const uint32_t eea = c.addrs[0x1D & 15] ? c.addrs[0x1D & 15] | 1 : (core ? 0x1DFFFF : 0x1FFFFF);
  r.preset = p->r;
  r.end = eea & ~1u;
  r.start = (eea + 1 - p->size) & ~1u;
  r.cur = r.start;
  if (mode & 0x100) std::memset(ram.data() + r.start, 0, r.end + 2 - r.start);
}

void Spu2::setCoreAttr(uint32_t entry, uint32_t value) { core_[entry & 1].attrs[(entry >> 1) & 7] = value; }
uint32_t Spu2::getCoreAttr(uint32_t entry) const { return core_[entry & 1].attrs[(entry >> 1) & 7]; }

uint32_t Spu2::write(uint32_t spuAddr, const uint8_t* src, uint32_t size) {
  spuAddr &= kRamSize - 1;
  size = std::min(size, kRamSize - spuAddr);
  std::memcpy(ram.data() + spuAddr, src, size);
  return size;
}

uint32_t Spu2::read(uint32_t spuAddr, uint8_t* dst, uint32_t size) const {
  spuAddr &= kRamSize - 1;
  size = std::min(size, kRamSize - spuAddr);
  std::memcpy(dst, ram.data() + spuAddr, size);
  return size;
}

void Spu2::keyOn(Voice& v) {
  ++keyOns;
  v.on = true;
  v.phase = Phase::Attack;
  v.envx = 0;
  v.envCounter = 0;
  v.nax = v.ssa;
  v.pos = 0;
  v.prev[0] = v.prev[1] = 0;
  v.last = 0;
  v.lsaSet = false;
  v.decoded = false;
}

void Spu2::keyOff(Voice& v) {
  if (!v.on) return;
  v.phase = Phase::Release;
  v.envCounter = 0;
}

void Spu2::readBlock(const Core& c, uint32_t addr) {
  constexpr uint32_t kAttrIrqEnable = 2, kAddrIrqa = 0x1F & 15;
  if (c.attrs[kAttrIrqEnable] && (c.addrs[kAddrIrqa] & ~15u) == addr) irqPending |= 1u << (&c - core_);
}

// One 16-byte ADPCM block: a shift/filter byte, a flag byte and 28 4-bit samples, each added to
// a prediction from the two before it.
void Spu2::decodeBlock(Core& c, Voice& v) {
  static constexpr int kF0[5] = {0, 60, 115, 98, 122}, kF1[5] = {0, 0, -52, -55, -60};
  readBlock(c, v.nax);
  const uint8_t* b = ram.data() + (v.nax & (kRamSize - 1));
  int shift = b[0] & 15, filter = std::min((b[0] >> 4) & 7, 4);
  if (shift > 12) shift = 9;
  if ((b[1] & 4) && !v.lsaSet) v.lsa = v.nax;  // loop start
  for (int i = 0; i < 28; ++i) {
    const int nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
    int32_t sample = (int32_t(int16_t(nibble << 12)) >> shift) + ((v.prev[0] * kF0[filter] + v.prev[1] * kF1[filter] + 32) >> 6);
    sample = std::clamp(sample, -32768, 32767);
    v.prev[1] = v.prev[0];
    v.prev[0] = int16_t(sample);
    v.block[i] = int16_t(sample);
  }
  v.decoded = true;
}

// The ADSR envelope, one sample further (rates as on the PS1/PS2 SPU).
void Spu2::stepEnvelope(Voice& v) {
  bool exponential, decrease;
  int rate;
  switch (v.phase) {
    case Phase::Attack: exponential = v.adsr1 & 0x8000, decrease = false, rate = (v.adsr1 >> 8) & 0x7F; break;
    case Phase::Decay: exponential = true, decrease = true, rate = ((v.adsr1 >> 4) & 15) * 4; break;
    case Phase::Sustain:
      exponential = v.adsr2 & 0x8000, decrease = v.adsr2 & 0x4000, rate = (v.adsr2 >> 6) & 0x7F;
      break;
    case Phase::Release: exponential = v.adsr2 & 0x20, decrease = true, rate = (v.adsr2 & 31) * 4; break;
    default: return;
  }
  if (v.envCounter > 0) {
    --v.envCounter;
    return;
  }
  const int shift = rate >> 2;
  int step = decrease ? -8 + (rate & 3) : 7 - (rate & 3);
  uint32_t cycles = 1u << std::max(0, shift - 11);
  step <<= std::max(0, 11 - shift);
  if (exponential && !decrease && v.envx > 0x6000) cycles *= 4;
  if (exponential && decrease) step = step * v.envx >> 15;
  v.envCounter = cycles - 1;
  v.envx = std::clamp(v.envx + step, 0, 0x7FFF);
  switch (v.phase) {
    case Phase::Attack:
      if (v.envx >= 0x7FFF) v.phase = Phase::Decay;
      break;
    case Phase::Decay:
      if (v.envx <= std::min(((v.adsr1 & 15) + 1) * 0x800, 0x7FFF)) v.phase = Phase::Sustain;
      break;
    case Phase::Release:
      if (v.envx == 0) v.phase = Phase::Off, v.on = false;
      break;
    default: break;
  }
}

int32_t Spu2::nextSample(Core& c, int index) {
  Voice& v = c.voice[index];
  if (!v.decoded) decodeBlock(c, v);
  stepEnvelope(v);
  if (!v.on) return 0;
  const uint32_t i = v.pos >> 12, frac = v.pos & 0xFFF;
  const int32_t a = i ? v.block[i - 1] : v.last, b = v.block[i];
  const int32_t sample = a + ((b - a) * int32_t(frac) >> 12);
  const int32_t out = sample * v.envx >> 15;
  v.pos += std::min<uint32_t>(v.pitch, 0x3FFF);
  while ((v.pos >> 12) >= 28) {
    v.pos -= 28 << 12;
    v.last = v.block[27];
    const uint8_t flags = ram[(v.nax + 1) & (kRamSize - 1)];
    if (flags & 1) {  // end of the sample: repeat from the loop start, or stop
      c.endx |= 1u << index;
      if (!(flags & 2)) {
        v.on = false;
        v.phase = Phase::Off;
        v.envx = 0;
        return out;
      }
      v.nax = v.lsa;
    } else {
      v.nax = (v.nax + kBlockBytes) & (kRamSize - 1);
    }
    decodeBlock(c, v);
  }
  return out;
}

void Spu2::startAutoDma(int core, uint32_t iopAddr, uint32_t size) {
  adma[core & 1] = AutoDma{true, iopAddr, size, 0};
}

uint32_t Spu2::autoDmaStatus(int core) const {
  const AutoDma& a = adma[core & 1];
  if (!a.active || a.size < 8) return 0;
  const uint32_t pos = uint32_t(a.samples * 4 % a.size);
  return (pos >= a.size / 2 ? 1u << 24 : 0) | ((a.iop + pos) & 0xFFFFFF);
}

namespace {
// Voice and master volumes: bit 15 clear = fixed volume in bits 0-14 (signed); sweeps are taken at full.
int32_t volume(uint32_t reg) { return (reg & 0x8000) ? 0x7FFF : int32_t(int16_t(uint16_t(reg << 1))); }
// Input volumes (AVOL, BVOL, EVOL) are plain signed 16-bit values.
int32_t plain(uint32_t reg) { return int16_t(uint16_t(reg)); }
int32_t scale(int32_t sample, int32_t vol) { return sample * vol >> 15; }
}  // namespace

// One output sample of the core's reverb: the PS1 formula runs at half the rate, so inputs are
// averaged over two samples and the output is interpolated between steps.
void Spu2::reverb(Core& c, int32_t inL, int32_t inR, int32_t& outL, int32_t& outR) {
  Reverb& rv = c.reverb;
  outL = outR = 0;
  if (!rv.preset) return;
  rv.inL += inL, rv.inR += inR;
  rv.odd = !rv.odd;
  if (rv.odd) {  // between two steps
    outL = (rv.prevL + rv.outL) / 2, outR = (rv.prevR + rv.outR) / 2;
    return;
  }
  const uint16_t* r = rv.preset;
  const uint32_t length = rv.end + 2 - rv.start;
  auto addr = [&](int32_t offset) {  // bytes from the current position, wrapped in the work area
    int64_t a = int64_t(rv.cur - rv.start) + offset;
    a %= int64_t(length);
    if (a < 0) a += length;
    return rv.start + uint32_t(a);
  };
  auto rd = [&](int32_t offset) {
    int16_t v;
    std::memcpy(&v, ram.data() + (addr(offset) & (kRamSize - 2)), 2);
    return int32_t(v);
  };
  auto wr = [&](int32_t offset, int32_t v) {
    const int16_t s = int16_t(std::clamp(v, -32768, 32767));
    std::memcpy(ram.data() + (addr(offset) & (kRamSize - 2)), &s, 2);
  };
  auto off = [&](int i) { return int32_t(r[i]) * 8; };
  auto vol = [&](int i) { return int32_t(int16_t(r[i])); };
  auto mul = [](int32_t a, int32_t b) { return int32_t(int64_t(a) * b >> 15); };
  enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
         mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
         mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };
  const int32_t lin = mul(vol(vLIN), std::clamp(rv.inL / 2, -32768, 32767));
  const int32_t rin = mul(vol(vRIN), std::clamp(rv.inR / 2, -32768, 32767));
  rv.inL = rv.inR = 0;
  // Same side and different side reflections.
  auto reflect = [&](int dst, int src, int32_t in) {
    const int32_t before = rd(off(dst) - 2);
    wr(off(dst), mul(in + mul(rd(off(src)), vol(vWALL)) - before, vol(vIIR)) + before);
  };
  reflect(mLSAME, dLSAME, lin);
  reflect(mRSAME, dRSAME, rin);
  reflect(mLDIFF, dRDIFF, lin);
  reflect(mRDIFF, dLDIFF, rin);
  // Early echo (comb filters), then two all-pass filters.
  int32_t l = mul(vol(vCOMB1), rd(off(mLCOMB1))) + mul(vol(vCOMB2), rd(off(mLCOMB2))) + mul(vol(vCOMB3), rd(off(mLCOMB3))) +
              mul(vol(vCOMB4), rd(off(mLCOMB4)));
  int32_t rr = mul(vol(vCOMB1), rd(off(mRCOMB1))) + mul(vol(vCOMB2), rd(off(mRCOMB2))) + mul(vol(vCOMB3), rd(off(mRCOMB3))) +
               mul(vol(vCOMB4), rd(off(mRCOMB4)));
  auto allPass = [&](int32_t x, int m, int d, int v) {
    const int32_t delayed = rd(off(m) - off(d));
    x -= mul(vol(v), delayed);
    wr(off(m), x);
    return mul(std::clamp(x, -32768, 32767), vol(v)) + delayed;
  };
  l = allPass(l, mLAPF1, dAPF1, vAPF1);
  rr = allPass(rr, mRAPF1, dAPF1, vAPF1);
  l = allPass(l, mLAPF2, dAPF2, vAPF2);
  rr = allPass(rr, mRAPF2, dAPF2, vAPF2);
  rv.prevL = rv.outL, rv.prevR = rv.outR;
  rv.outL = mul(std::clamp(l, -32768, 32767), rv.volL);
  rv.outR = mul(std::clamp(rr, -32768, 32767), rv.volR);
  rv.cur = rv.cur + 2 > rv.end ? rv.start : rv.cur + 2;
  outL = rv.outL, outR = rv.outR;
}

void Spu2::advance(uint64_t samples) {
  if (samples == 0) return;
  enum : uint32_t { kMvolL = 0x09, kMvolR = 0x0A, kAvolL = 0x0D, kAvolR = 0x0E, kBvolL = 0x0F, kBvolR = 0x10 };
  enum : uint32_t { kVmixL = 0x18 & 15, kVmixEL = 0x19 & 15, kVmixR = 0x1A & 15, kVmixER = 0x1B & 15 };
  for (uint64_t n = 0; n < samples; ++n) {
    int32_t mixL[2] = {}, mixR[2] = {};
    for (int ci = 0; ci < 2; ++ci) {
      Core& c = core_[ci];
      int32_t l = 0, r = 0, wetL = 0, wetR = 0;
      for (int i = 0; i < 24; ++i) {
        if (!c.voice[i].on) continue;
        const int32_t s = nextSample(c, i);
        const int32_t vl = scale(s, volume(c.voice[i].volL)), vr = scale(s, volume(c.voice[i].volR));
        if (c.switches[kVmixL] & (1u << i)) l += vl;
        if (c.switches[kVmixR] & (1u << i)) r += vr;
        if (c.switches[kVmixEL] & (1u << i)) wetL += vl;
        if (c.switches[kVmixER] & (1u << i)) wetR += vr;
      }
      constexpr uint32_t kAttrEffectEnable = 1;
      if (c.attrs[kAttrEffectEnable]) {
        int32_t el, er;
        reverb(c, wetL, wetR, el, er);
        l += el, r += er;
      }
      AutoDma& a = adma[ci];
      if (a.active && a.size >= 1024 && iopRam) {
        const uint32_t pos = uint32_t(a.samples * 4 % a.size), block = pos / 1024, k = (pos % 1024) / 4;
        const uint32_t base = a.iop + block * 1024 + k * 2;
        if (base + 512 + 2 <= iopRam->size()) {
          int16_t sl, sr;
          std::memcpy(&sl, iopRam->data() + base, 2);
          std::memcpy(&sr, iopRam->data() + base + 512, 2);
          l += scale(sl, plain(c.params[kBvolL]));
          r += scale(sr, plain(c.params[kBvolR]));
        }
      }
      if (a.active) ++a.samples;
      mixL[ci] = l, mixR[ci] = r;
    }
    // Core 0's output is core 1's external input; core 1 drives the speakers.
    const Core& c0 = core_[0];
    const Core& c1 = core_[1];
    const int32_t l = scale(scale(scale(mixL[0], volume(c0.params[kMvolL])), plain(c1.params[kAvolL])) + mixL[1], volume(c1.params[kMvolL]));
    const int32_t r = scale(scale(scale(mixR[0], volume(c0.params[kMvolR])), plain(c1.params[kAvolR])) + mixR[1], volume(c1.params[kMvolR]));
    if (output) {
      output->push_back(int16_t(std::clamp(l, -32768, 32767)));
      output->push_back(int16_t(std::clamp(r, -32768, 32767)));
    }
  }
  // Voices that are off still count as done for the drivers.
  for (Core& c : core_)
    for (Voice& v : c.voice)
      if (!v.on) v.envx = 0;
}

