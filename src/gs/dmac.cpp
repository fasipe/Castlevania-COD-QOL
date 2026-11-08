#include "gs/dmac.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

#include "ee/cpu.h"
#include "ee/memory.h"

namespace {
constexpr uint32_t kBase[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
constexpr uint32_t kStr = 1u << 8, kTte = 1u << 6;
enum : uint32_t { kCtrl = 0x1000E000, kPcr = 0x1000E020, kSqwc = 0x1000E030, kRbsr = 0x1000E040,
                  kRbor = 0x1000E050, kStadr = 0x1000E060 };
enum : uint32_t { kTagRefe, kTagCnt, kTagNext, kTagRef, kTagRefs, kTagCall, kTagRet, kTagEnd };

// Bit 31 of a DMA address selects the scratchpad; otherwise it is a physical address: RAM, or
// the VUs' micro and data memories at 0x11000000 (the game builds meshes in VU0 data memory and
// copies them to the scratchpad from there).
uint32_t dmaAddr(uint32_t a) {
  if (a & 0x80000000u) return 0x70000000u | (a & 0x3FF0);
  a &= 0x1FFFFFF0u;
  if (a >= 0x11000000u && a < 0x11010000u) return a;
  return a & 0x01FFFFF0u;
}
}  // namespace

int Dmac::channelOf(uint32_t phys, uint32_t& reg) const {
  for (int c = 0; c < 10; ++c) {
    if (phys >= kBase[c] && phys < kBase[c] + 0x100) {
      reg = phys - kBase[c];
      return c;
    }
  }
  return -1;
}

bool Dmac::read(uint32_t phys, uint64_t& value) {
  uint32_t reg;
  const int c = channelOf(phys, reg);
  if (c < 0) {
    switch (phys) {
      case kCtrl: value = ctrl_; return true;
      case kPcr: value = pcr_; return true;
      case kSqwc: value = sqwc_; return true;
      case kRbsr: value = rbsr_; return true;
      case kRbor: value = rbor_; return true;
      case kStadr: value = stadr_; return true;
    }
    return false;  // D_STAT lives in Hardware, with the interrupt logic
  }
  const Chan& ch = ch_[c];
  switch (reg) {
    case 0x00: value = ch.chcr; return true;
    case 0x10: value = ch.madr; return true;
    case 0x20: value = ch.qwc; return true;
    case 0x30: value = ch.tadr; return true;
    case 0x40: value = ch.asr[0]; return true;
    case 0x50: value = ch.asr[1]; return true;
    case 0x80: value = ch.sadr; return true;
  }
  value = 0;
  return true;
}

bool Dmac::write(uint32_t phys, uint64_t value) {
  const uint32_t v = uint32_t(value);
  uint32_t reg;
  const int c = channelOf(phys, reg);
  if (c < 0) {
    switch (phys) {
      case kCtrl: ctrl_ = v; return true;
      case kPcr: pcr_ = v; return true;
      case kSqwc: sqwc_ = v; return true;
      case kRbsr: rbsr_ = v; return true;
      case kRbor: rbor_ = v; return true;
      case kStadr: stadr_ = v; return true;
    }
    return false;
  }
  Chan& ch = ch_[c];
  switch (reg) {
    case 0x00:
      ch.chcr = v;
      if (v & kStr) start(c);
      return true;
    case 0x10: ch.madr = v; return true;
    case 0x20: ch.qwc = v & 0xFFFF; return true;
    case 0x30: ch.tadr = v; return true;
    case 0x40: ch.asr[0] = v; return true;
    case 0x50: ch.asr[1] = v; return true;
    case 0x80: ch.sadr = v & 0x3FF0; return true;
  }
  return true;
}

void Dmac::sendBlock(int c, uint32_t addr, uint32_t qwc) {
  if (!qwc) return;
  const uint8_t* src = mem_.hostPtr(dmaAddr(addr), qwc * 16);
  if (!src) {
    if (fault.empty()) fault = std::format("DMA ch{} reads unmapped 0x{:08X} ({} qwords)", c, addr, qwc);
    return;
  }
  qwords[c] += qwc;
  if (sinks[c]) sinks[c]->dmaWrite(reinterpret_cast<const u128*>(src), qwc);
}

// Scratchpad <-> RAM, one qword at a time (scratchpad addresses wrap at 16 KB).
void Dmac::sprBlock(int c, Chan& ch, uint32_t qwc) {
  for (uint32_t i = 0; i < qwc; ++i) {
    uint8_t* spr = mem_.hostPtr(0x70000000 | ch.sadr, 16);
    uint8_t* ram = mem_.hostPtr(dmaAddr(ch.madr), 16);
    if (!ram) {
      if (fault.empty()) fault = std::format("SPR DMA ch{} at unmapped 0x{:08X}", c, ch.madr);
      return;
    }
    if (c == kSprFrom)
      std::memcpy(ram, spr, 16);
    else
      std::memcpy(spr, ram, 16);
    ch.madr += 16;
    ch.sadr = (ch.sadr + 16) & 0x3FF0;
  }
  qwords[c] += qwc;
}

void Dmac::sprTransfer(int c, Chan& ch) {
  const uint32_t mode = (ch.chcr >> 2) & 3;
  if (mode == 0) {
    sprBlock(c, ch, ch.qwc);
  } else if (mode == 1 && c == kSprTo) {  // source chain: tags in RAM, data (and tags with TTE) to SPR
    ch.sp = 0;
    sprBlock(c, ch, ch.qwc);
    for (int guard = 0; guard < 1 << 20; ++guard) {
      const uint32_t tagAt = ch.tadr;
      if (!nextTag(c)) break;
      if (ch.chcr & kTte) {
        std::memcpy(mem_.hostPtr(0x70000000 | ch.sadr, 16), mem_.hostPtr(dmaAddr(tagAt), 16), 16);
        ch.sadr = (ch.sadr + 16) & 0x3FF0;
      }
      sprBlock(c, ch, ch.qwc);
      if (ch.lastTag) break;
    }
  } else if (mode == 1 && c == kSprFrom) {  // destination chain: each block starts with its tag in SPR
    for (int guard = 0; guard < 1 << 20; ++guard) {
      uint64_t tag;
      std::memcpy(&tag, mem_.hostPtr(0x70000000 | ch.sadr, 16), 8);
      ch.sadr = (ch.sadr + 16) & 0x3FF0;
      ch.chcr = (ch.chcr & 0xFFFF) | uint32_t(tag & 0xFFFF0000u);
      ch.madr = uint32_t(tag >> 32) & 0x7FFFFFF0u;
      sprBlock(c, ch, uint32_t(tag & 0xFFFF));
      if (((tag >> 28) & 7) == kTagEnd || (((tag >> 31) & 1) && (ch.chcr & (1u << 7)))) break;
    }
  } else if (fault.empty()) {
    fault = std::format("SPR DMA ch{} in mode {}", c, mode);
  }
  ch.qwc = 0;
}

void Dmac::finish(int c) {
  if (sinks[c]) sinks[c]->dmaDone();
  ++transfers[c];
  ch_[c].chcr &= ~kStr;
  mem_.hw.dmacStat |= 1u << c;
}

// Loads the next chain tag of a device channel into MADR/QWC; false if the chain cannot go on.
bool Dmac::nextTag(int c) {
  Chan& ch = ch_[c];
  const uint8_t* t = mem_.hostPtr(dmaAddr(ch.tadr), 16);
  if (!t) {
    if (fault.empty()) fault = std::format("DMA ch{} tag at unmapped 0x{:08X}", c, ch.tadr);
    return false;
  }
  uint64_t tag;
  std::memcpy(&tag, t, 8);
  const uint32_t qwc = uint32_t(tag & 0xFFFF), id = uint32_t(tag >> 28) & 7;
  const uint32_t addr = uint32_t(tag >> 32) & 0xFFFFFFF0u, next = ch.tadr + 16;  // bit 31: scratchpad
  ch.chcr = (ch.chcr & 0xFFFF) | uint32_t(tag & 0xFFFF0000u);
  ch.qwc = qwc;
  ch.lastTag = false;
  switch (id) {
    case kTagRefe: ch.madr = addr; ch.tadr = next; ch.lastTag = true; break;
    case kTagCnt: ch.madr = next; ch.tadr = next + qwc * 16; break;
    case kTagNext: ch.madr = next; ch.tadr = addr; break;
    case kTagRef: case kTagRefs: ch.madr = addr; ch.tadr = next; break;
    case kTagCall:
      ch.madr = next;
      if (ch.sp < 2) ch.stack[ch.sp++] = next + qwc * 16;
      ch.tadr = addr;
      break;
    case kTagRet:
      ch.madr = next;
      if (ch.sp > 0) ch.tadr = ch.stack[--ch.sp]; else ch.lastTag = true;
      break;
    case kTagEnd: ch.madr = next; ch.tadr = next + qwc * 16; ch.lastTag = true; break;
  }
  if (((tag >> 31) & 1) && (ch.chcr & (1u << 7))) ch.lastTag = true;  // TIE: stop after this tag
  return true;
}

uint32_t Dmac::pull(int c, void* dst, uint32_t maxQw) {
  Chan& ch = ch_[c];
  const bool chain = ((ch.chcr >> 2) & 3) == 1;
  uint32_t done = 0;
  auto* out = static_cast<uint8_t*>(dst);
  while (done < maxQw && active(c)) {
    if (ch.qwc == 0) {
      if (!chain || ch.lastTag || !nextTag(c)) {
        finish(c);
        break;
      }
      continue;
    }
    const uint32_t n = std::min(ch.qwc, maxQw - done);
    const uint8_t* src = mem_.hostPtr(dmaAddr(ch.madr), n * 16);
    if (!src) {
      if (fault.empty()) fault = std::format("DMA ch{} reads unmapped 0x{:08X}", c, ch.madr);
      finish(c);
      break;
    }
    std::memcpy(out + done * 16, src, n * 16);
    ch.madr += n * 16;
    ch.qwc -= n;
    done += n;
    qwords[c] += n;
    if (ch.qwc == 0 && (!chain || ch.lastTag)) finish(c);
  }
  return done;
}

uint32_t Dmac::push(int c, const void* src, uint32_t qw) {
  Chan& ch = ch_[c];
  if (!active(c)) return 0;
  const uint32_t n = std::min(ch.qwc, qw);
  uint8_t* dst = mem_.hostPtr(dmaAddr(ch.madr), n * 16);
  if (!dst) {
    if (fault.empty()) fault = std::format("DMA ch{} writes unmapped 0x{:08X}", c, ch.madr);
    finish(c);
    return 0;
  }
  std::memcpy(dst, src, n * 16);
  ch.madr += n * 16;
  ch.qwc -= n;
  qwords[c] += n;
  if (ch.qwc == 0) finish(c);
  return n;
}

// A chain restarted in the middle of a block (QWC > 0) first finishes that block; CHCR still
// holds the upper half of its tag, which says whether the chain ends after it.
bool Dmac::endsAfterCurrent(const Chan& ch) const {
  const uint32_t id = (ch.chcr >> 28) & 7;
  return id == kTagRefe || id == kTagEnd || ((ch.chcr >> 31) & 1 && (ch.chcr & (1u << 7)));
}

void Dmac::start(int c) {
  Chan& ch = ch_[c];
  const uint32_t mode = (ch.chcr >> 2) & 3;
  if (devices[c]) {
    ch.sp = 0;
    ch.lastTag = mode == 1 && ch.qwc > 0 && endsAfterCurrent(ch);
    if (mode == 1 && ch.qwc == 0 && !nextTag(c)) return finish(c);
    devices[c]->dmaStarted(c);
    return;
  }
  if (c == kSprFrom || c == kSprTo) {
    sprTransfer(c, ch);
  } else if (c == kSif0 || c == kSif1 || c == kSif2) {
    // SIF transfers are done by the kernel's sceSifSetDma in this port; the channels only
    // need to report completion.
  } else if (mode == 0) {  // normal: one block from MADR
    sendBlock(c, ch.madr, ch.qwc);
    ch.madr += ch.qwc * 16;
    ch.qwc = 0;
  } else if (mode == 1 && (ch.chcr & 1)) {  // source chain
    uint32_t stack[2];
    int sp = 0;
    bool done = false;
    if (ch.qwc) {  // restarted in the middle of a block
      sendBlock(c, ch.madr, ch.qwc);
      ch.madr += ch.qwc * 16;
      ch.qwc = 0;
      done = endsAfterCurrent(ch);
    }
    for (int guard = 0; !done && guard < 1 << 20; ++guard) {
      uint64_t tag[2];
      const uint8_t* t = mem_.hostPtr(dmaAddr(ch.tadr), 16);
      if (!t) {
        if (fault.empty()) fault = std::format("DMA ch{} tag at unmapped 0x{:08X}", c, ch.tadr);
        break;
      }
      if (c == kVif1 && vif1TagAt) vif1TagAt(ch.tadr);
      std::memcpy(tag, t, 16);
      const uint32_t qwc = uint32_t(tag[0] & 0xFFFF), id = uint32_t(tag[0] >> 28) & 7;
      const uint32_t addr = uint32_t(tag[0] >> 32) & 0xFFFFFFF0u;  // bit 31: scratchpad
      const bool irq = (tag[0] >> 31) & 1;
      ch.chcr = (ch.chcr & 0xFFFF) | uint32_t(tag[0] & 0xFFFF0000u);
      if ((ch.chcr & kTte) && sinks[c]) sinks[c]->dmaTag(tag[1]);  // tag's upper half: device words
      const uint32_t next = ch.tadr + 16;
      switch (id) {
        case kTagRefe: sendBlock(c, addr, qwc); ch.tadr = next; done = true; break;
        case kTagCnt: sendBlock(c, next, qwc); ch.tadr = next + qwc * 16; break;
        case kTagNext: sendBlock(c, next, qwc); ch.tadr = addr; break;
        case kTagRef:
        case kTagRefs: sendBlock(c, addr, qwc); ch.tadr = next; break;
        case kTagCall:
          sendBlock(c, next, qwc);
          if (sp < 2) stack[sp++] = next + qwc * 16;
          ch.tadr = addr;
          break;
        case kTagRet:
          sendBlock(c, next, qwc);
          if (sp > 0) {
            ch.tadr = stack[--sp];
          } else {
            done = true;
          }
          break;
        case kTagEnd: sendBlock(c, next, qwc); ch.tadr = next + qwc * 16; done = true; break;
      }
      if (irq && (ch.chcr & (1u << 7))) done = true;  // TIE: stop at a tag with IRQ set
    }
    ch.qwc = 0;
  } else {
    if (fault.empty()) fault = std::format("DMA ch{} mode {} dir {} not supported", c, mode, ch.chcr & 1);
  }
  finish(c);
}
