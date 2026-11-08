#include "gs/gs.h"

#include <cstdio>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdlib>

#include "gs/color.h"
#include "gs/gpu.h"
#include "host/window.h"
#include "timer.h"

// In 1/16 pixels. The game's picture is 512 wide; the 2D made for 4:3 keeps its shape in 16:9 at 3/4.
static constexpr int32_t kPictureCentreX = 256 * 16, kWholeWidthNarrowed = 480 * 16 * 3 / 4;

namespace {
enum : uint32_t {
  PRIM = 0x00, RGBAQ = 0x01, ST = 0x02, UV = 0x03, XYZF2 = 0x04, XYZ2 = 0x05, TEX0_1 = 0x06,
  TEX0_2 = 0x07, CLAMP_1 = 0x08, CLAMP_2 = 0x09, FOG = 0x0A, XYZF3 = 0x0C, XYZ3 = 0x0D,
  TEX1_1 = 0x14, TEX1_2 = 0x15, TEX2_1 = 0x16, TEX2_2 = 0x17, XYOFFSET_1 = 0x18, XYOFFSET_2 = 0x19,
  PRMODECONT = 0x1A, PRMODE = 0x1B, TEXCLUT = 0x1C, SCANMSK = 0x22, MIPTBP1_1 = 0x34,
  MIPTBP1_2 = 0x35, MIPTBP2_1 = 0x36, MIPTBP2_2 = 0x37, TEXA = 0x3B, FOGCOL = 0x3D,
  TEXFLUSH = 0x3F, SCISSOR_1 = 0x40, SCISSOR_2 = 0x41, ALPHA_1 = 0x42, ALPHA_2 = 0x43, DIMX = 0x44,
  DTHE = 0x45, COLCLAMP = 0x46, TEST_1 = 0x47, TEST_2 = 0x48, PABE = 0x49, FBA_1 = 0x4A, FBA_2 = 0x4B,
  FRAME_1 = 0x4C, FRAME_2 = 0x4D, ZBUF_1 = 0x4E, ZBUF_2 = 0x4F, BITBLTBUF = 0x50, TRXPOS = 0x51,
  TRXREG = 0x52, TRXDIR = 0x53, HWREG = 0x54, SIGNAL = 0x60, FINISH = 0x61, LABEL = 0x62,
};
enum : uint32_t { kCsrSignal = 1, kCsrFinish = 2 };

inline float f32(uint32_t v) { return std::bit_cast<float>(v); }
}  // namespace


bool Gs::readPriv(uint32_t phys, uint64_t& value) {
  if (phys < 0x12000000 || phys >= 0x12001000) return false;  // CSR/IMR live in Hardware
  value = priv_[((phys - 0x12000000) >> 4) & 15];
  return true;
}

bool Gs::writePriv(uint32_t phys, uint64_t value) {
  if (phys < 0x12000000 || phys >= 0x12001000) return false;
  priv_[((phys - 0x12000000) >> 4) & 15] = value;
  return true;
}

void Gs::writePacked(uint32_t reg, const uint64_t q[2]) {
  switch (reg) {
    case PRIM: writeReg(PRIM, q[0] & 0x7FF); return;
    case RGBAQ: {
      const uint64_t r = q[0] & 0xFF, g = (q[0] >> 32) & 0xFF, b = q[1] & 0xFF, a = (q[1] >> 32) & 0xFF;
      rgbaq_ = r | (g << 8) | (b << 16) | (a << 24) | (rgbaq_ & 0xFFFFFFFF00000000ull);  // Q stays: ST sets it
      return;
    }
    case ST:
      st_ = q[0];
      rgbaq_ = (rgbaq_ & 0xFFFFFFFFull) | (q[1] << 32);  // PACKED ST also sets Q (in RGBAQ), for this vertex
      return;
    case UV: uv_ = (q[0] & 0x3FFF) | (((q[0] >> 32) & 0x3FFF) << 16); return;
    case XYZF2: {
      const uint64_t x = q[0] & 0xFFFF, y = (q[0] >> 32) & 0xFFFF;
      const uint64_t z = (q[1] >> 4) & 0xFFFFFF, f = (q[1] >> 36) & 0xFF;
      writeReg((q[1] >> 47) & 1 ? XYZF3 : XYZF2, x | (y << 16) | (z << 32) | (f << 56));
      return;
    }
    case XYZ2: {
      const uint64_t x = q[0] & 0xFFFF, y = (q[0] >> 32) & 0xFFFF, z = q[1] & 0xFFFFFFFF;
      writeReg((q[1] >> 47) & 1 ? XYZ3 : XYZ2, x | (y << 16) | (z << 32));
      return;
    }
    case FOG: fog_ = ((q[1] >> 36) & 0xFF) << 56; return;
    case 0x0E: writeReg(uint32_t(q[1] & 0xFF), q[0]); return;  // A+D
    case 0x0F: return;                                          // NOP
  }
  writeReg(reg, q[0]);  // TEX0_1 .. XYOFFSET etc. take the low 64 bits as-is
}

void Gs::writeReg(uint32_t reg, uint64_t v) {
  // Vertex data (colour, texture coordinates, position, fog) leaves the drawing state alone.
  if (!((reg >= RGBAQ && reg <= XYZ2) || reg == FOG || reg == XYZF3 || reg == XYZ3)) drawDirty_ = true;
  switch (reg) {
    case PRIM:
      prim_ = v;
      queued_ = 0;
      stripToggle_ = 0;
      return;
    case RGBAQ: rgbaq_ = v; return;
    case ST: st_ = v; return;
    case UV: uv_ = v; return;
    case XYZF2: case XYZF3: case XYZ2: case XYZ3: {
      const bool fogged = reg == XYZF2 || reg == XYZF3;
      if (fogged) {
        fog_ = v & 0xFF00000000000000ull;
        v &= 0x00FFFFFFFFFFFFFFull;
      }
      const Context& c = ctx_[bits(prmodecont_ & 1 ? prim_ : prmode_, 9, 1)];
      Vertex& vx = queue_[std::min(queued_, 2)];
      vx.x = int32_t(bits(v, 0, 16)) - int32_t(bits(c.xyoffset, 0, 16));
      if (uiNarrow_) vx.x = kPictureCentreX + (vx.x - kPictureCentreX) * 3 / 4;
      vx.y = int32_t(bits(v, 16, 16)) - int32_t(bits(c.xyoffset, 32, 16));
      vx.z = uint32_t(v >> 32);
      vx.r = uint8_t(rgbaq_);
      vx.g = uint8_t(rgbaq_ >> 8);
      vx.b = uint8_t(rgbaq_ >> 16);
      vx.a = uint8_t(rgbaq_ >> 24);
      vx.q = f32(uint32_t(rgbaq_ >> 32));
      vx.s = f32(uint32_t(st_));
      vx.t = f32(uint32_t(st_ >> 32));
      vx.u = bits(uv_, 0, 14);
      vx.v = bits(uv_, 16, 14);
      vx.f = uint8_t(fog_ >> 56);
      vertexKick(reg == XYZF2 || reg == XYZ2, fogged);
      return;
    }
    case TEX0_1: case TEX0_2:
      ctx_[reg - TEX0_1].tex0 = v;
      loadClut(v);
      return;
    case CLAMP_1: case CLAMP_2: ctx_[reg - CLAMP_1].clamp = v; return;
    case FOG: fog_ = v; return;
    case TEX1_1: case TEX1_2: ctx_[reg - TEX1_1].tex1 = v; return;
    case TEX2_1: case TEX2_2: {  // TEX0's PSM and palette fields only
      Context& c = ctx_[reg - TEX2_1];
      const uint64_t fields = (0x3Full << 20) | (~0ull << 37);
      c.tex0 = (c.tex0 & ~fields) | (v & fields);
      loadClut(c.tex0);
      return;
    }
    case XYOFFSET_1: case XYOFFSET_2: ctx_[reg - XYOFFSET_1].xyoffset = v; return;
    case PRMODECONT: prmodecont_ = v; return;
    case PRMODE: prmode_ = v; return;
    case TEXCLUT: texclut_ = v; return;
    case SCANMSK: scanmsk_ = v; return;
    case MIPTBP1_1: case MIPTBP1_2: ctx_[reg - MIPTBP1_1].miptbp1 = v; return;
    case MIPTBP2_1: case MIPTBP2_2: ctx_[reg - MIPTBP2_1].miptbp2 = v; return;
    case TEXA: texa_ = v; return;
    case FOGCOL: fogcol_ = v; return;
    case TEXFLUSH: return;
    case SCISSOR_1: case SCISSOR_2: ctx_[reg - SCISSOR_1].scissor = v; return;
    case ALPHA_1: case ALPHA_2: ctx_[reg - ALPHA_1].alpha = v; return;
    case DIMX: dimx_ = v; return;
    case DTHE: dthe_ = v; return;
    case COLCLAMP: colclamp_ = v; return;
    case TEST_1: case TEST_2: ctx_[reg - TEST_1].test = v; return;
    case PABE: pabe_ = v; return;
    case FBA_1: case FBA_2: ctx_[reg - FBA_1].fba = v; return;
    case FRAME_1: case FRAME_2: ctx_[reg - FRAME_1].frame = v; return;
    case ZBUF_1: case ZBUF_2: ctx_[reg - ZBUF_1].zbuf = v; return;
    case BITBLTBUF: bitbltbuf_ = v; return;
    case TRXPOS: trxpos_ = v; return;
    case TRXREG: trxreg_ = v; return;
    case TRXDIR:
      trxdir_ = v;
      startTransfer();
      return;
    case HWREG: writeImage(&v, 1); return;
    case SIGNAL: {  // ID/IDMSK into SIGLBLID, then the SIGNAL event
      siglblid = (siglblid & ~uint64_t(bits(v, 32, 32))) | (v & bits(v, 32, 32));
      csrEvents |= kCsrSignal;
      ++signals;
      return;
    }
    case FINISH:
      csrEvents |= kCsrFinish;
      ++finishes;
      return;
    case LABEL: {
      const uint64_t mask = uint64_t(bits(v, 32, 32)) << 32;
      siglblid = (siglblid & ~mask) | ((v << 32) & mask);
      return;
    }
  }
}

void Gs::setUiLayer(bool on) { uiNarrow_ = on && wideScreen(); }

void Gs::vertexKick(bool drawing, bool) {
  const uint32_t type = bits(prim_, 0, 3);
  static constexpr int kNeeded[8] = {1, 2, 2, 3, 3, 3, 2, 1};
  ++queued_;
  if (queued_ < kNeeded[type]) return;
  // A sprite across the screen is a background or a fade: it stays as wide as the picture.
  if (uiNarrow_ && type == 6 && std::abs(queue_[1].x - queue_[0].x) >= kWholeWidthNarrowed) {
    for (int i = 0; i < 2; ++i) queue_[i].x = kPictureCentreX + (queue_[i].x - kPictureCentreX) * 4 / 3;
  }
  if (drawing) drawPrimitive();
  switch (type) {
    case 0: case 1: case 3: case 6: case 7: queued_ = 0; break;  // point, line, triangle, sprite
    case 2: queue_[0] = queue_[1]; queued_ = 1; break;           // line strip
    case 4: queue_[0] = queue_[1]; queue_[1] = queue_[2]; queued_ = 2; break;  // triangle strip
    case 5: queue_[1] = queue_[2]; queued_ = 2; break;           // triangle fan keeps vertex 0
  }
}

// TEX0/TEX2 writes may load the palette into the CLUT buffer (field CLD decides when).
void Gs::loadClut(uint64_t tex0) {
  const uint32_t cld = bits(tex0, 61, 3), cbp = bits(tex0, 37, 14), psm = bits(tex0, 20, 6);
  bool load = false;
  switch (cld) {
    case 1: load = true; break;
    case 2: load = true; cbp0_ = cbp; break;
    case 3: load = true; cbp1_ = cbp; break;
    case 4: load = cbp != cbp0_; cbp0_ = cbp; break;
    case 5: load = cbp != cbp1_; cbp1_ = cbp; break;
  }
  const bool t8 = psm == PSMT8 || psm == PSMT8H;
  const bool t4 = psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH;
  if (!load || (!t8 && !t4)) return;
  const uint32_t cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1), csa = bits(tex0, 56, 5);
  const uint32_t clutPsm = cpsm == 0 ? PSMCT32 : cpsm == 0xA ? PSMCT16S : PSMCT16;
  const int entries = t8 ? 256 : 16;
  const Range source = csm ? rectRange(clutPsm, cbp, bits(texclut_, 0, 6), bits(texclut_, 6, 6) * 16, bits(texclut_, 12, 10), entries, 1)
                           : rectRange(clutPsm, cbp, 1, 0, 0, t8 ? 16 : 8, t8 ? 16 : 2);
  if (gpu_) {  // a new palette slot, filled on the GPU in order with the drawing
    if (collectedConflict({source}, {})) flush();
    clutSlot_ = gpu_->loadClut(clutSlot_, cbp, clutPsm, csm, csa, t8, texclut_, texa_);
    drawDirty_ = true;
    return;
  }
  // Queued primitives keep the palette they were given; this load fills a new one, right away or
  // (if queued drawing writes its memory, or the current palette is not filled yet) in order.
  auto next = std::make_shared<Clut>();
  auto fill = [this, next, prev = clut_, cbp, clutPsm, entries, csm, csa, t8, texclut = texclut_, texa = texa_] {
    *next = *prev;
    for (int i = 0; i < entries; ++i) {
      uint32_t x, y, bw = 1;
      if (csm == 0) {  // CSM1: 16x16 (bits 3 and 4 of the index swapped) or 8x2 entries
        if (t8) {
          const uint32_t j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
          x = j & 15, y = j >> 4;
        } else {
          x = i & 7, y = i >> 3;
        }
      } else {  // CSM2: one line of a buffer described by TEXCLUT
        bw = bits(texclut, 0, 6);
        x = bits(texclut, 6, 6) * 16 + i;
        y = bits(texclut, 12, 10);
      }
      const uint32_t raw = mem.read(clutPsm, cbp, bw, x, y);
      uint32_t col = raw;
      if (clutPsm != PSMCT32) {
        const bool aem = bits(texa, 15, 1);
        const uint32_t a = (raw & 0x8000) ? bits(texa, 32, 8) : (aem && (raw & 0x7FFF) == 0 ? 0 : bits(texa, 0, 8));
        col = (from16(raw) & 0xFFFFFF) | (a << 24);
      }
      (*next)[t8 ? i : (csa * 16 + i) & 0xFF] = col;
    }
  };
  // (A palette uploaded by a transfer still waiting among the collected steps must wait for it.)
  if (clutInSteps_ || finished_.load() < clutReady_ || mustWait({source}, {})) {
    clutInSteps_ = true;
    if (finished_.load() < clutReady_) stepAfter_ = std::max(stepAfter_, clutReady_);  // fills a copy of that palette
    submit(std::move(fill), {source}, {});
  } else {
    fill();
  }
  clut_ = std::move(next);
  drawDirty_ = true;
}

void Gs::startTransfer() {
  if (trxActive_) submitUpload();  // the previous image ended early
  switch (bits(trxdir_, 0, 2)) {
    case 0: {  // host -> local: collect the image, then write it in order with drawing
      const uint64_t bpp = GsMemory::bitsPerPixel(bits(bitbltbuf_, 56, 6));
      trxNeeded_ = (uint64_t(bits(trxreg_, 0, 12)) * bits(trxreg_, 32, 12) * bpp + 63) / 64;
      trxData_.clear();
      trxActive_ = trxNeeded_ > 0;
      return;
    }
    case 2: {  // local -> local
      if (gpu_) {
        flush();
        gpu_->copy(bitbltbuf_, trxpos_, trxreg_);
        return;
      }
      const uint32_t rrw = bits(trxreg_, 0, 12), rrh = bits(trxreg_, 32, 12);
      const Range src = rectRange(bits(bitbltbuf_, 24, 6), bits(bitbltbuf_, 0, 14), bits(bitbltbuf_, 16, 6),
                                  bits(trxpos_, 0, 11), bits(trxpos_, 16, 11), rrw, rrh);
      const Range dst = rectRange(bits(bitbltbuf_, 56, 6), bits(bitbltbuf_, 32, 14), bits(bitbltbuf_, 48, 6),
                                  bits(trxpos_, 32, 11), bits(trxpos_, 48, 11), rrw, rrh);
      ordered([this, bitbltbuf = bitbltbuf_, trxpos = trxpos_, trxreg = trxreg_] {
        const uint32_t sbp = bits(bitbltbuf, 0, 14), sbw = bits(bitbltbuf, 16, 6), spsm = bits(bitbltbuf, 24, 6);
        const uint32_t dbp = bits(bitbltbuf, 32, 14), dbw = bits(bitbltbuf, 48, 6), dpsm = bits(bitbltbuf, 56, 6);
        const uint32_t ssax = bits(trxpos, 0, 11), ssay = bits(trxpos, 16, 11);
        const uint32_t dsax = bits(trxpos, 32, 11), dsay = bits(trxpos, 48, 11);
        const uint32_t rrw = bits(trxreg, 0, 12), rrh = bits(trxreg, 32, 12);
        std::vector<uint32_t> tmp(size_t(rrw) * rrh);
        for (uint32_t y = 0; y < rrh; ++y)
          for (uint32_t x = 0; x < rrw; ++x) tmp[size_t(y) * rrw + x] = mem.read(spsm, sbp, sbw, (ssax + x) & 2047, (ssay + y) & 2047);
        for (uint32_t y = 0; y < rrh; ++y)
          for (uint32_t x = 0; x < rrw; ++x) mem.write(dpsm, dbp, dbw, (dsax + x) & 2047, (dsay + y) & 2047, tmp[size_t(y) * rrw + x]);
      }, {src}, {dst});
      return;
    }
    default: return;  // local -> host: not used by the game so far
  }
}

void Gs::writeImage(const uint64_t* data, uint32_t count) {
  if (!trxActive_) return;
  const uint64_t take = std::min<uint64_t>(count, trxNeeded_ - trxData_.size());
  trxData_.insert(trxData_.end(), data, data + take);
  if (trxData_.size() == trxNeeded_) submitUpload();
}

void Gs::submitUpload() {
  trxActive_ = false;
  if (gpu_) {  // after the primitives before it
    flush();
    gpu_->upload(bitbltbuf_, trxpos_, trxreg_, trxData_);
    trxData_.clear();
    return;
  }
  const Range dst = rectRange(bits(bitbltbuf_, 56, 6), bits(bitbltbuf_, 32, 14), bits(bitbltbuf_, 48, 6),
                              bits(trxpos_, 32, 11), bits(trxpos_, 48, 11), bits(trxreg_, 0, 12), bits(trxreg_, 32, 12));
  ordered([this, data = std::move(trxData_), bitbltbuf = bitbltbuf_, trxpos = trxpos_, trxreg = trxreg_] {
    const uint32_t dbp = bits(bitbltbuf, 32, 14), dbw = bits(bitbltbuf, 48, 6), dpsm = bits(bitbltbuf, 56, 6);
    const uint32_t dsax = bits(trxpos, 32, 11), dsay = bits(trxpos, 48, 11);
    const uint32_t rrw = bits(trxreg, 0, 12), rrh = bits(trxreg, 32, 12);
    const uint32_t bpp = GsMemory::bitsPerPixel(dpsm);
    uint32_t tx = 0, ty = 0, bitCount = 0;
    uint64_t bitBuf = 0;
    for (uint64_t word : data) {
      uint32_t avail = 64;
      // 24-bit pixels straddle doublewords, so pixels are pulled from a small bit buffer.
      while (avail > 0) {
        const uint32_t take = std::min(avail, 64 - bitCount);
        if (take > 0) {
          const uint64_t part = take == 64 ? word : word & ((1ull << take) - 1);
          bitBuf |= bitCount < 64 ? part << bitCount : 0;
          bitCount += take;
          word = take == 64 ? 0 : word >> take;
          avail -= take;
        }
        while (bitCount >= bpp) {
          if (ty >= rrh) return;
          const uint32_t px = uint32_t(bpp == 32 ? bitBuf : bitBuf & ((1ull << bpp) - 1));
          bitBuf = bpp == 64 ? 0 : bitBuf >> bpp;
          bitCount -= bpp;
          mem.write(dpsm, dbp, dbw, (dsax + tx) & 2047, (dsay + ty) & 2047, px);
          if (++tx == rrw) tx = 0, ++ty;
        }
      }
    }
  }, {}, {dst});
  trxData_.clear();
}

bool Gs::frame(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height) {
  sync();
  if (const char* path = std::getenv("CODPORT_GSMEM")) {  // debugging: GS memory at each --dump picture
    std::vector<uint32_t> words(mem.words(), mem.words() + GsMemory::kSize / 4);
    if (gpu_) gpu_->readMemory(words);
    if (FILE* f = std::fopen(path, "wb")) std::fwrite(words.data(), 4, words.size(), f), std::fclose(f);
  }
  if (gpu_) {
    Display d;
    if (!display(std::to_array(priv_), d)) return false;
    const uint32_t scale = gpu_->picture(d, rgba);
    width = d.width * scale, height = d.height * scale;
    return true;
  }
  return picture(std::to_array(priv_), rgba, width, height);
}

double Gs::gpuBusySeconds() const { return gpu_ ? gpu_->busySeconds() : 0; }

void Gs::frameEnd() {
  if (gpu_) gpu_->frameEnd();
}

void Gs::queueFrame() {
  if (gpu_) {  // shown in the window by the GPU itself
    flush();
    Display d;
    if (display(std::to_array(priv_), d)) gpu_->present(d);
    return;
  }
  // Reads every frame buffer the display circuits may show.
  std::vector<Range> shown;
  for (int reg : {7, 9}) {
    const uint64_t dispfb = priv_[reg];
    shown.push_back(rectRange(bits(dispfb, 15, 5), bits(dispfb, 0, 9) * 32, bits(dispfb, 9, 6), bits(dispfb, 32, 11),
                              bits(dispfb, 43, 11), 1024, 1024));
  }
  ++framesQueued_;
  submit([this, priv = std::to_array(priv_)] {
    std::vector<uint32_t> rgba;
    uint32_t w = 0, h = 0;
    if (picture(priv, rgba, w, h)) {
      std::lock_guard lock(shownMutex_);
      shown_.swap(rgba);
      shownWidth_ = w, shownHeight_ = h;
      ++shownSerial_;
    }
    ++framesDone_;
  }, shown, {});
  flushSteps();
}

bool Gs::latestFrame(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height, uint64_t& serial) {
  std::lock_guard lock(shownMutex_);
  if (shownSerial_ == serial) return false;
  serial = shownSerial_;
  rgba = shown_;
  width = shownWidth_, height = shownHeight_;
  return true;
}

// The picture the display circuits show, from the given privileged registers.
bool Gs::display(const std::array<uint64_t, 16>& priv, Display& out) {
  const uint64_t pmode = priv[0], smode2 = priv[2];
  const bool en1 = pmode & 1, en2 = (pmode >> 1) & 1;
  if (!en1 && !en2) return false;
  const int circuit = en2 ? 2 : 1;  // the game's picture is normally on circuit 2 when both are on
  const uint64_t dispfb = priv[circuit == 1 ? 7 : 9], display = priv[circuit == 1 ? 8 : 10];
  out.fbp = bits(dispfb, 0, 9) * 32, out.fbw = bits(dispfb, 9, 6), out.psm = bits(dispfb, 15, 5);
  out.dbx = bits(dispfb, 32, 11), out.dby = bits(dispfb, 43, 11);
  const uint32_t magh = bits(display, 23, 4) + 1, magv = bits(display, 27, 2) + 1;
  const uint32_t dw = bits(display, 32, 12) + 1, dh = bits(display, 44, 11) + 1;
  out.width = std::max(1u, dw / magh);
  uint32_t lines = std::max(1u, dh / magv);
  out.field = (smode2 & 3) == 3;
  if (out.field) lines = std::max(1u, lines / 2);
  out.height = out.field ? lines * 2 : lines;
  return true;
}

bool Gs::picture(const std::array<uint64_t, 16>& priv, std::vector<uint32_t>& rgba, uint32_t& width,
                 uint32_t& height) const {
  Display disp;
  if (!display(priv, disp)) return false;
  const uint32_t fbp = disp.fbp, fbw = disp.fbw, psm = disp.psm, dbx = disp.dbx, dby = disp.dby;
  const bool fieldMode = disp.field;
  width = disp.width, height = disp.height;
  rgba.assign(size_t(width) * height, 0);
  const bool fb16 = psm == PSMCT16 || psm == PSMCT16S, fb32 = psm == PSMCT32 || psm == PSMCT24;
  const uint32_t* words = mem.words();
  const uint16_t* halves = reinterpret_cast<const uint16_t*>(words);
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t sy = dby + (fieldMode ? y / 2 : y);
    uint32_t* out = rgba.data() + size_t(y) * width;
    if (fb32 || fb16) {  // a row's addresses at a time
      const GsMemory::Row row = fb16 ? GsMemory::row16(fbp, fbw, sy, psm) : GsMemory::row32(fbp, fbw, sy);
      for (uint32_t x = 0; x < width; ++x)
        out[x] = (fb16 ? from16(halves[row.at(dbx + x)]) : words[row.at(dbx + x)]) | 0xFF000000;
      continue;
    }
    for (uint32_t x = 0; x < width; ++x) out[x] = mem.read(psm, fbp, fbw, dbx + x, sy) | 0xFF000000;
  }
  return true;
}
