#include "gs/vif.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "ee/cpu.h"
#include "ee/vu.h"

namespace {
enum : uint32_t {
  kNop = 0x00, kStcycl = 0x01, kOffset = 0x02, kBase = 0x03, kItop = 0x04, kStmod = 0x05,
  kMskpath3 = 0x06, kMark = 0x07, kFlushe = 0x10, kFlush = 0x11, kFlusha = 0x13, kMscal = 0x14,
  kMscalf = 0x15, kMscnt = 0x17, kStmask = 0x20, kStrow = 0x30, kStcol = 0x31, kMpg = 0x4A,
  kDirect = 0x50, kDirectHl = 0x51,
};

// UNPACK in the common case (no write mask, no ROW addition, every qword fed from the stream):
// one element per qword, decoded with the format known at compile time.
template <uint32_t VN, uint32_t VL>
void unpackPlain(uint8_t* data, uint32_t qwords, uint32_t base, uint32_t num, uint32_t cl, uint32_t wl, bool usn,
                 const uint8_t* src) {
  constexpr uint32_t comps = VN + 1;
  constexpr int written = VN == 2 ? 3 : 4;  // V3 leaves W alone
  for (uint32_t i = 0; i < num; ++i) {
    const uint32_t addr = cl == wl ? base + i : base + (i / wl) * cl + i % wl;
    uint32_t* dst = reinterpret_cast<uint32_t*>(data + (addr % qwords) * 16);
    uint32_t v[4];
    if constexpr (VN == 3 && VL == 3) {  // V4-5: RGBA 5:5:5:1 in 16 bits
      uint16_t h;
      std::memcpy(&h, src + i * 2, 2);
      v[0] = (h & 0x1Fu) << 3, v[1] = ((h >> 5) & 0x1Fu) << 3, v[2] = ((h >> 10) & 0x1Fu) << 3, v[3] = ((h >> 15) & 1u) << 7;
    } else {
      for (uint32_t c = 0; c < 4; ++c) {  // S formats repeat their value, V2 repeats x,y in z,w
        const uint32_t k = i * comps + c % comps;
        if constexpr (VL == 0) {
          std::memcpy(&v[c], src + k * 4, 4);
        } else if constexpr (VL == 1) {
          uint16_t h;
          std::memcpy(&h, src + k * 2, 2);
          v[c] = usn ? h : uint32_t(int32_t(int16_t(h)));
        } else {
          v[c] = usn ? src[k] : uint32_t(int32_t(int8_t(src[k])));
        }
      }
    }
    for (int c = 0; c < written; ++c) dst[c] = v[c];
  }
}

// Words of packed data an UNPACK of `num` elements takes (vn = components - 1, vl = width).
uint32_t unpackWords(uint32_t cmd, uint32_t num) {
  const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
  const uint32_t bitsPerElem = (vn == 3 && vl == 3) ? 16 : (vn + 1) * (32u >> vl);
  return (num * bitsPerElem + 31) / 32;
}
}  // namespace

void Vif::dmaWrite(const u128* data, uint32_t qwc) {
  const uint32_t* w = reinterpret_cast<const uint32_t*>(data);
  fifo_.insert(fifo_.end(), w, w + qwc * 4);
  process();
}

void Vif::dmaTag(uint64_t upper) {
  fifo_.push_back(uint32_t(upper));
  fifo_.push_back(uint32_t(upper >> 32));
  process();
}

bool Vif::read(uint32_t phys, uint64_t& value) {
  const uint32_t base = unit_ == 0 ? 0x10003800 : 0x10003C00;
  if (phys < base || phys >= base + 0x400) return false;
  switch (phys - base) {
    case 0x000: value = stat_; return true;
    case 0x020: value = err_; return true;
    case 0x030: value = mark_; return true;
    case 0x040: value = cl_ | (wl_ << 8); return true;
    case 0x050: value = mode_; return true;
    case 0x060: value = 0; return true;  // NUM
    case 0x070: value = mask_; return true;
    case 0x080: value = lastCode_; return true;
    case 0x090: value = itops_; return true;
    case 0x0A0: value = base_; return true;
    case 0x0B0: value = ofst_; return true;
    case 0x0C0: value = tops_; return true;
    case 0x0D0: value = itop_; return true;
    case 0x0E0: value = top_; return true;
  }
  value = 0;
  return true;
}

bool Vif::write(uint32_t phys, uint64_t value) {
  const uint32_t base = unit_ == 0 ? 0x10003800 : 0x10003C00;
  if (phys < base || phys >= base + 0x400) return false;
  switch (phys - base) {
    case 0x010:  // FBRST: reset clears the queue and state
      if (value & 1) {
        fifo_.clear();
        stat_ = 0;
      }
      return true;
    case 0x020: err_ = uint32_t(value); return true;
    case 0x030: mark_ = uint32_t(value); return true;
  }
  return true;
}

void Vif::process() {
  size_t pos = 0;
  while (pos < fifo_.size() && fault.empty()) {
    const uint32_t code = fifo_[pos];
    size_t next = pos;
    if (!execute(code, next)) break;
    pos = next;
  }
  fifo_.erase(fifo_.begin(), fifo_.begin() + std::min(pos, fifo_.size()));
}

bool Vif::execute(uint32_t code, size_t& pos) {
  const uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
  const size_t avail = fifo_.size() - pos - 1;
  const uint32_t* args = fifo_.data() + pos + 1;
  auto consume = [&](size_t words) { pos += 1 + words; };

  if ((cmd & 0x60) == 0x60) {  // UNPACK
    const uint32_t n = num ? num : 256;
    const uint32_t cl = cl_ ? cl_ : 1, wl = wl_ ? wl_ : 1;
    const uint32_t elems = cl >= wl ? n : (n / wl) * cl + std::min(n % wl, cl);
    const uint32_t words = unpackWords(cmd, elems);
    if (avail < words) return false;
    unpack(code, args);
    consume(words);
    ++commands;
    lastCode_ = code;
    return true;
  }
  switch (cmd) {
    case kNop: case kMark: case kFlushe: case kFlush: case kFlusha: case kMskpath3:
      if (cmd == kMark) mark_ = imm;
      consume(0);
      break;
    case kStcycl: cl_ = imm & 0xFF; wl_ = (imm >> 8) & 0xFF; consume(0); break;
    case kOffset: ofst_ = imm & 0x3FF; dbf_ = 0; tops_ = base_; consume(0); break;
    case kBase: base_ = imm & 0x3FF; consume(0); break;
    case kItop: itops_ = imm & 0x3FF; consume(0); break;
    case kStmod: mode_ = imm & 3; consume(0); break;
    case kMscal: case kMscalf: callMicro(imm * 8); consume(0); break;
    case kMscnt: callMicro(0xFFFFFFFF); consume(0); break;
    case kStmask:
      if (avail < 1) return false;
      mask_ = args[0];
      consume(1);
      break;
    case kStrow: case kStcol:
      if (avail < 4) return false;
      std::memcpy(cmd == kStrow ? row_ : col_, args, 16);
      consume(4);
      break;
    case kMpg: {  // upload num (0 = 256) 64-bit instructions at imm*8
      const uint32_t n = num ? num : 256;
      if (avail < n * 2) return false;
      for (uint32_t i = 0; i < n; ++i) std::memcpy(code_ + ((imm + i) * 8) % codeBytes_, args + i * 2, 8);
      vu_.codeChanged();
      consume(n * 2);
      break;
    }
    case kDirect: case kDirectHl: {  // imm (0 = 65536) qwords for GIF PATH2
      const uint32_t qwc = imm ? imm : 65536;
      if (avail < size_t(qwc) * 4) return false;
      if (direct) direct(reinterpret_cast<const u128*>(args), qwc);
      consume(size_t(qwc) * 4);
      break;
    }
    default:
      fault = std::format("VIF{} command 0x{:08X}", unit_, code);
      return false;
  }
  ++commands;
  lastCode_ = code;
  return true;
}

// UNPACK: expand packed elements into qwords of VU data memory, honouring the write cycle
// (CL/WL), the addition modes (STMOD with ROW) and the write mask (STMASK with ROW/COL).
// NUM counts written qwords. With CL >= WL ("skipping") the first WL of every CL qwords are
// written; with CL < WL ("filling") qwords are contiguous and the last WL-CL of each cycle are
// not fed from the stream.
void Vif::unpack(uint32_t code, const uint32_t* words) {
  const uint32_t cmd = (code >> 24) & 0x7F, num = ((code >> 16) & 0xFF) ? (code >> 16) & 0xFF : 256;
  const uint32_t imm = code & 0xFFFF;
  const bool usn = imm & 0x4000, addTops = imm & 0x8000, masked = cmd & 0x10;
  const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
  const uint32_t base = (imm & 0x3FF) + (addTops && unit_ == 1 ? tops_ : 0);
  const uint32_t qwords = dataBytes_ / 16;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(words);
  const uint32_t cl = cl_ ? cl_ : 1, wl = wl_ ? wl_ : 1;

  if (!masked && mode_ == 0 && cl >= wl) {
    using Fn = void (*)(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, const uint8_t*);
    static constexpr Fn kPlain[16] = {
        unpackPlain<0, 0>, unpackPlain<0, 1>, unpackPlain<0, 2>, unpackPlain<0, 3>, unpackPlain<1, 0>, unpackPlain<1, 1>,
        unpackPlain<1, 2>, unpackPlain<1, 3>, unpackPlain<2, 0>, unpackPlain<2, 1>, unpackPlain<2, 2>, unpackPlain<2, 3>,
        unpackPlain<3, 0>, unpackPlain<3, 1>, unpackPlain<3, 2>, unpackPlain<3, 3>};
    kPlain[vn * 4 + vl](data_, qwords, base, num, cl, wl, usn, bytes);
    unpackedQwords += num;
    return;
  }

  auto element = [&](uint32_t index, uint32_t comp) -> uint32_t {
    if (vn == 3 && vl == 3) {  // V4-5: RGBA 5:5:5:1 in 16 bits
      uint16_t v;
      std::memcpy(&v, bytes + index * 2, 2);
      static constexpr int kShift[4] = {0, 5, 10, 15};
      const uint32_t c = (v >> kShift[comp]) & (comp == 3 ? 1 : 0x1F);
      return comp == 3 ? c << 7 : c << 3;
    }
    // S formats repeat their value in every component; V2 repeats x,y in z,w.
    const uint32_t comps = vn + 1;
    const uint32_t k = index * comps + (comp % comps);
    switch (vl) {
      case 0: { uint32_t v; std::memcpy(&v, bytes + k * 4, 4); return v; }
      case 1: { uint16_t v; std::memcpy(&v, bytes + k * 2, 2); return usn ? v : uint32_t(int32_t(int16_t(v))); }
      default: { const uint8_t v = bytes[k]; return usn ? v : uint32_t(int32_t(int8_t(v))); }
    }
  };

  uint32_t elem = 0;
  for (uint32_t i = 0; i < num; ++i) {
    const uint32_t cycle = i % wl, block = i / wl;
    const bool skipping = cl >= wl;
    const uint32_t addr = skipping ? base + block * cl + cycle : base + i;
    const bool fromStream = skipping || cycle < cl;
    uint32_t* dst = reinterpret_cast<uint32_t*>(data_ + (addr % qwords) * 16);
    const uint32_t row = std::min(cycle, 3u);
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t m = masked ? (mask_ >> (row * 8 + c * 2)) & 3 : 0;
      if (m == 0 && !fromStream) m = 1;  // filled qwords take ROW where data would go
      if (m == 0 && vn == 2 && c == 3) continue;  // V3 leaves W alone
      uint32_t v;
      switch (m) {
        case 0:
          v = element(elem, c);
          if (mode_ == 1) {
            v += row_[c];
          } else if (mode_ == 2) {
            v += row_[c];
            row_[c] = v;
          }
          break;
        case 1: v = row_[c]; break;
        case 2: v = col_[row]; break;
        default: continue;  // write-protected
      }
      dst[c] = v;
    }
    if (fromStream) ++elem;
    ++unpackedQwords;
  }
}

void Vif::callMicro(uint32_t addr) {
  if (unit_ == 1) {  // double buffering: TOP takes TOPS, TOPS flips between BASE and BASE+OFST
    top_ = tops_;
    itop_ = itops_;
    dbf_ ^= 1;
    tops_ = dbf_ ? base_ + ofst_ : base_;
  }
  ++microCalls;
  vu_.run(addr != 0xFFFFFFFF ? addr : vu_.tpc);  // MSCNT continues where the program stopped
  if (!vu_.fault.empty() && fault.empty()) fault = std::format("VU{} micro: {}", unit_, vu_.fault);
}
