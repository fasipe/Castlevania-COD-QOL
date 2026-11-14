#pragma once
// HLSL for GsGpu (gpu.cpp), compiled when the renderer starts. kCommon is prepended to each
// shader; MEM names the GS memory view the shader declares, WRITABLE marks views it may write.
// SCALE (defined by gpu.cpp) is the resolution scale: GS memory then holds SCALE x SCALE copies
// ("layers"), one per sub-pixel position, so a pixel drawn at the higher resolution keeps all its
// samples and render targets reused as textures keep their detail; gLayer picks the copy.

namespace gpu_hlsl {

inline constexpr const char* kCommon = R"(
#define PSMCT32 0x00
#define PSMCT24 0x01
#define PSMCT16 0x02
#define PSMCT16S 0x0A
#define PSMT8 0x13
#define PSMT4 0x14
#define PSMT8H 0x1B
#define PSMT4HL 0x24
#define PSMT4HH 0x2C
#define PSMZ32 0x30
#define PSMZ24 0x31
#define PSMZ16 0x32
#define PSMZ16S 0x3A
#define MEM_WORDS 1048576u
#define LAYER_BYTES 4194304u
#define LAYERS (SCALE * SCALE)
static uint gLayer = 0;
uint memAt(uint w) { return gLayer * LAYER_BYTES + (w << 2); }  // byte address of word w in the layer

// Swizzle tables (GsMemory's kSwizzle): 16-bit entries, t32 t32z t16 t16s t16z t16sz t8 t4.
ByteAddressBuffer swz : register(t1);
uint swizzle(uint e) {
  uint w = swz.Load((e >> 1) << 2);
  return (e & 1) ? (w >> 16) : (w & 0xFFFF);
}
uint addr32(uint bp, uint bw, uint x, uint y, bool z) {
  uint page = (x >> 6) + (y >> 5) * bw;
  uint t = swizzle((z ? 2048u : 0u) + (y & 31) * 64 + (x & 63));
  return ((bp + page * 32) * 64 + t) & (MEM_WORDS - 1);
}
uint addr16(uint bp, uint bw, uint x, uint y, uint psm) {
  uint page = (x >> 6) + (y >> 6) * bw;
  uint table = psm == PSMCT16 ? 4096u : psm == PSMCT16S ? 8192u : psm == PSMZ16 ? 12288u : 16384u;
  uint t = swizzle(table + (y & 63) * 64 + (x & 63));
  return ((bp + page * 32) * 128 + t) & (MEM_WORDS * 2 - 1);
}
uint addr8(uint bp, uint bw, uint x, uint y) {
  uint page = (x >> 7) + (y >> 6) * (bw >> 1);
  uint t = swizzle(20480u + (y & 63) * 128 + (x & 127));
  return ((bp + page * 32) * 256 + t) & (MEM_WORDS * 4 - 1);
}
uint addr4(uint bp, uint bw, uint x, uint y) {
  uint page = (x >> 7) + (y >> 7) * (bw >> 1);
  uint t = swizzle(28672u + (y & 127) * 128 + (x & 127));
  return ((bp + page * 32) * 512 + t) & (MEM_WORDS * 8 - 1);
}

uint load32(uint w) { return MEM.Load(memAt(w)); }
uint load16(uint h) { return (MEM.Load(memAt(h >> 1)) >> ((h & 1) * 16)) & 0xFFFF; }
uint load8(uint b) { return (MEM.Load(memAt(b >> 2)) >> ((b & 3) * 8)) & 0xFF; }
uint load4(uint n) { return (load8(n >> 1) >> ((n & 1) * 4)) & 0xF; }

// GsMemory::read: the raw pixel value in the format's own width.
uint readPixel(uint psm, uint bp, uint bw, uint x, uint y) {
  switch (psm) {
    case PSMCT32: case PSMZ32: return load32(addr32(bp, bw, x, y, psm == PSMZ32));
    case PSMCT24: case PSMZ24: return load32(addr32(bp, bw, x, y, psm == PSMZ24)) & 0xFFFFFF;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return load16(addr16(bp, bw, x, y, psm));
    case PSMT8: return load8(addr8(bp, bw, x, y));
    case PSMT4: return load4(addr4(bp, bw, x, y));
    case PSMT8H: return load32(addr32(bp, bw, x, y, false)) >> 24;
    case PSMT4HL: return (load32(addr32(bp, bw, x, y, false)) >> 24) & 0xF;
    case PSMT4HH: return load32(addr32(bp, bw, x, y, false)) >> 28;
  }
  return 0;
}

#ifdef WRITABLE
// Several threads may write different bits of one word at once: those writes are atomic.
void storeBits(uint w, uint mask, uint v) {
  if (mask == 0xFFFFFFFF) {
    MEM.Store(memAt(w), v);
  } else {
    MEM.InterlockedAnd(memAt(w), ~mask);
    MEM.InterlockedOr(memAt(w), v & mask);
  }
}
// GsMemory::write.
void writePixel(uint psm, uint bp, uint bw, uint x, uint y, uint v) {
  switch (psm) {
    case PSMCT32: case PSMZ32: storeBits(addr32(bp, bw, x, y, psm == PSMZ32), 0xFFFFFFFF, v); return;
    case PSMCT24: case PSMZ24: storeBits(addr32(bp, bw, x, y, psm == PSMZ24), 0xFFFFFF, v); return;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
      uint h = addr16(bp, bw, x, y, psm), sh = (h & 1) * 16;
      storeBits(h >> 1, 0xFFFFu << sh, v << sh);
      return;
    }
    case PSMT8: {
      uint b = addr8(bp, bw, x, y), sh = (b & 3) * 8;
      storeBits(b >> 2, 0xFFu << sh, v << sh);
      return;
    }
    case PSMT4: {
      uint n = addr4(bp, bw, x, y), sh = ((n >> 1) & 3) * 8 + (n & 1) * 4;
      storeBits(n >> 3, 0xFu << sh, v << sh);
      return;
    }
    case PSMT8H: storeBits(addr32(bp, bw, x, y, false), 0xFF000000, v << 24); return;
    case PSMT4HL: storeBits(addr32(bp, bw, x, y, false), 0x0F000000, v << 24); return;
    case PSMT4HH: storeBits(addr32(bp, bw, x, y, false), 0xF0000000, v << 28); return;
  }
}
#endif

// Whether the GS memory page of a frame or depth buffer pixel is marked in a page bit set.
bool pageMarked(uint4 pages[4], uint psm, uint bp, uint bw, uint x, uint y) {
  uint page;
  switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: page = addr16(bp, bw, x, y, psm) >> 12; break;
    default: page = addr32(bp, bw, x, y, psm == PSMZ32 || psm == PSMZ24) >> 11; break;
  }
  page &= 511;
  return ((pages[page >> 7][(page >> 5) & 3] >> (page & 31)) & 1) != 0;
}

uint from16(uint c) {
  return ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19) | ((c & 0x8000) ? 0x80000000u : 0u);
}
uint to16(uint c) {
  return ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 31) << 15);
}
)";

// Draws: positions in pixels (the GS's, shifted half a pixel so pixel centres sample the GS's
// integer positions), and the primitive each vertex belongs to.
inline constexpr const char* kDrawVs = R"(
struct VOut {
  float4 pos : SV_Position;
  nointerpolation uint prim : PRIM;
};
VOut main(float2 p : POSITION, uint prim : PRIM) {
  VOut o;
  o.pos = float4(p.x / (1024.0 * SCALE) - 1.0, 1.0 - p.y / (1024.0 * SCALE), 0.0, 1.0);
  o.prim = prim;
  return o;
}
)";

// What both drawing paths share: the primitive's state and attribute planes, texturing.
inline constexpr const char* kDrawShared = R"(
struct Prim {
  uint flags, fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmax, ztst, atst, afail;
  int aref;
  uint blendSel;
  int fix;
  uint tbp, tbw, tpsm, tfx, wms, wmt, minu, maxu, minv, maxv, ta0, ta1, csa;
  int tw, th, fogR, fogG, fogB, sx0, sx1, sy0, sy1;
  uint clutSlot, flat, zConst;
  float fx0, fy0;
#ifndef SURFACE_TEXTURE
  float pl[33];  // planes (base, d/dx, d/dy) of z r g b a s t q u v f
#endif
};
#ifndef SURFACE_TEXTURE
StructuredBuffer<Prim> prims : register(t0);
#endif
StructuredBuffer<uint> cluts : register(t2);

#define F_FB16 (1u << 0)
#define F_FB24 (1u << 1)
#define F_ZTEST (1u << 2)
#define F_ZWRITE (1u << 3)
#define F_ATE (1u << 4)
#define F_DATE (1u << 5)
#define F_DATM (1u << 6)
#define F_BLEND (1u << 7)
#define F_PABE (1u << 8)
#define F_FBA (1u << 9)
#define F_COLCLAMP (1u << 10)
#define F_TME (1u << 11)
#define F_FST (1u << 12)
#define F_TCC (1u << 13)
#define F_AEM (1u << 14)
#define F_FGE (1u << 15)
#define F_IIP (1u << 16)
#define F_ZCONST (1u << 17)
#define F_LINEAR (1u << 18)
#define F_CACHED (1u << 19)  // filtered by the GPU from the texture decoded once (GsGpu::Impl::decoded)

#ifdef CACHED_TEXTURES
Texture2D<float4> cachedTex : register(t6);
SamplerState cachedSampler : register(s0);
#endif

#ifndef SURFACE_TEXTURE
float plane(Prim p, uint k, float rx, float ry) { return p.pl[3 * k] + p.pl[3 * k + 1] * rx + p.pl[3 * k + 2] * ry; }

#endif

int floorToInt(float f) {
  int i = int(f);
  return i - (float(i) > f ? 1 : 0);
}

int wrapCoord(int c, int size, uint mode, uint mn, uint mx) {
  if (mode == 0) return c & (size - 1);
  if (mode == 1) return clamp(c, 0, size - 1);
  if (mode == 2) return clamp(c, int(mn), int(mx));
  return int((uint(c) & mn) | mx);
}

uint fetchTexel(Prim p, int u, int v) {
  uint x = uint(wrapCoord(u, p.tw, p.wms, p.minu, p.maxu));
  uint y = uint(wrapCoord(v, p.th, p.wmt, p.minv, p.maxv));
  uint base = p.clutSlot * 256;
#ifdef SURFACE_TEXTURE
  if (texFromSurface == 2) {  // a count surface: its channels are the 16-bit texel's, wrapped
    const uint4 n = uint4(int4(round(texSurface.Load(int3(x * SCALE + gLayer % SCALE, y * SCALE + gLayer / SCALE, 0)))) & int4(31, 31, 31, 1));
    const uint raw = n.r | (n.g << 5) | (n.b << 10) | (n.a << 15);
    const uint alpha = (raw & 0x8000) ? p.ta1 : (((p.flags & F_AEM) && (raw & 0x7FFF) == 0) ? 0u : p.ta0);
    return (from16(raw) & 0xFFFFFF) | (alpha << 24);
  }
  if (texFromSurface != 0 && (p.tpsm == PSMCT32 || p.tpsm == PSMCT24)) {  // the sample of the texel's layer
    uint4 c = uint4(saturate(texSurface.Load(int3(x * SCALE + gLayer % SCALE, y * SCALE + gLayer / SCALE, 0))) * 255.0 + 0.5);
    uint raw = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
    if (p.tpsm == PSMCT32) return raw;
    raw &= 0xFFFFFF;
    return raw | (((p.flags & F_AEM) && raw == 0) ? 0u : p.ta0 << 24);
  }
#endif
  switch (p.tpsm) {
    case PSMCT32: return load32(addr32(p.tbp, p.tbw, x, y, false));
    case PSMCT24: {
      uint raw = load32(addr32(p.tbp, p.tbw, x, y, false)) & 0xFFFFFF;
      return raw | (((p.flags & F_AEM) && raw == 0) ? 0u : p.ta0 << 24);
    }
    case PSMCT16: case PSMCT16S: {
      uint raw = load16(addr16(p.tbp, p.tbw, x, y, p.tpsm));
      uint a = (raw & 0x8000) ? p.ta1 : (((p.flags & F_AEM) && (raw & 0x7FFF) == 0) ? 0u : p.ta0);
      return (from16(raw) & 0xFFFFFF) | (a << 24);
    }
    case PSMT8: return cluts[base + load8(addr8(p.tbp, p.tbw, x, y))];
    case PSMT4: return cluts[base + ((p.csa + load4(addr4(p.tbp, p.tbw, x, y))) & 0xFF)];
    case PSMT8H: return cluts[base + (load32(addr32(p.tbp, p.tbw, x, y, false)) >> 24)];
    case PSMT4HL: return cluts[base + ((p.csa + ((load32(addr32(p.tbp, p.tbw, x, y, false)) >> 24) & 0xF)) & 0xFF)];
    case PSMT4HH: return cluts[base + ((p.csa + (load32(addr32(p.tbp, p.tbw, x, y, false)) >> 28)) & 0xFF)];
  }
  return 0;
}

int floorDiv(int a, int b) { return (a >= 0 ? a : a - b + 1) / b; }

// A texel of a picture of `grid` samples per GS texel.
uint fetchSample(Prim p, int au, int av, int grid) {
  if (grid == 1) return fetchTexel(p, au, av);
  const int tu = floorDiv(au, grid), tv = floorDiv(av, grid);
  gLayer = uint(av - tv * grid) * SCALE + uint(au - tu * grid);
  return fetchTexel(p, tu, tv);
}

// The GS's linear filter: the four texels around a coordinate half a texel back, weighed in
// sixteenths. (One fetch per sample chosen by its place in the pixel, which costs much less at 4x,
// turned texel edges into saw teeth on screen.) A picture drawn on
// a surface holds SCALE samples per texel: it is filtered between those, each where the GS's texel
// would be (half a texel back, not half a sample), so that one copied pixel for pixel stays sharp.
// Other textures keep the caller's layer.
int4 filteredTexel(Prim p, float fu, float fv, uint layer) {
  int grid = 1;
#ifdef SURFACE_TEXTURE
  if (texFromSurface == 2 || (texFromSurface != 0 && (p.tpsm == PSMCT32 || p.tpsm == PSMCT24))) grid = SCALE;
#endif
  const float cu = (fu - 0.5) * float(grid), cv = (fv - 0.5) * float(grid);
  const int au = floorToInt(cu * 16.0), av = floorToInt(cv * 16.0);
  const int wu = au & 15, wv = av & 15;
  int4 sum = int4(0, 0, 0, 0);
  [loop] for (int n = 0; n < 4; ++n) {
    const int ou = n & 1, ov = n >> 1;
    const int weight = (ou != 0 ? wu : 16 - wu) * (ov != 0 ? wv : 16 - wv);
    if (weight == 0) continue;
    const uint c = fetchSample(p, (au >> 4) + ou, (av >> 4) + ov, grid);
    sum += int4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) * weight;
  }
  return sum >> 8;
}

#ifdef CACHED_TEXTURES
// filteredTexel for a texture in a region mode (REGION_CLAMP, REGION_REPEAT), which the GPU's
// sampler cannot do: the same four texels and weights, read from the decoded texture rather than
// decoded from GS memory each time (the smoke in front of the giant, 2x2 per pixel at 4x).
int4 cachedRegionTexel(Prim p, float fu, float fv) {
  const int au = floorToInt((fu - 0.5) * 16.0), av = floorToInt((fv - 0.5) * 16.0);
  const int wu = au & 15, wv = av & 15;
  int4 sum = int4(0, 0, 0, 0);
  [loop] for (int n = 0; n < 4; ++n) {
    const int ou = n & 1, ov = n >> 1;
    const int weight = (ou != 0 ? wu : 16 - wu) * (ov != 0 ? wv : 16 - wv);
    if (weight == 0) continue;
    const int x = wrapCoord((au >> 4) + ou, p.tw, p.wms, p.minu, p.maxu), y = wrapCoord((av >> 4) + ov, p.th, p.wmt, p.minv, p.maxv);
    sum += int4(round(cachedTex.Load(int3(x, y, 0)) * 255.0)) * weight;
  }
  return sum >> 8;
}
#endif

// uint(z) for 0 <= z < 2^32, from double operations every D3D11 device with doubles has.
uint doubleToUint(double z) {
  double hiD = z * (1.0 / 65536.0);
  uint hi = uint(float(hiD));
  double lo = z - double(hi) * 65536.0;
  if (lo < 0.0) { hi -= 1; lo += 65536.0; }
  if (lo >= 65536.0) { hi += 1; lo -= 65536.0; }
  uint li = uint(float(lo));
  if (double(li) > lo) li -= 1;
  return (hi << 16) + li;
}


// The sample's depth, from the plane as Gs::drawTriangle sets it up.
#ifndef SURFACE_TEXTURE
uint depthAt(Prim p, float rx, float ry) {
  if (p.flags & F_ZCONST) return p.zConst;
  double zz = double(p.pl[0]) + double(p.pl[1]) * double(rx) + double(p.pl[2]) * double(ry);
  zz = max(zz, 0.0);
  zz = min(zz, 4294967040.0);
  return doubleToUint(zz);
}

#endif

// The colour after texturing and fog, from the sample's interpolated values: colour, S T Q,
// U V (10.4 fixed point) and fog; `layer` is the sample's own layer.
int4 shadeWith(Prim p, int4 c, float s, float t, float q, float u, float v, float fog, uint layer) {
  uint fl = p.flags;
  int r = c.r, g = c.g, b = c.b, a = c.a;
  if (fl & F_TME) {
    int tu, tv, su, sv;  // the texel, and which of its sub-texels (the texture's layer)
    float tx, ty;        // the coordinate, in texels
    if (fl & F_FST) {
      int fu = int(u), fv = int(v);
      tu = fu >> 4, tv = fv >> 4;
      su = ((fu & 15) * SCALE) >> 4, sv = ((fv & 15) * SCALE) >> 4;
      tx = u / 16.0, ty = v / 16.0;
    } else {
      float inv = q != 0.0 ? 1.0 / q : 1.0;
      float fu = s * inv * float(p.tw), fv = t * inv * float(p.th);
      tu = floorToInt(fu), tv = floorToInt(fv);
      su = min(SCALE - 1, int((fu - float(tu)) * SCALE)), sv = min(SCALE - 1, int((fv - float(tv)) * SCALE));
      tx = fu, ty = fv;
    }
    gLayer = uint(sv) * SCALE + uint(su);
    int tr, tg, tb, ta;
    if (fl & F_LINEAR) {
      int4 tf;
#ifdef CACHED_TEXTURES
      if (fl & F_CACHED)  // the GPU's filter samples half a texel back too (not in the region modes)
        tf = p.wms > 1 || p.wmt > 1 ? cachedRegionTexel(p, tx, ty)
                                    : int4(round(cachedTex.SampleLevel(cachedSampler, float2(tx / float(p.tw), ty / float(p.th)), 0) * 255.0));
      else
#endif
        tf = filteredTexel(p, tx, ty, layer);
      tr = tf.r, tg = tf.g, tb = tf.b, ta = tf.a;
    } else {
      const uint tc = fetchTexel(p, tu, tv);
      tr = int(tc & 0xFF), tg = int((tc >> 8) & 0xFF), tb = int((tc >> 16) & 0xFF), ta = int(tc >> 24);
    }
    gLayer = layer;
    bool tcc = (fl & F_TCC) != 0;
    if (p.tfx == 0) {  // MODULATE
      r = min(255, (tr * r) >> 7), g = min(255, (tg * g) >> 7), b = min(255, (tb * b) >> 7);
      if (tcc) a = min(255, (ta * a) >> 7);
    } else if (p.tfx == 1) {  // DECAL
      r = tr, g = tg, b = tb;
      if (tcc) a = ta;
    } else {  // HIGHLIGHT, HIGHLIGHT2
      r = min(255, ((tr * r) >> 7) + a), g = min(255, ((tg * g) >> 7) + a), b = min(255, ((tb * b) >> 7) + a);
      if (tcc) a = p.tfx == 2 ? min(255, ta + a) : ta;
    }
  }
  if (fl & F_FGE) {
    int f = int(clamp(fog, 0.0, 255.0));
    r = (f * r + (255 - f) * p.fogR) >> 8;
    g = (f * g + (255 - f) * p.fogG) >> 8;
    b = (f * b + (255 - f) * p.fogB) >> 8;
  }
  return int4(r, g, b, a);
}

#ifndef SURFACE_TEXTURE
// The same from the primitive's attribute planes at (rx, ry) from its origin.
int4 shade(Prim p, float rx, float ry, uint layer) {
  int4 c;
  if (p.flags & F_IIP) {
    c = int4(clamp(int(plane(p, 1, rx, ry) + 0.5), 0, 255), clamp(int(plane(p, 2, rx, ry) + 0.5), 0, 255),
             clamp(int(plane(p, 3, rx, ry) + 0.5), 0, 255), clamp(int(plane(p, 4, rx, ry) + 0.5), 0, 255));
  } else {
    c = int4(p.flat & 0xFF, (p.flat >> 8) & 0xFF, (p.flat >> 16) & 0xFF, p.flat >> 24);
  }
  return shadeWith(p, c, plane(p, 5, rx, ry), plane(p, 6, rx, ry), plane(p, 7, rx, ry), plane(p, 8, rx, ry),
                   plane(p, 9, rx, ry), plane(p, 10, rx, ry), layer);
}

#endif

bool alphaPasses(Prim p, int a) {
  switch (p.atst) {
    case 0: return false;
    case 1: return true;
    case 2: return a < p.aref;
    case 3: return a <= p.aref;
    case 4: return a == p.aref;
    case 5: return a >= p.aref;
    case 6: return a > p.aref;
  }
  return a != p.aref;
}
)";

// The GS pixel pipeline (Gs::plotT), on GS memory through a rasterizer-ordered view: kDrawShared
// comes first.
inline constexpr const char* kDrawPs = R"(
// A 16-bit pixel: the other half of its word may belong to another pixel being drawn at once.
void store16(uint h, uint v) {
  uint sh = (h & 1) * 16, a = memAt(h >> 1);
  MEM.InterlockedAnd(a, ~(0xFFFFu << sh));
  MEM.InterlockedOr(a, (v & 0xFFFF) << sh);
}

void main(float4 pos : SV_Position, nointerpolation uint id : PRIM) {
  Prim p = prims[id];
  // A sample of the SCALE-times larger picture: the GS pixel it is in, and its layer.
  int sxs = int(pos.x), sys = int(pos.y);
  int x = sxs / SCALE, y = sys / SCALE;
  if (x < p.sx0 || x > p.sx1 || y < p.sy0 || y > p.sy1) return;
  const uint layer = uint(sys % SCALE) * SCALE + uint(sxs % SCALE);
  gLayer = layer;
  uint fl = p.flags;
  float rx = float(sxs) / SCALE - p.fx0, ry = float(sys) / SCALE - p.fy0;

  uint z = depthAt(p, rx, ry);

  bool z16 = p.zpsm == PSMZ16 || p.zpsm == PSMZ16S;
  uint zIndex = 0;
  if (fl & F_ZTEST) {
    z = min(z, p.zmax);
    zIndex = z16 ? addr16(p.zbp, p.fbw, x, y, p.zpsm) : addr32(p.zbp, p.fbw, x, y, true);
    if (p.ztst == 0) return;
    if (p.ztst >= 2) {
      uint zd = z16 ? load16(zIndex) : p.zpsm == PSMZ24 ? (load32(zIndex) & 0xFFFFFF) : load32(zIndex);
      if (p.ztst == 2 ? z < zd : z <= zd) return;
    }
  }

  const int4 c = shade(p, rx, ry, layer);
  int r = c.r, g = c.g, b = c.b, a = c.a;

  bool writeFb = true, writeZ = (fl & F_ZWRITE) != 0, writeAlpha = true;
  if (fl & F_ATE) {
    if (!alphaPasses(p, a)) {
      if (p.afail == 0) return;
      if (p.afail == 1) writeZ = false;
      else if (p.afail == 2) writeFb = false;
      else writeZ = false, writeAlpha = false;
    }
  }

  bool fb16 = (fl & F_FB16) != 0, fb24 = (fl & F_FB24) != 0;
  uint fbIndex = fb16 ? addr16(p.fbp, p.fbw, x, y, p.fpsm) : addr32(p.fbp, p.fbw, x, y, false);
  uint dst = fb16 ? load16(fbIndex) : load32(fbIndex);
  uint dst32 = fb16 ? from16(dst) : dst;
  if ((fl & F_DATE) && (dst32 >> 31) != ((fl & F_DATM) ? 1u : 0u)) return;

  if ((fl & F_BLEND) && (!(fl & F_PABE) || (a & 0x80))) {
    int ad = fb24 ? 0x80 : int(dst32 >> 24);
    uint ba = p.blendSel & 3, bb = (p.blendSel >> 2) & 3, bc = (p.blendSel >> 4) & 3, bd = (p.blendSel >> 6) & 3;
    int cc = bc == 0 ? a : bc == 1 ? ad : p.fix;
    int cs[3] = {r, g, b};
    int cd[3] = {int(dst32 & 0xFF), int((dst32 >> 8) & 0xFF), int((dst32 >> 16) & 0xFF)};
    int o[3];
    [unroll] for (int i = 0; i < 3; ++i) {
      int A = ba == 0 ? cs[i] : ba == 1 ? cd[i] : 0;
      int B = bb == 0 ? cs[i] : bb == 1 ? cd[i] : 0;
      int D = bd == 0 ? cs[i] : bd == 1 ? cd[i] : 0;
      o[i] = (((A - B) * cc) >> 7) + D;
    }
    r = o[0], g = o[1], b = o[2];
  }
  if (fl & F_COLCLAMP) {
    r = clamp(r, 0, 255), g = clamp(g, 0, 255), b = clamp(b, 0, 255);
  } else {
    r &= 0xFF, g &= 0xFF, b &= 0xFF;
  }
  if (fl & F_FBA) a |= 0x80;

  if (writeFb) {
    uint o = uint(r) | (uint(g) << 8) | (uint(b) << 16) | (uint(a & 0xFF) << 24);
    if (!writeAlpha) o = (o & 0xFFFFFF) | (dst32 & 0xFF000000);
    if (fb16) {
      uint m = to16(p.fbmsk) | ((p.fbmsk >> 31) << 15);
      store16(fbIndex, (to16(o) & ~m) | (dst & m));
    } else {
      uint m = fb24 ? p.fbmsk | 0xFF000000 : p.fbmsk;
      MEM.Store(memAt(fbIndex), (o & ~m) | (dst & m));
    }
  }
  if (writeZ) {
    if (z16) store16(zIndex, z);
    else if (p.zpsm == PSMZ24) MEM.Store(memAt(zIndex), (load32(zIndex) & 0xFF000000) | (z & 0xFFFFFF));
    else MEM.Store(memAt(zIndex), z);
  }
}
)";

// The hardware path (GsGpu::Impl::drawFast): a sample of a frame buffer kept as a GPU texture (a
// surface), whose depth test, depth buffer and blending are the GPU's own. Textures and palettes
// still come from GS memory (read only here). kDrawShared comes first.
inline constexpr const char* kFastPs = R"(
cbuffer FastPass : register(b0) {
  uint passMode;  // samples failing the alpha test: 0, 1 = dropped, 2 = only those are kept; 3: output 0
  float zScale;
  uint counting;  // a draw into a count surface: the value of each 5-bit channel
  uint exact;     // an exact blend: the shader computes ((A - B) * C) >> 7, the GPU adds Cd if D is
  uint weighted;  // the usual blend: the shader weighs Cs, the GPU adds Cd * (1 - C / 128)
  uint destRead;  // the whole blend here, as the GS, the destination read from a copy of the frame buffer
  float zBias;
  uint lowDepth;  // LOWRES: the depth test here, against depthCopy: 1 a 24 or 32-bit buffer, 2 a 16-bit one
};
Texture2D<float4> destCopy : register(t5);
#ifdef LOWRES
Texture2D<float> depthCopy : register(t7);
#endif
cbuffer PrimState : register(b2) { Prim primState; };
struct Out {
  float4 colour : SV_Target0;
  float4 factor : SV_Target1;  // alpha: the blend's C (source alpha or FIX) / 128
#if EXPORT_DEPTH
  // The GS's depth is whole: what the GPU interpolates, truncated (and clamped). Never above it, so
  // the GPU still rejects early what the GS tests (GEQUAL, GREATER) would. D16 surfaces need none
  // of this: the GPU's own rounding, of a depth almost half a step lower, truncates.
  float depth : SV_DepthLessEqual;
#endif
};
// The GPU interpolates the attributes, linearly on the screen as the GS does; depth too.
#if EXPORT_DEPTH
#define POSITION_INPUT noperspective centroid float4 pos : SV_Position
#else
#define POSITION_INPUT float4 pos : SV_Position
#endif
Out main(POSITION_INPUT, noperspective float4 colour : COLOUR,
         noperspective float4 stqf : STQF, noperspective float2 uv : TEXUV) {
  const Prim p = primState;
#ifdef LOWRES
  // LOWRES: drawn at the GS's resolution, a pixel standing for its middle sample (drawLowRes).
  int sxs = int(pos.x) * SCALE + SCALE / 2, sys = int(pos.y) * SCALE + SCALE / 2;
#else
  int sxs = int(pos.x), sys = int(pos.y);
#endif
  int x = sxs / SCALE, y = sys / SCALE;
  if (x < p.sx0 || x > p.sx1 || y < p.sy0 || y > p.sy1) discard;
#ifdef LOWRES
  if (lowDepth) {  // as the GPU tests the depth it would have been given (see EXPORT_DEPTH and zBias)
    const float stored = depthCopy.Load(int3(sxs, sys, 0));
    float zs, zd = stored;
    if (lowDepth == 2) zs = round(saturate(pos.z) * 65535.0), zd = round(stored * 65535.0);
    else zs = min(floor(pos.z / zScale + 1e-3), float(p.zmax)) * zScale;
    if (p.ztst == 2 ? zs < zd : zs <= zd) discard;
  }
#endif
  if (passMode == 3) {  // the second pass of a subtracting blend: with MAX, floors the result at 0
    Out none;
    none.colour = 0, none.factor = 0;
#if EXPORT_DEPTH
    none.depth = pos.z;
#endif
    return none;
  }
  const uint layer = uint(sys % SCALE) * SCALE + uint(sxs % SCALE);
  gLayer = layer;
  int4 c = clamp(int4(colour + 0.5), 0, 255);
  // Interpolation lands a hair under exact texel edges the GS reaches exactly; nudge past them.
  c = shadeWith(p, c, stqf.x + 2e-5 * stqf.z, stqf.y + 2e-5 * stqf.z, stqf.z, uv.x + 1.0 / 64, uv.y + 1.0 / 64, stqf.w + 1.0 / 64, layer);
  if (p.flags & F_ATE) {
    bool ok = alphaPasses(p, c.a);
    if (passMode == 2 ? ok : !ok) discard;
  }
  int factor = ((p.blendSel >> 4) & 3) == 0 ? c.a : p.fix;
  int a = (p.flags & F_FBA) ? (c.a | 0x80) : c.a;
  Out o;
#ifdef LOWRES
  // The weighed source, as below, and C / 128 for the GPU to gather the layers' colour (C * (1 - C')
  // + Cs') and what they let through of what is under them (T * (1 - C')); the alpha written.
  {
    const float f = float(factor) / 128.0, loss = 0.5 - float(min(factor & -factor, 128)) / 256.0;
    o.colour = float4((((p.blendSel & 3) == 0 ? float3(c.rgb) * f : float3(0, 0, 0)) - loss) / 255.0, f);
    o.factor = float4(float(a & 0xFF) / 255.0, 0, 0, 0);
    return o;
  }
#endif
#if EXPORT_DEPTH
  o.depth = min(floor(pos.z / zScale + 1e-3), float(p.zmax)) * zScale;  // as the GS, clamped to what the buffer holds
#endif
  if (destRead) {
    const int4 d = int4(saturate(destCopy.Load(int3(sxs, sys, 0))) * 255.0 + 0.5);
    if ((p.flags & F_DATE) && ((d.a >> 7) != 0) != ((p.flags & F_DATM) != 0)) discard;
    if (!(p.flags & F_BLEND)) {
      o.colour = float4(c.rgb, a & 0xFF) / 255.0;
      o.factor = 0;
      return o;
    }
    const int3 cd = (p.flags & F_FB16) ? (d.rgb & ~7) : d.rgb;  // the 16-bit pixel, read back
    const uint sel = p.blendSel;
    if (((sel >> 4) & 3) == 1) factor = (p.flags & F_FB24) ? 128 : d.a;
    const int3 A = (sel & 3) == 0 ? c.rgb : (sel & 3) == 1 ? cd : int3(0, 0, 0), B = ((sel >> 2) & 3) == 0 ? c.rgb : ((sel >> 2) & 3) == 1 ? cd : int3(0, 0, 0);
    const int3 D = ((sel >> 6) & 3) == 0 ? c.rgb : ((sel >> 6) & 3) == 1 ? cd : int3(0, 0, 0);
    int3 v = (((A - B) * factor) >> 7) + D;
    v = (p.flags & F_COLCLAMP) ? clamp(v, 0, 255) : (v & 0xFF);
    o.colour = float4(v, a & 0xFF) / 255.0;
    o.factor = 0;
    return o;
  }
  if (counting) {  // the 16-bit pixel's alpha is a bit, written rather than added
    o.colour = float4(c.r >> 3, c.g >> 3, c.b >> 3, (a >> 7) & 1);
    o.factor = 0;
    return o;
  }
  if (exact) {
    uint sel = p.blendSel;
    int3 A = (sel & 3) == 0 ? c.rgb : 0, B = ((sel >> 2) & 3) == 0 ? c.rgb : 0;
    int3 v = ((A - B) * factor) >> 7;
    if (((sel >> 6) & 3) == 1) {
      v = abs(v);  // its sign is the blend's (A is Cs and B 0, or the other way round)
    } else {
      if (((sel >> 6) & 3) == 0) v += c.rgb;
      v = (p.flags & F_COLCLAMP) ? clamp(v, 0, 255) : (v & 0xFF);
    }
    o.colour = float4(v, a & 0xFF) / 255.0;
    o.factor = 0;
    return o;
  }
  o.colour = float4(c.r, c.g, c.b, a & 0xFF) / 255.0;
  o.factor = float4(0, 0, 0, float(factor) / 128.0);
  if (weighted) {  // less what the GS's truncation loses on average: 1/2 - 1/(2g), g steps per unit of C's lowest bit
    const float loss = 0.5 - float(min(factor & -factor, 128)) / 256.0;
    o.colour.rgb = (((p.blendSel & 3) == 0 ? float3(c.rgb) * o.factor.a : float3(0, 0, 0)) - loss) / 255.0;
  }
  return o;
}
)";

// A texture in GS memory decoded into an RGBA texture, as fetchTexel reads it (palette and TEXA
// applied), for the hardware path to filter with the GPU's own sampler. kDrawShared comes first.
inline constexpr const char* kDecodeCs = R"(
cbuffer PrimState : register(b2) { Prim primState; };
RWTexture2D<unorm float4> decoded : register(u0);
[numthreads(8, 8, 1)] void main(uint3 t : SV_DispatchThreadID) {
  const Prim p = primState;
  if (int(t.x) >= p.tw || int(t.y) >= p.th) return;
  gLayer = 0;
  const uint c = fetchTexel(p, int(t.x), int(t.y));
  decoded[t.xy] = float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0;
}
)";

// Its vertex shader: GS depth to the depth buffer's 0..1.
inline constexpr const char* kFastVs = R"(
cbuffer FastPass : register(b0) {
  uint passMode;
  float zScale;
  uint pad0, pad1, pad2, pad3;
  float zBias;
};
struct VOut {
  float4 pos : SV_Position;
  noperspective float4 colour : COLOUR;
  noperspective float4 stqf : STQF;
  noperspective float2 uv : TEXUV;
};
VOut main(float2 p : POSITION, uint prim : PRIM, float z : DEPTH, float4 colour : COLOUR, float4 stqf : STQF, float2 uv : TEXUV) {
  VOut o;
  o.pos = float4(p.x / (1024.0 * SCALE) - 1.0, 1.0 - p.y / (1024.0 * SCALE), (z - zBias) * zScale, 1.0);
  o.colour = colour, o.stqf = stqf, o.uv = uv;
  return o;
}
)";

// A surface back into GS memory, sample by sample into the layers, only the pages `pages` marks.
inline constexpr const char* kSurfaceToMemCs = R"(
Texture2D<float4> surface : register(t0);
cbuffer C : register(b0) {
  uint bp, bw, psm, kind, width, height;  // kind: 0 colour, 1 depth, 2 count; width, height: samples
  float zMul;
  uint origin;     // of the samples covered: x | y << 16
  uint4 pages[4];  // a bit per GS memory page
};
[numthreads(8, 8, 1)] void main(uint3 t : SV_DispatchThreadID) {
  t.xy += uint2(origin & 0xFFFF, origin >> 16);
  if (t.x >= width || t.y >= height) return;
  uint x = t.x / SCALE, y = t.y / SCALE;
  if (!pageMarked(pages, psm, bp, bw, x, y)) return;
  gLayer = (t.y % SCALE) * SCALE + t.x % SCALE;
  float4 v = surface.Load(int3(t.xy, 0));
  uint value;
  if (kind == 1) {
    value = uint(v.r * zMul + 0.5);
  } else if (kind == 2) {  // counts wrap in their 5 bits
    uint4 c = uint4(int4(round(v)) & int4(31, 31, 31, 1));
    value = to16((c.r << 3) | (c.g << 11) | (c.b << 19) | (c.a << 31));
  } else {
    uint4 c = uint4(saturate(v) * 255.0 + 0.5);
    value = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
    if (psm == PSMCT16 || psm == PSMCT16S) value = to16(value);
  }
  writePixel(psm, bp, bw, x, y, value);
}
)";

// GS memory into a colour surface (COUNT: a count surface, the 16-bit pixel's channels as numbers).
inline constexpr const char* kMemToSurfaceCs = R"(
#if COUNT
RWTexture2D<float4> surface : register(u0);
#else
RWTexture2D<snorm float4> surface : register(u0);
#endif
cbuffer C : register(b0) {
  uint bp, bw, psm, width, height;  // width, height: samples
  uint origin;  // of the samples covered: x | y << 16
  uint2 pad;
  uint4 pages[4];  // a bit per GS memory page: only those are loaded
};
[numthreads(8, 8, 1)] void main(uint3 t : SV_DispatchThreadID) {
  t.xy += uint2(origin & 0xFFFF, origin >> 16);
  if (t.x >= width || t.y >= height) return;
  if (!pageMarked(pages, psm, bp, bw, t.x / SCALE, t.y / SCALE)) return;
  gLayer = (t.y % SCALE) * SCALE + t.x % SCALE;
  uint c = readPixel(psm, bp, bw, t.x / SCALE, t.y / SCALE);
#if COUNT
  surface[t.xy] = float4(c & 31, (c >> 5) & 31, (c >> 10) & 31, (c >> 15) & 1);
#else
  if (psm == PSMCT16 || psm == PSMCT16S) c = from16(c);
  surface[t.xy] = float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0;
#endif
}
)";

// GS memory into a depth surface: a full-screen pass writing the depth.
inline constexpr const char* kMemToDepthPs = R"(
cbuffer C : register(b0) {
  uint bp, bw, psm;
  float zScale;
  uint4 pages[4];
};
float main(float4 pos : SV_Position) : SV_Depth {
  uint sx = uint(pos.x), sy = uint(pos.y);
  if (!pageMarked(pages, psm, bp, bw, sx / SCALE, sy / SCALE)) discard;
  gLayer = (sy % SCALE) * SCALE + sx % SCALE;
  return float(readPixel(psm, bp, bw, sx / SCALE, sy / SCALE)) * zScale;
}
)";

// Host -> local transfer: one thread per pixel of the bit stream.
inline constexpr const char* kUploadCs = R"(
ByteAddressBuffer src : register(t0);
cbuffer C : register(b0) { uint dbp, dbw, dpsm, dsax, dsay, rrw, bpp, count, first; };  // first: word where the data starts
[numthreads(64, 1, 1)] void main(uint3 t : SV_DispatchThreadID) {
  uint i = t.y * 65536 + t.x;
  if (i >= count) return;
  uint bit = i * bpp, w = first + (bit >> 5), sh = bit & 31;
  uint v = src.Load(w << 2) >> sh;
  if (sh != 0 && sh + bpp > 32) v |= src.Load((w + 1) << 2) << (32 - sh);
  if (bpp < 32) v &= (1u << bpp) - 1;
  for (uint l = 0; l < LAYERS; ++l) {  // the host's pixels have no finer detail: every layer
    gLayer = l;
    writePixel(dpsm, dbp, dbw, (dsax + i % rrw) & 2047, (dsay + i / rrw) & 2047, v);
  }
}
)";

// Local -> local copy: first every source pixel into tmp, then into the destination.
inline constexpr const char* kCopyCs = R"(
RWStructuredBuffer<uint> tmp : register(u1);
cbuffer C : register(b0) { uint sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, count, scatter, layer; };
[numthreads(64, 1, 1)] void main(uint3 t : SV_DispatchThreadID) {
  uint i = t.y * 65536 + t.x;
  if (i >= count) return;
  gLayer = layer;
  uint tx = i % rrw, ty = i / rrw;
  if (scatter) writePixel(dpsm, dbp, dbw, (dsax + tx) & 2047, (dsay + ty) & 2047, tmp[i]);
  else tmp[i] = readPixel(spsm, sbp, sbw, (ssax + tx) & 2047, (ssay + ty) & 2047);
}
)";

// A palette load (Gs::loadClut): the previous slot's entries, those the load covers replaced.
inline constexpr const char* kClutCs = R"(
RWStructuredBuffer<uint> cluts : register(u1);
cbuffer C : register(b0) { uint prev, slot, cbp, cpsm, csm, csa, t8, cbw, cou, cov, ta0, ta1, aem; };
[numthreads(256, 1, 1)] void main(uint j : SV_GroupIndex) {
  uint v = cluts[prev * 256 + j];
  int i = -1;
  if (t8) {
    i = int(j);
  } else {
    uint k = (j - csa * 16) & 0xFF;
    if (k < 16) i = int(k);
  }
  if (i >= 0) {
    uint x, y, bw = 1;
    if (csm == 0) {
      if (t8) {
        uint jj = (uint(i) & 0xE7) | ((uint(i) & 0x08) << 1) | ((uint(i) & 0x10) >> 1);
        x = jj & 15, y = jj >> 4;
      } else {
        x = uint(i) & 7, y = uint(i) >> 3;
      }
    } else {
      bw = cbw, x = cou * 16 + uint(i), y = cov;
    }
    uint raw = readPixel(cpsm, cbp, bw, x, y);
    v = raw;
    if (cpsm != PSMCT32) {
      uint a = (raw & 0x8000) ? ta1 : ((aem && (raw & 0x7FFF) == 0) ? 0u : ta0);
      v = (from16(raw) & 0xFFFFFF) | (a << 24);
    }
  }
  cluts[slot * 256 + j] = v;
}
)";

// The displayed picture (Gs::picture) into a texture.
inline constexpr const char* kPictureCs = R"(
RWTexture2D<unorm float4> picture : register(u0);
cbuffer C : register(b0) { uint fbp, fbw, psm, dbx, dby, width, height, field, scale; };
[numthreads(8, 8, 1)] void main(uint3 t : SV_DispatchThreadID) {
  if (t.x >= width * scale || t.y >= height * scale) return;
  uint row = field ? t.y / 2 : t.y;  // in samples of the frame buffer
  gLayer = (row % scale) * scale + t.x % scale;
  uint c = readPixel(psm == PSMCT24 ? PSMCT32 : psm, fbp, fbw, dbx + t.x / scale, dby + row / scale);
  if (psm == PSMCT16 || psm == PSMCT16S) c = from16(c);
  picture[t.xy] = float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255) / 255.0;
}
)";

// The same from a colour surface holding the frame buffer.
inline constexpr const char* kSurfacePictureCs = R"(
Texture2D<float4> surface : register(t0);
RWTexture2D<unorm float4> picture : register(u0);
cbuffer C : register(b0) { uint originX, originY, width, height, field, step, bits16; };  // step: samples per picture pixel
[numthreads(8, 8, 1)] void main(uint3 t : SV_DispatchThreadID) {
  if (t.x >= width || t.y >= height) return;
  const uint row = field ? t.y / 2 : t.y;
  uint4 c = uint4(saturate(surface.Load(int3(originX + t.x * step, originY + row * step, 0))) * 255.0 + 0.5);
  if (bits16) c &= ~7u;
  picture[t.xy] = float4(c.rgb, 255) / 255.0;
}
)";

// The picture scaled into the window.
inline constexpr const char* kPresentVs = R"(
struct VOut { float4 pos : SV_Position; float2 uv : UV; };
VOut main(uint id : SV_VertexID) {
  VOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
)";
// The FPS counter over the window (Window's framesPerSecond): 3x5 digits on a dark box.
inline constexpr const char* kFpsPs = R"(
cbuffer Counter : register(b0) {
  float2 origin;  // the box's top left corner, in window pixels
  float cell;     // pixels per cell of the digits
  uint count;     // glyphs
  uint4 glyphs[2];  // 15 bits each, left to right
};
float4 main(float4 pos : SV_Position) : SV_Target {
  int2 at = int2(floor((pos.xy - origin) / cell));
  if (at.x < 0 || at.y < 0 || at.x >= int(count) * 4 + 1 || at.y >= 7) discard;
  int col = at.x - 1, row = at.y - 1;
  if (col >= 0 && row >= 0 && row < 5 && col % 4 < 3 && ((glyphs[col / 16][(col / 4) % 4] >> (14 - (row * 3 + col % 4))) & 1))
    return float4(1.0, 0.9, 0.25, 1.0);
  return float4(0.0, 0.0, 0.0, 0.6);
}
)";

// The map in the corner (game/minimap.h): triangles from a buffer, then the player's arrow.
inline constexpr const char* kMapVs = R"(
struct Vertex { float2 at; uint colour; };
StructuredBuffer<Vertex> mesh : register(t0);
cbuffer Map : register(b0) {
  float2 centre;     // the player
  float zoom;        // map units to clip space
  uint meshVertices;
  float2 facing;
  float arrow;       // its length, in clip space
  float unused;
};
struct VOut { float4 pos : SV_Position; float4 colour : COLOUR; };
VOut main(uint id : SV_VertexID) {
  VOut o;
  if (id < meshVertices) {
    const Vertex v = mesh[id];
    o.pos = float4((v.at.x - centre.x) * zoom, -(v.at.y - centre.y) * zoom, 0, 1);
    o.colour = float4(v.colour & 255, (v.colour >> 8) & 255, (v.colour >> 16) & 255, 255) / 255.0;
  } else {
    const float2 side = float2(facing.y, -facing.x);
    const float2 corner = id == meshVertices ? facing : (id == meshVertices + 1 ? -0.7 * facing + 0.6 * side : -0.7 * facing - 0.6 * side);
    o.pos = float4(corner.x * arrow, -corner.y * arrow, 0, 1);
    o.colour = float4(1.0, 0.9, 0.1, 1.0);
  }
  return o;
}
)";
inline constexpr const char* kMapPs = R"(
float4 main(float4 pos : SV_Position, float4 colour : COLOUR) : SV_Target { return colour; }
)";
// The finished map over the picture, in a frame of the same burnished gold as the HP bar's trim.
inline constexpr const char* kMapOverPs = R"(
Texture2D map : register(t0);
SamplerState linearSampler : register(s0);
cbuffer Over : register(b0) { float2 origin; float size; float alpha; };
static float pixel;
float cover(float d) { return saturate(0.5 - d / pixel); }
float diamond(float2 p, float2 c, float r) { const float2 q = abs(p - c); return q.x + q.y - r; }
float roundBox(float2 p, float halfSide, float r) { const float2 q = abs(p) - halfSide + r; return length(max(q, 0)) + min(max(q.x, q.y), 0) - r; }
void over(inout float4 c, float3 colour, float a) {
  const float outA = a + c.a * (1 - a);
  if (outA > 0) c.rgb = (colour * a + c.rgb * c.a * (1 - a)) / outA;
  c.a = outA;
}
// Across a band, t from 0 to 1: dark edges and a bright line.
float3 gold(float t) {
  const float3 dark = float3(0.27, 0.18, 0.07), mid = float3(0.66, 0.50, 0.22), light = float3(0.98, 0.88, 0.58);
  return t < 0.35 ? lerp(dark, light, t / 0.35) : lerp(light, lerp(mid, dark, (t - 0.35) / 0.65), saturate((t - 0.35) / 0.25));
}
float4 main(float4 pos : SV_Position) : SV_Target {
  const float2 uv = (pos.xy - origin) / size;
  pixel = 1.0 / size;
  if (uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1) discard;
  const float2 p = uv - 0.5;
  const float2 q = abs(p);  // the frame is the same in its four quarters
  const float outer = roundBox(p, 0.46, 0.008);
  const float inner = outer + 0.036;
  const float3 outline = float3(0.06, 0.04, 0.02);
  float4 c = map.Sample(linearSampler, uv);
  c.a = lerp(0, max(c.a, 0.78), cover(inner));
  over(c, gold(saturate(-outer / 0.036)), cover(outer) * (1 - cover(inner)));
  over(c, float3(0.05, 0.03, 0.02), cover(abs(inner) - 0.0035));          // the shadow inside
  over(c, float3(0.52, 0.40, 0.18), cover(abs(inner + 0.012) - 0.0016));  // a thin line within it
  over(c, outline, cover(abs(outer) - 0.0025));
  // Corners: a pointed diamond with a dark red stone; a smaller point in the middle of each side.
  const float corner = diamond(q, float2(0.445, 0.445), 0.062);
  over(c, outline, cover(corner - 0.005));
  over(c, gold(saturate(0.2 - corner / 0.124)), cover(corner));
  const float stone = diamond(q, float2(0.445, 0.445), 0.024);
  over(c, float3(0.05, 0.02, 0.02), cover(stone - 0.005));
  over(c, lerp(float3(0.28, 0.02, 0.04), float3(0.85, 0.12, 0.12), saturate(0.3 - stone / 0.03)), cover(stone));
  const float side = min(diamond(q, float2(0.0, 0.452), 0.04), diamond(q, float2(0.452, 0.0), 0.04));
  over(c, outline, cover(side - 0.004));
  over(c, gold(saturate(0.25 - side / 0.08)), cover(side));
  c.a *= alpha;
  return c;
}
)";

// A run drawn at the GS's resolution (drawLowRes) over its surface: the layers' colour C added to
// what they let through T of the surface, alpha the last layer's where one was drawn.
inline constexpr const char* kLowResOverPs = R"(
Texture2D<float4> layers : register(t0);  // C, T
Texture2D<float> last : register(t1);     // alpha / 255; 0 where none was drawn
SamplerState smooth : register(s1);
cbuffer Size : register(b0) { float2 texel; };  // 1 / the size of layers
struct Out {
  float4 colour : SV_Target0;
  float4 factor : SV_Target1;
};
Out main(float4 pos : SV_Position) {
  const float4 l = layers.SampleLevel(smooth, pos.xy / SCALE * texel, 0);
  const float a = last.Load(int3(int2(pos.xy) / SCALE, 0));
  Out o;
  o.colour = float4(l.rgb, a);
  o.factor = float4(l.aaa, a > 0 ? 1 : 0);
  return o;
}
)";

inline constexpr const char* kPresentPs = R"(
Texture2D picture : register(t0);
SamplerState pointSampler : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : UV) : SV_Target { return picture.Sample(pointSampler, uv); }
)";

}  // namespace gpu_hlsl
