#pragma once
// A primitive as the GS front end hands it to a renderer: its decoded drawing state (Draw) and
// vertices. Shared by the software rasterizer (raster.cpp) and the GPU one (gpu.cpp).
#include "gs/gs.h"

struct Gs::Draw {
  // Frame and depth buffers.
  uint32_t fbp, fbw, fpsm, fbmsk;
  bool fb16, fb24;
  uint32_t zbp, zpsm, zmax;
  bool ztest, zwrite;
  uint32_t ztst;
  // Tests.
  bool ate, date, datm;
  uint32_t atst, afail;
  int aref;
  // Blending: ((A - B) * C >> 7) + D, selections 0 = source, 1 = destination, 2 = zero/FIX.
  bool blend, pabe, fba, colclamp;
  int ba, bb, bc, bd, fix;
  // Texture.
  bool tme, fst, tcc, aem;
  bool linear;  // the GPU renderer filters it (TEX1, and Gs::setTextureFilter)
  uint32_t tbp, tbw, tpsm, tfx, wms, wmt, minu, maxu, minv, maxv, ta0, ta1, csa;
  int tw, th;
  // Fog.
  bool fge;
  int fogR, fogG, fogB;
  // Scissor.
  int sx0, sx1, sy0, sy1;
  bool iip;             // Gouraud shading
  // No texture, fog, blending or destination alpha test, and the depth test always passes: a
  // span of one colour and depth is a fill (fillSpan).
  bool fill;
  // The texture overlaps the frame or depth buffer (texFeedback): pixels of a span that read
  // what earlier ones of the span write go one at a time. The frame and depth buffers overlap
  // (selfRead): all of them do.
  bool texFeedback, selfRead;
  const uint32_t* clut;  // palette snapshot
  uint32_t clutSlot;     // ... its slot on the GPU (GsGpu)
  PlotFn plot;           // plotT for this primitive's features
  SpanFn span;           // spanT for them
};

struct Gs::Command {
  Draw d;
  Vertex v[3];
  uint32_t type;
};
