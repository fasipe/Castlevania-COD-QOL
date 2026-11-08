#include "game/widescreen.h"

#include "ee/cpu.h"
#include "game/hooks.h"
#include "gs/graphics.h"
#include "host/window.h"
#include "recomp/runtime.h"

namespace {
// The function that sets up the 3D projection (FUN_00752698, called from 0x00772A70) gets the
// horizontal scale in f13; three quarters of it shows a 16:9 field of view on a 16:9 screen
// (the patch multiplies its copy, f24, by 0.75 at 0x007526FC).
constexpr uint32_t kProjection = 0x00752698;
// The function that places the movie picture on the screen (FUN_00775328, a0..a3 in 1/16
// pixels). For 16:9 the patch skips the call that fits movies to the screen (0x004448D0) and
// changes a1 and a3 of the default movie rectangle (0x00776718/24).
constexpr uint32_t kMovieRect = 0x00775328;
constexpr uint32_t kFitMovieReturn = 0x004448D8, kDefaultMovieReturn = 0x00776728;

// The game draws in six layers, one DMA chain from layer to layer (FUN_00432478 ends and sends
// them); the 2D of its interface is layer 4. Each layer's start is in 0x00ADF1F0, 16 bytes apart.
constexpr uint32_t kSendLayers = 0x00435D40, kLayerStarts = 0x00ADF1F0;
// The status display's update (HP and so on) runs in play and in scenes, not on full-screen menus,
// the map screen, the title: those are built of pieces across the whole picture, and stay as they are.
constexpr uint32_t kStatusUpdate = 0x005012A0;
uint64_t gStatusUpdates = 0;
uint64_t gUpdatesAtLastFrame = 0;
int gFramesWithoutStatus = 1000;

Cpu::RecompFn gProjection = nullptr, gMovieRect = nullptr, gSendLayers = nullptr, gStatusUpdate = nullptr;

uint32_t projection(Cpu& c, uint32_t entry) {
  if (wideScreen()) c.s.fpr[13] *= 0.75f;
  return gProjection(c, entry);
}

uint32_t statusUpdate(Cpu& c, uint32_t entry) {
  ++gStatusUpdates;
  return gStatusUpdate(c, entry);
}

uint32_t sendLayers(Cpu& c, uint32_t entry) {
  gFramesWithoutStatus = gStatusUpdates != gUpdatesAtLastFrame ? 0 : gFramesWithoutStatus + 1;
  gUpdatesAtLastFrame = gStatusUpdates;
  c.mem.gfx->setUiLayerChain(c.mem.read<uint32_t>(kLayerStarts + 4 * 16), c.mem.read<uint32_t>(kLayerStarts + 5 * 16), gFramesWithoutStatus <= 2);
  return gSendLayers(c, entry);
}

uint32_t movieRect(Cpu& c, uint32_t entry) {
  if (wideScreen()) {
    const uint32_t ra = uint32_t(c.s.gpr[31].ud[0]);
    if (ra == kFitMovieReturn) return rc::leave(c, 0, ra);  // keep the 4:3 rectangle set before
    if (ra == kDefaultMovieReturn) {
      c.s.gpr[5].ud[0] = rc::sx32(0x6D80);
      c.s.gpr[7].ud[0] = rc::sx32(0x2380);
    }
  }
  return gMovieRect(c, entry);
}

}  // namespace

uint64_t statusDisplayUpdates() { return gStatusUpdates; }

void installWidescreen(Cpu& cpu) {
  Cpu::RecompFn& p = recompSlot(cpu, kProjection);
  Cpu::RecompFn& m = recompSlot(cpu, kMovieRect);
  if (!p || !m) return;
  gProjection = p, p = projection;
  gMovieRect = m, m = movieRect;
  Cpu::RecompFn& s = recompSlot(cpu, kSendLayers);
  if (s) gSendLayers = s, s = sendLayers;
  Cpu::RecompFn& u = recompSlot(cpu, kStatusUpdate);
  if (u) gStatusUpdate = u, u = statusUpdate;
}
