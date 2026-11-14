#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gs/gs.h"

// The GS on the graphics card (Direct3D 11). GS local memory lives in a GPU buffer and the GPU
// does what the software rasterizer does, on that memory, in the same order: a pixel shader runs
// the GS pixel pipeline (texture, fog, tests, blending, masks) for every covered pixel, reading
// and writing the buffer through a rasterizer-ordered view, so overlapping primitives of one draw
// call apply in submission order as on the GS. Transfers, copies and palette loads are compute
// shaders on the same buffer, and draw calls follow the front end's batches, so everything keeps
// the order the game gave it. Since memory keeps the GS's own layout, render-to-texture,
// palettes and format reinterpretation work as on the console.
//
// All calls come from one thread at a time (the GS thread, or the EE thread while it is idle).
class GsGpu {
public:
  // Null (with `error` set) if the graphics card cannot do it. `window` (an HWND) is where
  // present() shows pictures; null for none. `scale` (1-4) multiplies the drawing resolution:
  // pictures in the window are that much sharper, read-backs stay at the GS's resolution.
  static std::unique_ptr<GsGpu> create(void* window, uint32_t scale, std::string& error);
  ~GsGpu();

  void draw(const std::vector<Gs::Command>& commands);  // one batch, in order
  // Host -> local transfer of `data` (64-bit words) and local -> local copy, as the registers say.
  void upload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const std::vector<uint64_t>& data);
  void copy(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
  // Fills a new palette slot: `previous` with the entries the load replaces (see Gs::loadClut).
  uint32_t loadClut(uint32_t previous, uint32_t cbp, uint32_t clutPsm, uint32_t csm, uint32_t csa, bool t8,
                    uint64_t texclut, uint64_t texa);
  // In the window, letterboxed (4:3, or 16:9 with F10); `stamp`: the VBlank's time (steady clock,
  // ns), or 0. With interpolation the picture is shown by tick() instead.
  void present(const Gs::Display& display, uint64_t stamp = 0);
  // Interpolation: `wake` is called (from a thread of the GPU's) at each refresh of the screen
  // while it is on, and tick() then shows the picture of that moment. Empty: stops.
  void onRefresh(std::function<void()> wake);
  void tick();
  // Where the game's camera looks in the next picture (radians), or NaN: interpolation follows its
  // turn; and whether the game is being played in it: menus, movies and the like are not interpolated.
  void setView(float yaw, float pitch, bool playing);
  void frameEnd();  // the end of a frame's drawing (GPU time per frame with CODPORT_GPU_TIME set)
  double busySeconds() const;  // the graphics card's working time so far (frame by frame, with a window)
  // The displayed picture read back (RGBA, as Gs::frame gives it), at the GS's resolution, or with
  // CODPORT_DUMP_SCALED set at the drawing resolution; returns the scale it used.
  uint32_t picture(const Gs::Display& display, std::vector<uint32_t>& rgba);
  void readMemory(std::vector<uint32_t>& words);  // GS memory read back (for debugging, and a new scale)
  uint32_t scale() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
