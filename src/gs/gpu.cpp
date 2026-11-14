#include "gs/gpu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <format>
#include <cstdio>
#include <cstdlib>

#include "gs/color.h"
#include "gs/draw.h"
#include "gs/frame_gen.h"
#include "gs/gpu_shaders.h"
#include "gs/shader_cache.h"
#include "gs/texture_pack.h"
#include "game/minimap.h"
#include "host/icon.h"
#include "host/screenshot.h"
#include "host/settings.h"
#include "host/window.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t kClutSlots = 4096;  // palettes in flight; slot 0 is the empty start palette

// One primitive's state and attribute planes, as the pixel shader's Prim.
struct GpuPrim {
  uint32_t flags, fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmax, ztst, atst, afail;
  int32_t aref;
  uint32_t blendSel;
  int32_t fix;
  uint32_t tbp, tbw, tpsm, tfx, wms, wmt, minu, maxu, minv, maxv, ta0, ta1, csa;
  int32_t tw, th, fogR, fogG, fogB, sx0, sx1, sy0, sy1;
  uint32_t clutSlot, flat, zConst;
  float fx0, fy0;
  float pl[33];
};
enum : uint32_t {
  F_FB16 = 1 << 0, F_FB24 = 1 << 1, F_ZTEST = 1 << 2, F_ZWRITE = 1 << 3, F_ATE = 1 << 4, F_DATE = 1 << 5,
  F_DATM = 1 << 6, F_BLEND = 1 << 7, F_PABE = 1 << 8, F_FBA = 1 << 9, F_COLCLAMP = 1 << 10, F_TME = 1 << 11,
  F_FST = 1 << 12, F_TCC = 1 << 13, F_AEM = 1 << 14, F_FGE = 1 << 15, F_IIP = 1 << 16, F_ZCONST = 1 << 17, F_LINEAR = 1 << 18, F_CACHED = 1 << 19,
};
enum { kZ, kR, kG, kB, kA, kS, kT, kQ, kU, kV, kF };

// A vertex: its position and primitive (the GS-memory path evaluates the primitive's planes), and
// the attributes the GPU interpolates for the hardware path.
struct Attr {
  float z;
  float r, g, b, a;
  float s, t, q, f;
  float u, v;  // 10.4 fixed point
};
struct Vtx {
  float x, y;
  uint32_t prim;
  Attr attr;
};
template <typename V>  // Gs::Vertex
Attr attrOf(const V& v, float z, const V& colour) {
  return {z, float(colour.r), float(colour.g), float(colour.b), float(colour.a), v.s, v.t, v.q, float(v.f), float(v.u), float(v.v)};
}

void setPlane(GpuPrim& p, int k, float base, float dx = 0, float dy = 0) {
  p.pl[3 * k] = base, p.pl[3 * k + 1] = dx, p.pl[3 * k + 2] = dy;
}
uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return r | g << 8 | b << 16 | uint32_t(a) << 24; }

// A pixel or rectangle (GS pixel coordinates) of constant attributes.
void constant(GpuPrim& p, uint32_t z, uint32_t colour, float s, float t, float q, float u, float v, float f) {
  p.flags |= F_ZCONST;
  p.zConst = z, p.flat = colour;
  setPlane(p, kS, s), setPlane(p, kT, t), setPlane(p, kQ, q), setPlane(p, kU, u), setPlane(p, kV, v), setPlane(p, kF, f);
}
// GS pixel coordinates to the draw's, `scale` times larger and half a sample on: D3D samples
// pixel centres, the GS the pixels' corners.
float toTarget(float gs, float scale) { return gs * scale + 0.5f; }
// A rectangle as two triangles; attributes at its corners (top left, top right, bottom left, bottom right).
void quad(std::vector<Vtx>& out, float x0, float y0, float x1, float y1, uint32_t prim, float scale, const Attr& tl,
          const Attr& tr, const Attr& bl, const Attr& br) {
  x0 = toTarget(x0, scale), y0 = toTarget(y0, scale), x1 = toTarget(x1, scale), y1 = toTarget(y1, scale);
  out.insert(out.end(), {{x0, y0, prim, tl}, {x1, y0, prim, tr}, {x0, y1, prim, bl}, {x1, y0, prim, tr}, {x1, y1, prim, br}, {x0, y1, prim, bl}});
}
void quad(std::vector<Vtx>& out, float x0, float y0, float x1, float y1, uint32_t prim, float scale, const Attr& all) {
  quad(out, x0, y0, x1, y1, prim, scale, all, all, all, all);
}

ComPtr<ID3DBlob> compile(const std::string& source, const char* target, std::string& error) { return compileShader(source, target, error); }
}  // namespace

struct GsGpu::Impl {
  static GpuPrim stateOf(const Gs::Draw& d) {
    GpuPrim p{};
    p.flags = (d.fb16 ? F_FB16 : 0) | (d.fb24 ? F_FB24 : 0) | (d.ztest ? F_ZTEST : 0) | (d.zwrite ? F_ZWRITE : 0) |
              (d.ate ? F_ATE : 0) | (d.date ? F_DATE : 0) | (d.datm ? F_DATM : 0) | (d.blend ? F_BLEND : 0) |
              (d.pabe ? F_PABE : 0) | (d.fba ? F_FBA : 0) | (d.colclamp ? F_COLCLAMP : 0) | (d.tme ? F_TME : 0) |
              (d.fst ? F_FST : 0) | (d.tcc ? F_TCC : 0) | (d.aem ? F_AEM : 0) | (d.fge ? F_FGE : 0) |
              (d.linear ? F_LINEAR : 0);
    p.fbp = d.fbp, p.fbw = d.fbw, p.fpsm = d.fpsm, p.fbmsk = d.fbmsk;
    p.zbp = d.zbp, p.zpsm = d.zpsm, p.zmax = d.zmax, p.ztst = d.ztst;
    p.atst = d.atst, p.afail = d.afail, p.aref = d.aref;
    p.blendSel = uint32_t(d.ba | d.bb << 2 | d.bc << 4 | d.bd << 6), p.fix = d.fix;
    p.tbp = d.tbp, p.tbw = d.tbw, p.tpsm = d.tpsm, p.tfx = d.tfx, p.wms = d.wms, p.wmt = d.wmt;
    p.minu = d.minu, p.maxu = d.maxu, p.minv = d.minv, p.maxv = d.maxv, p.ta0 = d.ta0, p.ta1 = d.ta1, p.csa = d.csa;
    p.tw = d.tw, p.th = d.th, p.fogR = d.fogR, p.fogG = d.fogG, p.fogB = d.fogB;
    p.sx0 = d.sx0, p.sx1 = d.sx1, p.sy0 = d.sy0, p.sy1 = d.sy1;
    p.clutSlot = d.clutSlot;
    return p;
  }

  uint32_t scale = 1;  // resolution scale; GS memory holds scale * scale layers
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  // GS memory: written by the draws (rasterizer-ordered) and compute shaders, read by the picture.
  ComPtr<ID3D11Buffer> mem;
  ComPtr<ID3D11UnorderedAccessView> memUav;
  ComPtr<ID3D11ShaderResourceView> memSrv;
  ComPtr<ID3D11ShaderResourceView> swzSrv;
  ComPtr<ID3D11Buffer> cluts;
  ComPtr<ID3D11UnorderedAccessView> clutUav;
  ComPtr<ID3D11ShaderResourceView> clutSrv;
  uint32_t nextClut = 1;
  // Palettes already loaded, by everything the load depends on; memory is unchanged while the
  // write counts of the pages it reads are.
  struct ClutKey {
    uint32_t previous, previousUse;  // the palette it starts from (and which filling of that slot)
    uint32_t cbp, psm, csm, csa, t8, texclut, texa;
    uint64_t memory;  // what is in memory there: an upload's hash, or (unknown) its blocks' write counts
    bool known;
    bool operator==(const ClutKey&) const = default;
  };
  struct ClutHash {
    size_t operator()(const ClutKey& k) const {
      size_t h = k.memory + k.known;
      for (uint32_t v : {k.previous, k.previousUse, k.cbp, k.psm, k.csm, k.csa, k.t8, k.texclut, k.texa}) h = h * 0x9E3779B97F4A7C15ull + v;
      return h;
    }
  };
  // The last upload to each block pointer: where, its data's hash, and its blocks' write count after.
  struct Upload {
    uint32_t bw, psm, x0, y0, x1, y1;
    uint64_t hash, writes;
  };
  std::unordered_map<uint32_t, Upload> uploads;
  std::unordered_map<ClutKey, uint32_t, ClutHash> clutCache;
  std::vector<ClutKey> clutOfSlot = std::vector<ClutKey>(kClutSlots);  // what each slot holds (to forget it when reused)
  std::vector<uint32_t> slotUses = std::vector<uint32_t>(kClutSlots);   // how often each slot has been filled
  ComPtr<ID3D11Buffer> copyTmp;
  ComPtr<ID3D11UnorderedAccessView> copyTmpUav;
  // Filtered textures, decoded once into RGBA textures for the GPU's sampler: four fetches from GS
  // memory per sample cost more than the whole draw at 4x. One per texture and palette, decoded
  // again when the write counts of its memory change.
  struct TexKey {
    uint32_t tbp, tbw, tpsm, tw, th, clutSlot, clutUse, csa, texa;
    bool operator==(const TexKey&) const = default;
  };
  struct TexKeyHash {
    size_t operator()(const TexKey& k) const {
      size_t h = 0;
      for (uint32_t v : {k.tbp, k.tbw, k.tpsm, k.tw, k.th, k.clutSlot, k.clutUse, k.csa, k.texa}) h = h * 0x9E3779B97F4A7C15ull + v;
      return h;
    }
  };
  struct TexEntry {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint64_t memory = ~0ull;  // the write counts it was decoded at
    uint64_t lastUse = 0;
    uint64_t content = 0;  // its hash for texture packs (contentOf); 0 if it has none
  };
  std::unordered_map<TexKey, TexEntry, TexKeyHash> texCache;
  size_t texCacheBytes = 0;
  uint64_t texUse = 0;
  static constexpr size_t kTexCacheBytes = size_t(384) << 20;
  ComPtr<ID3D11ComputeShader> decodeCs;
  ComPtr<ID3D11SamplerState> texSamplers[4];  // linear; bit 0: S clamped, bit 1: T clamped (else repeated)
  ID3D11ShaderResourceView* decoded(const Gs::Draw& d);
  // Texture packs (gs/texture_pack.h): the hash of a texture uploaded whole (with its palette's,
  // if it has one), or 0; the palette content of each slot, likewise; the textures loaded in place
  // of the game's (null for a file that could not be read).
  TexturePack pack;
  std::vector<uint64_t> slotContent = std::vector<uint64_t>(kClutSlots);
  std::unordered_map<uint64_t, ComPtr<ID3D11ShaderResourceView>> replaced;
  uint64_t contentOf(const Gs::Draw& d) const;
  void dumpTexture(ID3D11Texture2D* tex, uint32_t w, uint32_t h, uint64_t hash);
  ID3D11ShaderResourceView* replacement(uint64_t hash);
  // Per state of the batch: its decoded texture (F_CACHED), and which run last looked.
  std::vector<ID3D11ShaderResourceView*> stateTex;
  std::vector<uint64_t> stateRun;
  uint64_t runNo = 0;

  ComPtr<ID3D11VertexShader> drawVs;
  ComPtr<ID3D11PixelShader> drawPs;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11RasterizerState> rasterizer, scissored;
  // Draws need a render target for the rasterizer to run; nothing is written to it.
  ComPtr<ID3D11RenderTargetView> dummyTarget;
  ComPtr<ID3D11BlendState> noWrites;
  ComPtr<ID3D11ComputeShader> uploadCs, copyCs, clutCs, pictureCs, surfacePictureCs;
  ComPtr<ID3D11VertexShader> presentVs;
  ComPtr<ID3D11PixelShader> presentPs, fpsPs;
  // The map in the corner: drawn apart with 4x antialiasing, then over the picture.
  ComPtr<ID3D11VertexShader> mapVs;
  ComPtr<ID3D11PixelShader> mapPs, mapOverPs;
  ComPtr<ID3D11Buffer> mapMesh;
  ComPtr<ID3D11ShaderResourceView> mapMeshSrv, mapSrv;
  ComPtr<ID3D11Texture2D> mapMsaa, mapResolved;
  ComPtr<ID3D11RenderTargetView> mapRtv;
  std::vector<MinimapVertex> mapVertices;
  uint64_t mapVersion = 0;
  uint32_t mapSize = 0;
  void drawMap(const MinimapView& view, float left, float top, uint32_t size);
  // Interpolation (Settings::interpolation): pictures shown for each of the game's, as many as the
  // screen's refresh holds (1 without it, or when the game outruns real time, as in fast loading).
  std::unique_ptr<FrameGen> frameGen;
  bool frameGenFailed = false;
  FrameGen* generator();
  // Its pace: a thread waits on the swap chain for each refresh of the screen and wakes the
  // thread that draws (onRefresh), which shows the picture of that moment (tick): the game's
  // pictures a little late, each moment between two made from them.
  HANDLE refreshWait = nullptr;  // the swap chain's: signalled when it takes another picture
  std::thread refreshThread;
  std::atomic<bool> refreshStop = false, interpolating = false, ticked = false;  // ticked: a picture shown by tick since interpolation began
  std::mutex callMutex;  // ticks against calls from the EE thread (picture, readMemory)
  double stampA = 0, stampB = 0;  // the two latest pictures' VBlanks (seconds)
  double frameSeconds = 1 / 59.94, lag = 0;  // between VBlanks; the most a picture came after its own
  std::array<double, 120> delays{};  // ... of the latest pictures
  double refreshHz = 60;
  uint64_t presents = 0;
  std::string fps;  // the counter's text, from the game's last picture
  FrameGen::View view;  // the camera's in the next picture (setView)
  bool playing = false;  // the game is being played in it (setView)
  bool target(RECT& rc);  // the window's size, the back buffer made to fit it; false if none
  // The picture (`srv`) in the window, then the minimap and the counter.
  void show(ID3D11ShaderResourceView* srv, const RECT& rc);
  ComPtr<ID3D11BlendState> overBlend;  // the counter's box over the picture
  ComPtr<ID3D11SamplerState> pointSampler, linearSampler;
  ComPtr<ID3D11Buffer> constants;  // 256 bytes
  ComPtr<ID3D11Buffer> texSource[3];  // the hardware path's TexSource: textures from memory, a surface, a count surface

  // The hardware path: frame and depth buffers as GPU textures ("surfaces"), drawn with the GPU's
  // own depth test and blending; GS memory stays the reference copy, kept in step page by page
  // (a GS page is 8 KB: 64 x 32 pixels of a 32-bit buffer).
  ComPtr<ID3D11PixelShader> fastPs, fastDepthPs, memToDepthPs;  // fastDepthPs: exports depth, for 24 and 32-bit depth
  ComPtr<ID3D11VertexShader> fastVs;
  ComPtr<ID3D11InputLayout> fastLayout;
  ComPtr<ID3D11ComputeShader> surfaceToMemCs, memToSurfaceCs, memToCountCs;
  struct Surface {
    uint32_t bp, bw, psm;
    bool depth;
    bool count = false;  // a 16-bit frame buffer for counting draws: its channels as numbers
    uint32_t width = 0, height = 0;  // GS pixels covered
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Texture2D> copy;  // what a draw into it reads of it
    ComPtr<ID3D11ShaderResourceView> copySrv;
  };
  // What a surface holds now in these GS pixels, for a draw into it to read.
  ID3D11ShaderResourceView* snapshot(Surface& sf, int x0, int y0, int x1, int y1);
  ComPtr<ID3D11BlendState> floorBlend;  // MAX of the result and the shader's 0
  // Runs of many see-through layers over a large part of the screen (the smoke of the giant
  // breaking the wall: about 200), above 1x: drawn at the GS's resolution, gathering their colour
  // and what they let through, then over the surface in one go. The surface keeps its detail
  // under them; only the layers are as the PS2 draws them.
  ComPtr<ID3D11PixelShader> lowResPs, lowResOverPs;
  ComPtr<ID3D11Texture2D> lowTex, lowLastTex;
  ComPtr<ID3D11RenderTargetView> lowRtv, lowLastRtv;
  ComPtr<ID3D11ShaderResourceView> lowSrv, lowLastSrv;
  uint32_t lowW = 0, lowH = 0;
  ComPtr<ID3D11BlendState> lowBlend[2], lowOverBlend[2];  // by whether alpha is written
  bool lowResRun(const std::vector<Gs::Command>& commands, size_t first, size_t end, int x0, int y0, int x1, int y1, bool destRead) const;
  bool drawLowRes(size_t first, size_t end, const Gs::Draw& d, int depth, float zScale, bool alpha, int x0, int y0, int x1, int y1,
                  const Surface& cs);
  ComPtr<ID3D11DepthStencilState> noDepth;
  std::vector<Surface> surfaces;
  // The depth of the last 3D scene drawn, for interpolation: kept when its depth buffer is cleared
  // (the game clears it again for other passes before the picture is shown).
  std::vector<int> drew3D;  // per surface: runs of depth-tested drawing since it was last cleared
  ComPtr<ID3D11Texture2D> sceneDepth;
  ComPtr<ID3D11ShaderResourceView> sceneDepthSrv;
  bool sceneDepth16 = false, sceneDepthNew = false;
  int lastDepth = -1;  // the depth surface depth-tested last
  void keepSceneDepth(int depth);
  int sceneDepthOf = -1;  // the surface it was kept from
  // The picture's depth for interpolation: the depth buffer as the frame left it, and the scene's
  // before the characters were drawn over it, when a 3D scene was drawn since the last picture
  // (else none: menus, movies).
  struct PictureDepth {
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* scene = nullptr;
  };
  PictureDepth pictureDepth() {
    PictureDepth z;
    if (sceneDepthNew && lastDepth >= 0 && lastDepth == sceneDepthOf) z = {surfaces[size_t(lastDepth)].srv.Get(), sceneDepthSrv.Get()};
    sceneDepthNew = false;
    return z;
  }
  using Pages = std::bitset<512>;
  std::array<int, 512> owner{};  // the surface holding each page, or -1
  Pages memStale, ownerStale;    // the owner has newer pixels than memory / memory than the owner
  Pages held;                    // pages some surface holds
  Pages rendered;                // drawn into since the CPU last wrote there
  // How often memory in each 256-byte block has been written (or may have been).
  std::vector<uint32_t> blockWrites = std::vector<uint32_t>(16384);
  // Which upload last wrote each block (its data's hash, its first block), with the block's write
  // count then: a texture whose blocks all still hold one upload's data has that upload's content.
  struct BlockSource {
    uint64_t hash = 0;
    uint32_t dbp = 0, writes = 0;
  };
  std::vector<BlockSource> blockSource = std::vector<BlockSource>(16384);
  void wrote(const Pages& pages) {
    for (size_t p = 0; p < 512; ++p)
      if (pages[p])
        for (size_t b = p * 32; b < p * 32 + 32; ++b) ++blockWrites[b];
  }
  // The blocks a rectangle of a buffer covers, each once.
  template <typename F>
  static void forBlocks(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, F f) {
    const uint32_t bpp = GsMemory::bitsPerPixel(psm), w = bpp >= 24 ? 8 : bpp == 4 ? 32 : 16, h = bpp >= 16 ? 8 : 16;
    for (uint32_t y = y0 / h * h; y <= y1; y += h)
      for (uint32_t x = x0 / w * w; x <= x1; x += w) {
        const uint32_t px = std::max(x, x0) & 2047, py = std::max(y, y0) & 2047;
        uint32_t byte;
        switch (psm) {
          case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: byte = GsMemory::addr16(bp, bw, px, py, psm) * 2; break;
          case PSMT8: byte = GsMemory::addr8(bp, bw, px, py); break;
          case PSMT4: byte = GsMemory::addr4(bp, bw, px, py) / 2; break;
          default: byte = GsMemory::addr32(bp, bw, px, py, psm >= PSMZ32) * 4; break;
        }
        f(byte / 256);
      }
  }
  uint64_t writesOf(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const {
    uint64_t sum = 0;
    forBlocks(bp, bw, psm, x0, y0, x1, y1, [&](uint32_t b) { sum += blockWrites[b]; });
    return sum;
  }
  std::map<uint32_t, ComPtr<ID3D11BlendState>> blendStates;
  std::map<uint32_t, ComPtr<ID3D11DepthStencilState>> depthStates;
  uint64_t fastDraws = 0, slowDraws = 0, syncs = 0, lowResRuns = 0;
  // The graphics card's working time (with a window, or CODPORT_GPU_TIME set): timestamps around
  // each piece of work, by kind, read a few frames late.
  enum Work { kIdle, kSlow, kFast, kFlush, kLoad, kUpload, kCopy, kClut, kPicture, kWorks };
  struct FrameStamps {
    ComPtr<ID3D11Query> disjoint;
    std::vector<ComPtr<ID3D11Query>> queries;
    std::vector<int> works;  // what follows each stamp
    bool pending = false, open = false;
  };
  std::array<FrameStamps, 8> stamps;
  uint64_t frameNo = 0, gpuFrames = 0;
  std::atomic<double> busySeconds{0};
  double workMs[kWorks] = {};
  bool timing() const {
    static const bool asked = std::getenv("CODPORT_GPU_TIME") != nullptr;
    return hwnd || asked;
  }
  void mark(int work);
  void frameTimer();
  struct Busy {  // a piece of GPU work
    Impl& m;
    Busy(Impl& m, int work) : m(m) { m.mark(work); }
    ~Busy() { m.mark(kIdle); }
  };

  // Per-draw data in rings: each draw appends after the last without disturbing data the GPU may
  // still read, and only wrapping round discards the buffer. Discarding on every draw instead
  // stalls the driver once its supply of renamed buffers runs out (scenes with many draws).
  struct Dynamic {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    size_t bytes = 0;
    size_t head = 0;  // where the next data goes
  };
  bool noOverwriteSrv = false;  // appending works for buffers read through views too (D3D 11.1)
  Dynamic prims, vertices, uploadData;
  std::vector<GpuPrim> primList;
  std::vector<Vtx> vertexList;
  std::vector<uint32_t> firstVertex;
  std::vector<GpuPrim> states;         // the hardware path's states in this batch (their planes unused)
  std::vector<uint32_t> commandState;  // each hardware command's among them
  ComPtr<ID3D11Buffer> primStateBuffer;
  // The run's commands, a draw call per state.
  void drawStates(size_t first, size_t end);
  std::vector<uint8_t> ways;  // each primitive's: the hardware path or not
  std::vector<uint64_t> uploadScratch;
  void drawSlow(const std::vector<Gs::Command>& commands, size_t first, size_t end, UINT start, UINT count);
  void drawFast(const std::vector<Gs::Command>& commands, size_t first, size_t end, UINT start, UINT count, bool destRead);
  static bool sameRun(const Gs::Draw& a, const Gs::Draw& b);
  // The pixels a primitive can touch: its vertices' box within the scissor (the scissor alone often
  // reaches past the frame buffer, into the next buffer in memory).
  static void bounds(const Gs::Command& c, int& x0, int& y0, int& x1, int& y1) {
    const int n = c.type == 0 ? 1 : c.type <= 2 || c.type == 6 ? 2 : 3;
    int ax = INT32_MAX, ay = INT32_MAX, bx = INT32_MIN, by = INT32_MIN;
    for (int i = 0; i < n; ++i)
      ax = std::min(ax, c.v[i].x >> 4), ay = std::min(ay, c.v[i].y >> 4), bx = std::max(bx, (c.v[i].x + 15) >> 4),
      by = std::max(by, (c.v[i].y + 15) >> 4);
    x0 = std::max(ax, c.d.sx0), y0 = std::max(ay, c.d.sy0), x1 = std::min(bx, c.d.sx1), y1 = std::min(by, c.d.sy1);
  }
  // Whether a primitive may write depth: not if every sample fails an alpha test whose AFAIL
  // mode keeps depth (as shadow volumes counted in a colour channel do).
  static bool writesDepth(const Gs::Draw& d) {
    return d.ztest && d.zwrite && !(d.ate && d.atst == 0 && d.afail != 2);
  }
  // The texels a primitive's texture coordinates can reach (all the texture's when they wrap).
  static void texRect(const Gs::Command& c, int& x0, int& y0, int& x1, int& y1) {
    const Gs::Draw& d = c.d;
    x0 = 0, y0 = 0, x1 = d.tw - 1, y1 = d.th - 1;
    const int n = c.type == 0 ? 1 : c.type <= 2 || c.type == 6 ? 2 : 3;
    float us[3], vs[3];
    for (int i = 0; i < n; ++i) {
      if (d.fst) {
        us[i] = float(c.v[i].u) / 16, vs[i] = float(c.v[i].v) / 16;
      } else {
        if (c.v[i].q <= 0) return;
        us[i] = c.v[i].s / c.v[i].q * float(d.tw), vs[i] = c.v[i].t / c.v[i].q * float(d.th);
      }
    }
    float u0 = us[0], u1 = us[0], v0 = vs[0], v1 = vs[0];
    for (int i = 1; i < n; ++i) u0 = std::min(u0, us[i]), u1 = std::max(u1, us[i]), v0 = std::min(v0, vs[i]), v1 = std::max(v1, vs[i]);
    if (c.type == 6) {  // a sprite: what its first and last pixels read (the far edge is never reached)
      int cx0, cy0, cx1, cy1;
      coverage(c, cx0, cy0, cx1, cy1);
      const float xa = float(c.v[0].x) / 16, xb = float(c.v[1].x) / 16, ya = float(c.v[0].y) / 16, yb = float(c.v[1].y) / 16;
      if (cx1 >= cx0 && cy1 >= cy0 && xb != xa && yb != ya) {
        auto at = [](float p, float pa, float pb, float ta, float tb) { return ta + (p - pa) * (tb - ta) / (pb - pa); };
        const float p0 = at(float(cx0), xa, xb, us[0], us[1]), p1 = at(float(cx1), xa, xb, us[0], us[1]);
        const float q0 = at(float(cy0), ya, yb, vs[0], vs[1]), q1 = at(float(cy1), ya, yb, vs[0], vs[1]);
        u0 = std::min(p0, p1), u1 = std::max(p0, p1), v0 = std::min(q0, q1), v1 = std::max(q0, q1);
      }
    }
    // Between those coordinates (a hair beyond, as interpolated), unless they wrap.
    // Filtering reaches half a texel further; past the edge, that would make any sprite starting at
    // the texture's first texel seem to wrap and reach all of it.
    auto range = [reach = d.linear ? 0.5f : 0.0f](float lo, float hi, uint32_t mode, int size, int& a, int& b) {
      const int na = int(std::floor(lo - 0.05f)), nb = int(std::floor(hi + 0.05f));
      if (mode >= 2 || (mode == 0 && (na < 0 || nb >= size))) return;
      a = std::clamp(int(std::floor(lo - 0.05f - reach)), 0, size - 1), b = std::clamp(int(std::floor(hi + 0.05f + reach)), 0, size - 1);
    };
    range(u0, u1, d.wms, d.tw, x0, x1);
    range(v0, v1, d.wmt, d.th, y0, y1);
  }
  static Pages texPages(const Gs::Command& c) {
    int x0, y0, x1, y1;
    texRect(c, x0, y0, x1, y1);
    return pagesOf(c.d.tbp, c.d.tbw, c.d.tpsm, x0, y0, x1, y1);
  }
  // The same, as many as matter: the whole texture's pages when no surface holds any of them.
  Pages texPagesHeld(const Gs::Command& c) {
    const Gs::Draw& d = c.d;
    if (d.tbp != lastTex.tbp || d.tbw != lastTex.tbw || d.tpsm != lastTex.tpsm || d.tw != lastTex.tw || d.th != lastTex.th)
      lastTex = {d.tbp, d.tbw, d.tpsm, d.tw, d.th, pagesOf(d.tbp, d.tbw, d.tpsm, 0, 0, d.tw - 1, d.th - 1)};
    return (lastTex.pages & held).none() ? lastTex.pages : texPages(c);
  }
  struct {
    uint32_t tbp = ~0u, tbw = 0, tpsm = 0;
    int tw = 0, th = 0;
    Pages pages;
  } lastTex;
  // Whether a primitive's texture reads what is drawn in this box of its frame or depth buffer:
  // by pixels where the texture has the buffer's layout, by pages otherwise.
  static bool readsDrawn(const Gs::Command& c, int x0, int y0, int x1, int y1) {
    const Gs::Draw& d = c.d;
    int tx0, ty0, tx1, ty1;
    texRect(c, tx0, ty0, tx1, ty1);
    const Pages tex = pagesOf(d.tbp, d.tbw, d.tpsm, tx0, ty0, tx1, ty1);
    auto hits = [&](uint32_t bp, uint32_t psm) {
      if (d.tbp == bp && d.tbw == d.fbw && d.tpsm == psm) return tx0 <= x1 && x0 <= tx1 && ty0 <= y1 && y0 <= ty1;
      return (tex & pagesOf(bp, d.fbw, psm, x0, y0, x1, y1)).any();
    };
    return hits(d.fbp, d.fpsm) || (d.ztest && hits(d.zbp, d.zpsm));
  }
  // Whether a sprite whose texture is its own frame buffer reads only texels it has not written yet
  // when it reads them, as the GS draws it (rows top to bottom, each left to right): then reading
  // the texture as it was before the sprite gives the same pixels.
  static bool readsAhead(const Gs::Command& c) {
    const Gs::Draw& d = c.d;
    if (c.type != 6 || d.tbp != d.fbp || d.tbw != d.fbw || d.tpsm != d.fpsm) return false;
    if (d.ztest && d.zbp == d.tbp) return false;
    int x0, y0, x1, y1;
    coverage(c, x0, y0, x1, y1);
    if (x1 < x0 || y1 < y0) return true;
    const Gs::Vertex &a = c.v[0], &b = c.v[1];
    // The texel coordinate at a pixel, linear between the vertices (as interpolated: a hair either way).
    auto axis = [&](float pa, float pb, float ta, float tb, int size, uint32_t mode, int p, int& lo, int& hi) {
      const float tc = pb == pa ? ta : ta + (float(p) - pa) * (tb - ta) / (pb - pa);
      lo = int(std::floor(tc - 0.05f)), hi = int(std::floor(tc + 0.05f));
      if (mode == 1) lo = std::clamp(lo, 0, size - 1), hi = std::clamp(hi, 0, size - 1);
      return mode <= 1 && lo >= 0 && hi < size;  // repeating past the texture's edge, or a region mode: give up
    };
    float ua, ub, va, vb;
    if (d.fst) {
      ua = float(a.u) / 16, ub = float(b.u) / 16, va = float(a.v) / 16, vb = float(b.v) / 16;
    } else {
      if (a.q <= 0 || b.q <= 0) return false;
      ua = a.s / a.q * float(d.tw), ub = b.s / b.q * float(d.tw), va = a.t / a.q * float(d.th), vb = b.t / b.q * float(d.th);
    }
    const float xa = float(a.x) / 16, xb = float(b.x) / 16, ya = float(a.y) / 16, yb = float(b.y) / 16;
    // Columns read by the sprite's pixels: the rightmost pixel reading each column of the sprite.
    std::vector<int> lastReader(size_t(x1 - x0 + 1), INT32_MIN);
    for (int x = x0; x <= x1; ++x) {
      int lo, hi;
      if (!axis(xa, xb, ua, ub, d.tw, d.wms, x, lo, hi)) return false;
      for (int tx = std::max(lo, x0); tx <= std::min(hi, x1); ++tx) lastReader[size_t(tx - x0)] = std::max(lastReader[size_t(tx - x0)], x);
    }
    bool readsInside = false;  // some pixel reads a column of the sprite
    for (int v : lastReader) readsInside |= v != INT32_MIN;
    if (!readsInside) return true;
    for (int y = y0; y <= y1; ++y) {
      int lo, hi;
      if (!axis(ya, yb, va, vb, d.th, d.wmt, y, lo, hi)) return false;
      for (int ty = std::max(lo, y0); ty <= std::min(hi, y1); ++ty) {
        if (ty < y) return false;  // a row written already
        if (ty == y)               // this row: a column must be read before it is written
          for (int tx = x0; tx <= x1; ++tx)
            if (lastReader[size_t(tx - x0)] > tx) return false;
      }
    }
    return true;
  }
  // The pixels a sprite or triangle covers (left and top edges in, right and bottom out); other
  // primitives' box.
  static void coverage(const Gs::Command& c, int& x0, int& y0, int& x1, int& y1) {
    bounds(c, x0, y0, x1, y1);
    if (c.type < 3) return;
    const int n = c.type == 6 ? 2 : 3;
    int ax = INT32_MAX, ay = INT32_MAX, bx = INT32_MIN, by = INT32_MIN;
    for (int i = 0; i < n; ++i)
      ax = std::min(ax, (c.v[i].x + 15) >> 4), ay = std::min(ay, (c.v[i].y + 15) >> 4), bx = std::max(bx, ((c.v[i].x + 15) >> 4) - 1),
      by = std::max(by, ((c.v[i].y + 15) >> 4) - 1);
    x0 = std::max(x0, ax), y0 = std::max(y0, ay), x1 = std::min(x1, bx), y1 = std::min(y1, by);
  }
  // fastPath; a texture not read from where the primitive draws; and no merely unbiased blend of
  // what is drawn from a surface: effects that blend a picture back into itself frame after frame
  // would drift (where Cs equals Cd the GS's result does not change, the correction's would). A
  // paletted texture is no such picture, wherever its pages have been.
  bool fast(const Gs::Command& c) const {
    const Gs::Draw& d = c.d;
    if (!fastPath(d)) return false;
    if (!d.tme) return true;
    if (d.texFeedback) {
      int x0, y0, x1, y1;
      coverage(c, x0, y0, x1, y1);
      if (readsDrawn(c, x0, y0, x1, y1) && !readsAhead(c)) return false;
    }
    return !d.blend || exactBlend(d) || readsDestination(c) || paletted(d.tpsm) || (texPages(c) & rendered).none();
  }
  UINT vertexOffset = 0;  // of this batch's vertices in the ring
  // The vertex input and rasterizer for drawing primitives (after any surface syncs, which use
  // the pipeline themselves).
  void bindDrawInput(bool fast = false) {
    const UINT stride = sizeof(Vtx);
    ctx->IASetInputLayout((fast ? fastLayout : layout).Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, vertices.buffer.GetAddressOf(), &stride, &vertexOffset);
    ctx->VSSetShader((fast ? fastVs : drawVs).Get(), nullptr, 0);
    ctx->RSSetState(rasterizer.Get());
    const D3D11_VIEWPORT vp{0, 0, 2048.0f * scale, 2048.0f * scale, 0, 1};
    ctx->RSSetViewports(1, &vp);
  }
  Pages regionOwnedBy(int surface) const {
    Pages out;
    for (size_t p = 0; p < 512; ++p)
      if (owner[p] == surface) out.set(p);
    return out;
  }

  // The picture: computed into a texture, then shown or read back.
  ComPtr<ID3D11Texture2D> picture, staging;
  ComPtr<ID3D11UnorderedAccessView> pictureUav;
  ComPtr<ID3D11ShaderResourceView> pictureSrv;
  uint32_t pictureW = 0, pictureH = 0;  // in pixels of the texture

  HWND hwnd = nullptr;
  ComPtr<IDXGISwapChain1> swapChain;
  ComPtr<ID3D11RenderTargetView> backBuffer;
  UINT backW = 0, backH = 0;

  bool init(void* window, std::string& error);
  // Copies `data` into the ring; `at` gets its byte offset there.
  bool fill(Dynamic& d, const void* data, size_t bytes, UINT bind, UINT misc, UINT stride, size_t& at);
  void setConstants(const void* data, size_t bytes);
  void dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* srv0, ID3D11UnorderedAccessView* uav1, uint32_t threads);
  bool makePicture(const Gs::Display& display, uint32_t pictureScale);

  static Pages pagesOf(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1);
  static Pages wholePages(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1);
  void memRead(const Pages& pages);   // GS memory must hold the newest pixels of these pages
  void memWrite(const Pages& pages, bool counted = true);  // ... and is about to be written there
  void flush(int surface);            // a surface's newer pages into GS memory
  int surfaceFor(uint32_t bp, uint32_t bw, uint32_t psm, bool depth, uint32_t width, uint32_t height, bool count = false);
  static bool counting(const Gs::Draw& d);
  static bool exactBlend(const Gs::Draw& d);
  static bool paletted(uint32_t psm) { return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; }
  // (Cs or 0 - Cd) * C >> 7 + Cd, C the source alpha, FIX or the destination's alpha.
  static bool weightedBlend(const Gs::Draw& d) {
    return d.blend && (d.ba == 0 || d.ba == 2) && d.bb == 1 && d.bd == 1 && (d.bc == 1 || d.colclamp) && !d.pabe;
  }
  // Whether a primitive's blend (and destination alpha test) is done in the shader, reading the
  // destination from a copy: it tests the destination's alpha, C is the destination's alpha, or it blends a picture just drawn (see fast()), or it is a 16-bit frame buffer
  // (whose pixels lose their low bits). One copy serves a run, so its primitives must not overlap.
  bool readsDestination(const Gs::Command& c) const {
    const Gs::Draw& d = c.d;
    if (c.type != 6 || (d.blend && d.pabe)) return false;
    if (d.date) return true;
    if (!d.blend) return false;
    return d.bc == 1 || (weightedBlend(d) && (d.fb16 || (d.tme && !paletted(d.tpsm) && (texPages(c) & rendered).any())));
  }
  // The blend's result can go below 0, which signed surfaces do not clamp by themselves.
  static bool subtracts(const Gs::Draw& d);
  int textureSurface(const std::vector<Gs::Command>& commands, size_t first, size_t end, const Pages& reads) const;
  Pages pagesOf(const Surface& s) const { return pagesOf(s.bp, s.bw, s.psm, 0, 0, int(s.width) - 1, int(s.height) - 1); }
  // The samples of a surface that these pages cover, [x0, x1) x [y0, y1); empty if none.
  struct Rect {
    uint32_t x0, y0, x1, y1;
  };
  Rect rectOf(const Surface& s, const Pages& pages) const;
  void use(int surface, const Pages& drawn, const Pages& replaced = {});  // about to draw these pages into it: it must hold them, but for those the draw replaces entirely
  ID3D11BlendState* blendState(const Gs::Draw& d, UINT8 writeMask, bool inShader);
  ID3D11DepthStencilState* depthState(const Gs::Draw& d, bool write);
  static bool fastBlend(const Gs::Draw& d, D3D11_RENDER_TARGET_BLEND_DESC* out);
  static bool fastPath(const Gs::Draw& d);
};

bool GsGpu::Impl::init(void* window, std::string& error) {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  const UINT flags = 0;  // D3D11_CREATE_DEVICE_DEBUG needs the Graphics Tools feature
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    error = "no Direct3D 11 device";
    return false;
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2{};
  if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2))) || !options2.ROVsSupported) {
    error = "the graphics card has no rasterizer-ordered views";
    return false;
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  noOverwriteSrv = SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
                   options.MapNoOverwriteOnDynamicBufferSRV;
  D3D11_FEATURE_DATA_DOUBLES doubles{};
  if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles))) || !doubles.DoublePrecisionFloatShaderOps) {
    error = "the graphics card has no double-precision shaders";
    return false;
  }

  // GS memory, one layer per sample of a pixel (and a spare word for 24-bit reads at the end), zeroed.
  {
    std::vector<uint32_t> zero(size_t(GsMemory::kSize / 4) * scale * scale + 4, 0);
    D3D11_BUFFER_DESC bd{UINT(zero.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    D3D11_SUBRESOURCE_DATA init{zero.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &init, &mem))) return error = "GS memory buffer", false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS, ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = UINT(zero.size()), ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(mem.Get(), &ud, &memUav);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS, sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = UINT(zero.size()), sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    dev->CreateShaderResourceView(mem.Get(), &sd, &memSrv);
  }
  // Swizzle tables.
  {
    ComPtr<ID3D11Buffer> swz;
    D3D11_BUFFER_DESC bd{UINT((sizeof(gsmem_detail::kSwizzle) + 3) & ~3u), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    std::vector<uint8_t> bytes(bd.ByteWidth, 0);
    std::memcpy(bytes.data(), &gsmem_detail::kSwizzle, sizeof(gsmem_detail::kSwizzle));
    D3D11_SUBRESOURCE_DATA init{bytes.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &init, &swz))) return error = "swizzle buffer", false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS, sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = bd.ByteWidth / 4, sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    dev->CreateShaderResourceView(swz.Get(), &sd, &swzSrv);
  }
  // Palettes, and the copy's temporary pixels (2048 x 2048 at most).
  auto structured = [&](UINT count, ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11UnorderedAccessView>& uav,
                        ComPtr<ID3D11ShaderResourceView>* srv) {
    std::vector<uint32_t> zero(count, 0);
    D3D11_BUFFER_DESC bd{count * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
    D3D11_SUBRESOURCE_DATA init{zero.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &init, &buffer))) return false;
    dev->CreateUnorderedAccessView(buffer.Get(), nullptr, &uav);
    if (srv) dev->CreateShaderResourceView(buffer.Get(), nullptr, srv->ReleaseAndGetAddressOf());
    return true;
  };
  if (!structured(kClutSlots * 256, cluts, clutUav, &clutSrv) || !structured(2048 * 2048, copyTmp, copyTmpUav, nullptr))
    return error = "palette buffers", false;

  // Shaders.
  const std::string scaleDefine = std::format("#define SCALE {}\n", scale);
  const std::string common = scaleDefine + gpu_hlsl::kCommon;
  auto with = [&](const char* memDecl, const char* body) { return std::string(memDecl) + common + body; };
  const char* rov = "RasterizerOrderedByteAddressBuffer gsMem : register(u1);\n#define MEM gsMem\n";  // u0: the render target
  const char* rw = "RWByteAddressBuffer gsMem : register(u0);\n#define MEM gsMem\n#define WRITABLE\n";
  const char* ro = "ByteAddressBuffer gsMem : register(t0);\n#define MEM gsMem\n";
  ComPtr<ID3DBlob> vsCode = compile(scaleDefine + gpu_hlsl::kDrawVs, "vs_5_0", error);
  if (!vsCode) return false;
  ComPtr<ID3DBlob> psCode = compile(with(rov, (std::string(gpu_hlsl::kDrawShared) + gpu_hlsl::kDrawPs).c_str()), "ps_5_0", error);
  if (!psCode) return false;
  if (FAILED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &drawVs)) ||
      FAILED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &drawPs)))
    return error = "the drawing shaders", false;
  const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"PRIM", 0, DXGI_FORMAT_R32_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"DEPTH", 0, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOUR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"STQF", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(dev->CreateInputLayout(elements, 2, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout)))
    return error = "the vertex layout", false;
  ComPtr<ID3DBlob> fastVsCode = compile(scaleDefine + gpu_hlsl::kFastVs, "vs_5_0", error);
  if (!fastVsCode || FAILED(dev->CreateVertexShader(fastVsCode->GetBufferPointer(), fastVsCode->GetBufferSize(), nullptr, &fastVs)) ||
      FAILED(dev->CreateInputLayout(elements, 6, fastVsCode->GetBufferPointer(), fastVsCode->GetBufferSize(), &fastLayout)))
    return error = "the hardware path's vertex shader", false;
  struct Cs {
    const char* decl;
    const char* body;
    ComPtr<ID3D11ComputeShader>* out;
  };
  // The upload's source is t0, so memory there is the UAV; the picture reads memory at t0.
  for (const Cs& cs : {Cs{rw, gpu_hlsl::kUploadCs, &uploadCs}, Cs{rw, gpu_hlsl::kCopyCs, &copyCs}, Cs{rw, gpu_hlsl::kClutCs, &clutCs},
                       Cs{ro, gpu_hlsl::kPictureCs, &pictureCs}, Cs{ro, gpu_hlsl::kSurfacePictureCs, &surfacePictureCs}}) {
    ComPtr<ID3DBlob> code = compile(with(cs.decl, cs.body), "cs_5_0", error);
    if (!code) return false;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs.out->ReleaseAndGetAddressOf())))
      return error = "a compute shader", false;
  }
  const char* roT3 =
      "ByteAddressBuffer gsMem : register(t3);\n#define MEM gsMem\n#define SURFACE_TEXTURE\n"
      "Texture2D<float4> texSurface : register(t4);\ncbuffer TexSource : register(b1) { uint texFromSurface; };\n";
  const std::string fastBody = std::string(gpu_hlsl::kDrawShared) + gpu_hlsl::kFastPs;
  ComPtr<ID3DBlob> fastCode = compile(with(roT3, ("#define CACHED_TEXTURES\n#define EXPORT_DEPTH 0\n" + fastBody).c_str()), "ps_5_0", error);
  ComPtr<ID3DBlob> fastDepthCode = compile(with(roT3, ("#define CACHED_TEXTURES\n#define EXPORT_DEPTH 1\n" + fastBody).c_str()), "ps_5_0", error);
  ComPtr<ID3DBlob> decodeCode = compile(with(roT3, (std::string(gpu_hlsl::kDrawShared) + gpu_hlsl::kDecodeCs).c_str()), "cs_5_0", error);
  if (!decodeCode || FAILED(dev->CreateComputeShader(decodeCode->GetBufferPointer(), decodeCode->GetBufferSize(), nullptr, &decodeCs)))
    return error = "the texture decoder", false;
  ComPtr<ID3DBlob> depthCode = compile(with(ro, gpu_hlsl::kMemToDepthPs), "ps_5_0", error);
  if (!fastCode || !fastDepthCode || !depthCode) return false;
  dev->CreatePixelShader(fastCode->GetBufferPointer(), fastCode->GetBufferSize(), nullptr, &fastPs);
  dev->CreatePixelShader(fastDepthCode->GetBufferPointer(), fastDepthCode->GetBufferSize(), nullptr, &fastDepthPs);
  if (scale > 1) {
    ComPtr<ID3DBlob> lowCode = compile(with(roT3, ("#define CACHED_TEXTURES\n#define EXPORT_DEPTH 0\n#define LOWRES\n" + fastBody).c_str()), "ps_5_0", error);
    ComPtr<ID3DBlob> overCode = compile(scaleDefine + gpu_hlsl::kLowResOverPs, "ps_5_0", error);
    if (!lowCode || !overCode) return false;
    dev->CreatePixelShader(lowCode->GetBufferPointer(), lowCode->GetBufferSize(), nullptr, &lowResPs);
    dev->CreatePixelShader(overCode->GetBufferPointer(), overCode->GetBufferSize(), nullptr, &lowResOverPs);
  }
  dev->CreatePixelShader(depthCode->GetBufferPointer(), depthCode->GetBufferSize(), nullptr, &memToDepthPs);
  const std::string countSource = std::string("#define COUNT 1\n") + gpu_hlsl::kMemToSurfaceCs;
  for (const Cs& cs : {Cs{rw, gpu_hlsl::kSurfaceToMemCs, &surfaceToMemCs}, Cs{ro, gpu_hlsl::kMemToSurfaceCs, &memToSurfaceCs},
                       Cs{ro, countSource.c_str(), &memToCountCs}}) {
    ComPtr<ID3DBlob> code = compile(with(cs.decl, cs.body), "cs_5_0", error);
    if (!code) return false;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs.out->ReleaseAndGetAddressOf())))
      return error = "a compute shader", false;
  }
  owner.fill(-1);
  ComPtr<ID3DBlob> pvs = compile(gpu_hlsl::kPresentVs, "vs_5_0", error), pps = compile(gpu_hlsl::kPresentPs, "ps_5_0", error);
  if (!pvs || !pps) return false;
  dev->CreateVertexShader(pvs->GetBufferPointer(), pvs->GetBufferSize(), nullptr, &presentVs);
  dev->CreatePixelShader(pps->GetBufferPointer(), pps->GetBufferSize(), nullptr, &presentPs);
  ComPtr<ID3DBlob> fps = compile(gpu_hlsl::kFpsPs, "ps_5_0", error);
  if (!fps) return false;
  dev->CreatePixelShader(fps->GetBufferPointer(), fps->GetBufferSize(), nullptr, &fpsPs);
  ComPtr<ID3DBlob> mvs = compile(gpu_hlsl::kMapVs, "vs_5_0", error), mps = compile(gpu_hlsl::kMapPs, "ps_5_0", error), mos = compile(gpu_hlsl::kMapOverPs, "ps_5_0", error);
  if (!mvs || !mps || !mos) return false;
  dev->CreateVertexShader(mvs->GetBufferPointer(), mvs->GetBufferSize(), nullptr, &mapVs);
  dev->CreatePixelShader(mps->GetBufferPointer(), mps->GetBufferSize(), nullptr, &mapPs);
  dev->CreatePixelShader(mos->GetBufferPointer(), mos->GetBufferSize(), nullptr, &mapOverPs);
  {
    D3D11_BLEND_DESC bd{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE, rt.SrcBlend = D3D11_BLEND_SRC_ALPHA, rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE, rt.DestBlendAlpha = D3D11_BLEND_ZERO, rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &overBlend);
  }

  {
    ComPtr<ID3D11Texture2D> t;
    D3D11_TEXTURE2D_DESC td{2048 * scale, 2048 * scale, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t))) return error = "render target", false;
    dev->CreateRenderTargetView(t.Get(), nullptr, &dummyTarget);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = 0;
    dev->CreateBlendState(&bd, &noWrites);
  }
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = FALSE;
  dev->CreateRasterizerState(&rd, &rasterizer);
  rd.ScissorEnable = TRUE;
  dev->CreateRasterizerState(&rd, &scissored);
  {
    D3D11_DEPTH_STENCIL_DESC dd{};  // no test, no writes
    dev->CreateDepthStencilState(&dd, &noDepth);
  }
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  dev->CreateSamplerState(&sd, &pointSampler);
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  dev->CreateSamplerState(&sd, &linearSampler);
  for (int i = 0; i < 4; ++i) {
    D3D11_SAMPLER_DESC ts{};
    ts.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ts.AddressU = (i & 1) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    ts.AddressV = (i & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    ts.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ts.MaxLOD = 0;
    dev->CreateSamplerState(&ts, &texSamplers[i]);
  }
  D3D11_BUFFER_DESC cb{256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  dev->CreateBuffer(&cb, nullptr, &constants);
  dev->CreateBuffer(&cb, nullptr, &primStateBuffer);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t v[4] = {i};
    D3D11_BUFFER_DESC tb{16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init{v, 0, 0};
    dev->CreateBuffer(&tb, &init, &texSource[i]);
  }

  // The window's swap chain.
  hwnd = static_cast<HWND>(window);
  if (hwnd) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    dev.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, scd.SampleDesc.Count = 1, scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2, scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;  // interpolation's pace
    if (FAILED(factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &scd, nullptr, nullptr, &swapChain)))
      return error = "no swap chain for the window", false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (ComPtr<IDXGISwapChain2> chain2; SUCCEEDED(swapChain.As(&chain2))) {
      chain2->SetMaximumFrameLatency(1);
      refreshWait = chain2->GetFrameLatencyWaitableObject();
    }
  }
  return true;
}

bool GsGpu::Impl::fill(Dynamic& d, const void* data, size_t bytes, UINT bind, UINT misc, UINT stride, size_t& at) {
  if (bytes == 0) return false;
  const size_t align = stride ? stride : 16;
  at = (d.head + align - 1) / align * align;
  D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (d.bytes < bytes || at + bytes > d.bytes || ((bind & D3D11_BIND_SHADER_RESOURCE) && !noOverwriteSrv))
    at = 0, mode = D3D11_MAP_WRITE_DISCARD;
  if (d.bytes < bytes) {
    size_t size = 4 << 20;
    while (size < bytes) size *= 2;
    if (stride) size = size / stride * stride + stride;  // structured buffers hold whole elements
    D3D11_BUFFER_DESC bd{UINT(size), D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE, misc, stride};
    d.buffer.Reset(), d.srv.Reset();
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &d.buffer))) return d.bytes = 0, false;
    d.bytes = size;
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      if (misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
        sd.Format = DXGI_FORMAT_UNKNOWN, sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER, sd.Buffer.NumElements = UINT(size / stride);
      } else {
        sd.Format = DXGI_FORMAT_R32_TYPELESS, sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = UINT(size / 4), sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      }
      dev->CreateShaderResourceView(d.buffer.Get(), &sd, &d.srv);
    }
  }
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(d.buffer.Get(), 0, mode, 0, &m))) return false;
  std::memcpy(static_cast<uint8_t*>(m.pData) + at, data, bytes);
  ctx->Unmap(d.buffer.Get(), 0);
  d.head = at + bytes;
  return true;
}

void GsGpu::Impl::setConstants(const void* data, size_t bytes) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  std::memcpy(m.pData, data, std::min<size_t>(bytes, 256));
  ctx->Unmap(constants.Get(), 0);
}

// ---- The hardware path: surfaces and the pages they hold ----------------------------------------

GsGpu::Impl::Pages GsGpu::Impl::pagesOf(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1) {
  Pages out;
  x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, 2047), y1 = std::min(y1, 2047);
  if (x1 < x0 || y1 < y0) return out;
  uint32_t pw = 64, ph = 32, perRow = std::max(1u, bw);  // a page in pixels, pages per buffer row
  switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: ph = 64; break;
    case PSMT8: pw = 128, ph = 64, perRow = std::max(1u, bw / 2); break;
    case PSMT4: pw = 128, ph = 128, perRow = std::max(1u, bw / 2); break;
  }
  // A buffer need not start on a page: then each of its pages spans two of memory's.
  const uint32_t first = bp / 32, spill = bp % 32 ? 1 : 0;
  for (uint32_t py = uint32_t(y0) / ph; py <= uint32_t(y1) / ph; ++py)
    for (uint32_t px = uint32_t(x0) / pw; px <= uint32_t(x1) / pw; ++px) {
      const uint32_t page = first + py * perRow + px;
      out.set(page & 511);
      if (spill) out.set((page + 1) & 511);
    }
  return out;
}

GsGpu::Impl::Rect GsGpu::Impl::rectOf(const Surface& s, const Pages& pages) const {
  const uint32_t pw = 64, ph = s.psm == PSMCT16 || s.psm == PSMCT16S || s.psm == PSMZ16 || s.psm == PSMZ16S ? 64 : 32;
  const uint32_t perRow = std::max(1u, s.bw), rows = (s.height + ph - 1) / ph;
  uint32_t x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0;
  for (size_t p = 0; p < 512; ++p) {
    if (!pages[p]) continue;
    // The buffer's page there, or (when it starts off a page) the one before spilling into it.
    const uint32_t o = (uint32_t(p) - s.bp / 32) & 511;
    for (uint32_t k : {o, (o - 1) & 511}) {
      if (k == o || s.bp % 32) {
        const uint32_t py = k / perRow, px = k % perRow;
        if (py >= rows) continue;
        x0 = std::min(x0, px * pw), y0 = std::min(y0, py * ph), x1 = std::max(x1, (px + 1) * pw), y1 = std::max(y1, (py + 1) * ph);
      }
    }
  }
  if (x0 >= x1) return {0, 0, 0, 0};
  return {x0 * scale, y0 * scale, std::min(x1, s.width) * scale, std::min(y1, s.height) * scale};
}

// The pages a rectangle covers entirely.
GsGpu::Impl::Pages GsGpu::Impl::wholePages(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1) {
  Pages out;
  if (bp % 32 || x0 < 0 || y0 < 0) return out;
  uint32_t pw = 64, ph = 32, perRow = std::max(1u, bw);
  switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: ph = 64; break;
    case PSMT8: pw = 128, ph = 64, perRow = std::max(1u, bw / 2); break;
    case PSMT4: pw = 128, ph = 128, perRow = std::max(1u, bw / 2); break;
  }
  for (uint32_t py = (uint32_t(y0) + ph - 1) / ph; (py + 1) * ph - 1 <= uint32_t(y1); ++py)
    for (uint32_t px = (uint32_t(x0) + pw - 1) / pw; (px + 1) * pw - 1 <= uint32_t(x1) && px < perRow; ++px)
      out.set((bp / 32 + py * perRow + px) & 511);
  return out;
}

void GsGpu::Impl::flush(int index) {
  Surface& sf = surfaces[size_t(index)];
  Pages mask;
  for (size_t p = 0; p < 512; ++p)
    if (owner[p] == index && memStale[p]) mask.set(p);
  if (mask.none()) return;
  struct {
    uint32_t bp, bw, psm, kind, width, height;
    float zMul;
    uint32_t pad;  // origin
    uint32_t pages[16];
  } c{sf.bp, sf.bw, sf.psm, sf.depth ? 1u : sf.count ? 2u : 0u, sf.width * scale, sf.height * scale, 0, 0, {}};
  const Rect r = rectOf(sf, mask);
  if (r.x1 <= r.x0 || r.y1 <= r.y0) {
    memStale &= ~mask;
    return;
  }
  c.pad = r.x0 | r.y0 << 16;
  c.zMul = sf.psm == PSMZ16 || sf.psm == PSMZ16S ? 65535.0f : sf.psm == PSMZ24 ? 16777216.0f : 4294967296.0f;
  for (size_t p = 0; p < 512; ++p)
    if (mask[p]) c.pages[p >> 5] |= 1u << (p & 31);
  setConstants(&c, sizeof(c));
  ID3D11ShaderResourceView* srvs[2] = {sf.srv.Get(), swzSrv.Get()};
  ctx->CSSetShader(surfaceToMemCs.Get(), nullptr, 0);
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, memUav.GetAddressOf(), nullptr);
  ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
  {
    Busy busy(*this, kFlush);
    ctx->Dispatch((r.x1 - r.x0 + 7) / 8, (r.y1 - r.y0 + 7) / 8, 1);
  }
  ID3D11ShaderResourceView* noSrvs[2] = {};
  ID3D11UnorderedAccessView* noUav = nullptr;
  ctx->CSSetShaderResources(0, 2, noSrvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  memStale &= ~mask;
  ++syncs;
}

void GsGpu::Impl::memRead(const Pages& pages) {
  const Pages stale = pages & memStale;
  if (stale.none()) return;
  for (size_t p = 0; p < 512; ++p)
    if (stale[p] && memStale[p] && owner[p] >= 0) flush(owner[p]);
}

void GsGpu::Impl::memWrite(const Pages& pages, bool counted) {
  memRead(pages);
  if (counted) wrote(pages);
  for (size_t p = 0; p < 512; ++p)
    if (pages[p] && owner[p] >= 0) ownerStale.set(p);
}

int GsGpu::Impl::surfaceFor(uint32_t bp, uint32_t bw, uint32_t psm, bool depth, uint32_t width, uint32_t height, bool count) {
  width = std::clamp(std::max(width, bw * 64), 64u, 2048u);
  height = std::clamp((height + 31) / 32 * 32, 32u, 2048u);  // no further than drawn: beyond may be another buffer
  int index = -1;
  for (size_t i = 0; i < surfaces.size(); ++i) {
    const Surface& sf = surfaces[i];
    if (sf.bp == bp && sf.bw == bw && sf.psm == psm && sf.depth == depth && sf.count == count) index = int(i);
  }
  if (index >= 0) {
    Surface& sf = surfaces[size_t(index)];
    if (sf.width >= width && sf.height >= height) return index;
    // Too small: its pixels go to memory and it starts again, larger, from there.
    flush(index);
    for (size_t p = 0; p < 512; ++p)
      if (owner[p] == index) owner[p] = -1, ownerStale.reset(p), held.reset(p);
    width = std::max(width, sf.width), height = std::max(height, sf.height);
  } else {
    index = int(surfaces.size());
    surfaces.push_back({});
  }
  Surface& sf = surfaces[size_t(index)];
  sf = Surface{bp, bw, psm, depth, count, width, height};
  const bool depth16 = psm == PSMZ16 || psm == PSMZ16S;
  D3D11_TEXTURE2D_DESC td{width * scale, height * scale, 1, 1,
                          depth ? (depth16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R32_TYPELESS) : count ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_SNORM, {1, 0},
                          D3D11_USAGE_DEFAULT,
                          UINT(depth ? D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE
                                     : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS),
                          0, 0};
  dev->CreateTexture2D(&td, nullptr, &sf.tex);
  if (depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = depth16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT, dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(sf.tex.Get(), &dd, &sf.dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = depth16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT, sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(sf.tex.Get(), &sd, &sf.srv);
  } else {
    dev->CreateRenderTargetView(sf.tex.Get(), nullptr, &sf.rtv);
    dev->CreateShaderResourceView(sf.tex.Get(), nullptr, &sf.srv);
    dev->CreateUnorderedAccessView(sf.tex.Get(), nullptr, &sf.uav);
  }
  return index;
}

void GsGpu::Impl::use(int index, const Pages& drawn, const Pages& replaced) {
  // The pages it must take over: those someone else holds, or it holds older than memory.
  Pages need;
  for (size_t p = 0; p < 512; ++p)
    if (drawn[p] && (owner[p] != index || ownerStale[p])) need.set(p);
  if (need.none()) return;
  // What the draw will replace entirely need not come here first.
  const Pages taken = need & replaced;
  if (taken.any()) {
    memStale &= ~taken;
    for (size_t p = 0; p < 512; ++p)
      if (taken[p]) owner[p] = index, ownerStale.reset(p);
    need &= ~taken;
    if (need.none()) return;
  }
  memRead(need);  // the newest pixels of those pages into memory
  const Surface& sf = surfaces[size_t(index)];
  const uint32_t w = sf.width * scale, h = sf.height * scale;
  const Rect r = rectOf(sf, need);
  uint32_t mask[16] = {};
  for (size_t p = 0; p < 512; ++p)
    if (need[p]) mask[p >> 5] |= 1u << (p & 31);
  if (!sf.depth) {
    uint32_t c[24] = {sf.bp, sf.bw, sf.psm, w, h, r.x0 | r.y0 << 16};
    std::memcpy(c + 8, mask, sizeof(mask));
    setConstants(c, sizeof(c));
    ID3D11ShaderResourceView* srvs[2] = {memSrv.Get(), swzSrv.Get()};
    ctx->CSSetShader((sf.count ? memToCountCs : memToSurfaceCs).Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, sf.uav.GetAddressOf(), nullptr);
    ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    Busy busy(*this, kLoad);
    if (r.x1 > r.x0) ctx->Dispatch((r.x1 - r.x0 + 7) / 8, (r.y1 - r.y0 + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrvs[2] = {};
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetShaderResources(0, 2, noSrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  } else {
    struct {
      uint32_t bp, bw, psm;
      float zScale;
      uint32_t pages[16];
    } c{sf.bp, sf.bw, sf.psm, sf.psm == PSMZ16 || sf.psm == PSMZ16S ? 1.0f / 65535 : sf.psm == PSMZ24 ? 1.0f / 16777216 : 1.0f / 4294967296.0f, {}};
    std::memcpy(c.pages, mask, sizeof(mask));
    setConstants(&c, sizeof(c));
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> always;
    dev->CreateDepthStencilState(&dd, &always);
    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    const D3D11_RECT sr{LONG(r.x0), LONG(r.y0), LONG(r.x1), LONG(r.y1)};
    ctx->RSSetScissorRects(1, &sr);
    ctx->RSSetState(scissored.Get());
    ctx->OMSetRenderTargets(0, nullptr, sf.dsv.Get());
    ctx->OMSetDepthStencilState(always.Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(presentVs.Get(), nullptr, 0);
    ctx->PSSetShader(memToDepthPs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {memSrv.Get(), swzSrv.Get()};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    Busy busy(*this, kLoad);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* noSrvs[2] = {};
    ctx->PSSetShaderResources(0, 2, noSrvs);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->OMSetDepthStencilState(nullptr, 0);
  }
  for (size_t p = 0; p < 512; ++p)
    if (need[p]) owner[p] = index, ownerStale.reset(p), held.set(p);
  ++syncs;
}

// The GS blend ((A - B) * C >> 7) + D as the GPU's: each of Cs and Cd ends up with a factor
// k1 * C + k0, which must be one the GPU has (C reaches it as the second output's alpha).
bool GsGpu::Impl::fastBlend(const Gs::Draw& d, D3D11_RENDER_TARGET_BLEND_DESC* out) {
  if (!d.blend) {
    if (out) out->BlendEnable = FALSE;
    return true;
  }
  if (d.bc == 1 && !out) return !d.pabe;  // readsDestination decides
  if (d.pabe || (d.bc != 1 && !d.colclamp)) return false;  // per-pixel or wrapping blends
  auto coefficient = [&](int who, int& k1, int& k0) {
    k1 = (d.ba == who ? 1 : 0) - (d.bb == who ? 1 : 0);
    k0 = d.bd == who ? 1 : 0;
  };
  int s1, s0, d1, d0;
  coefficient(0, s1, s0);
  coefficient(1, d1, d0);
  // factor, and whether the term is subtracted
  auto factor = [](int k1, int k0, D3D11_BLEND& f, bool& negative) {
    negative = false;
    if (k1 == 0) return f = k0 ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO, true;
    if (k1 == 1 && k0 == 0) return f = D3D11_BLEND_SRC1_ALPHA, true;
    if (k1 == -1 && k0 == 1) return f = D3D11_BLEND_INV_SRC1_ALPHA, true;
    if (k1 == -1 && k0 == 0) return negative = true, f = D3D11_BLEND_SRC1_ALPHA, true;
    return false;  // 1 + C, or -C + ... beyond what one factor gives
  };
  D3D11_BLEND fs, fd;
  bool ns, nd;
  if (!factor(s1, s0, fs, ns) || !factor(d1, d0, fd, nd)) return false;
  if (!out) return true;
  out->BlendEnable = TRUE;
  out->SrcBlend = fs, out->DestBlend = fd;
  out->BlendOp = ns && nd ? D3D11_BLEND_OP_ADD : ns ? D3D11_BLEND_OP_REV_SUBTRACT : nd ? D3D11_BLEND_OP_SUBTRACT : D3D11_BLEND_OP_ADD;
  if (ns && nd) out->SrcBlend = out->DestBlend = D3D11_BLEND_ZERO;  // never above zero: clamped to it
  out->SrcBlendAlpha = D3D11_BLEND_ONE, out->DestBlendAlpha = D3D11_BLEND_ZERO, out->BlendOpAlpha = D3D11_BLEND_OP_ADD;
  return true;
}

// Whether a primitive can take the hardware path: 32 and 24-bit frame buffers, and counting into 16-bit ones.
bool GsGpu::Impl::fastPath(const Gs::Draw& d) {
  static const bool off = std::getenv("CODPORT_GSMEM") != nullptr;  // everything on GS memory, for comparisons
  if (off) return false;
  if (d.selfRead) return false;
  if (d.ztest && d.zpsm != PSMZ32 && d.zpsm != PSMZ24 && d.zpsm != PSMZ16 && d.zpsm != PSMZ16S) return false;
  if (counting(d)) {  // whole channels masked or not (a 16-bit channel is a mask byte's top 5 bits)
    for (int k = 0; k < 4; ++k) {
      const uint32_t m = (d.fbmsk >> (8 * k)) & (k == 3 ? 0x80u : 0xF8u);
      if (m != 0 && m != (k == 3 ? 0x80u : 0xF8u)) return false;
    }
    return true;
  }
  if (d.fb16) return d.fbmsk == 0 && (!d.blend || weightedBlend(d));  // kept in 8 bits a channel until it goes to memory
  if (d.fpsm != PSMCT32 && d.fpsm != PSMCT24) return false;
  if (d.fbmsk != 0 && d.fbmsk != 0xFF000000u && d.fbmsk != 0x00FFFFFFu) return false;
  return exactBlend(d) || fastBlend(d, nullptr);
}

ID3D11BlendState* GsGpu::Impl::blendState(const Gs::Draw& d, UINT8 writeMask, bool inShader) {
  const bool count = counting(d);
  const uint32_t key = uint32_t(d.blend) | uint32_t(d.ba) << 1 | uint32_t(d.bb) << 3 | uint32_t(d.bc) << 5 | uint32_t(d.bd) << 7 |
                       uint32_t(writeMask) << 9 | uint32_t(count) << 13 | uint32_t(inShader) << 14;
  ComPtr<ID3D11BlendState>& state = blendStates[key];
  if (!state) {
    D3D11_BLEND_DESC bd{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
    if ((count && !d.blend) || inShader) {  // a clear; or all done in the shader
      rt.BlendEnable = FALSE;
    } else if (count || (exactBlend(d) && d.bd == 1)) {  // the shader's value added to Cd (or taken from it); alpha replaced
      rt.BlendEnable = TRUE;
      rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
      rt.BlendOp = !count && d.ba == 2 && d.bb == 0 ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_ADD;
      rt.SrcBlendAlpha = D3D11_BLEND_ONE, rt.DestBlendAlpha = D3D11_BLEND_ZERO, rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    } else if (exactBlend(d)) {  // all in the shader
      rt.BlendEnable = FALSE;
    } else {
      fastBlend(d, &rt);
      if (weightedBlend(d)) rt.SrcBlend = D3D11_BLEND_ONE;  // the shader weighs the source
    }
    rt.RenderTargetWriteMask = writeMask;
    dev->CreateBlendState(&bd, &state);
  }
  return state.Get();
}

// The colour surface a run's textures can be read from: one 32 or 24-bit texture, all of whose
// pages that surface holds as they are (else -1: they are read from memory).
int GsGpu::Impl::textureSurface(const std::vector<Gs::Command>& commands, size_t first, size_t end, const Pages& reads) const {
  const Gs::Draw* tex = nullptr;
  for (size_t k = first; k < end; ++k) {
    const Gs::Draw& d = commands[k].d;
    if (!d.tme) continue;
    if (tex && (d.tbp != tex->tbp || d.tbw != tex->tbw || d.tpsm != tex->tpsm)) return -1;
    tex = &d;
  }
  const bool count = tex && (tex->tpsm == PSMCT16 || tex->tpsm == PSMCT16S);
  if (!tex || (!count && tex->tpsm != PSMCT32 && tex->tpsm != PSMCT24)) return -1;
  for (size_t i = 0; i < surfaces.size(); ++i) {
    const Surface& sf = surfaces[i];
    if (sf.depth || sf.count != count || sf.bp != tex->tbp || sf.bw != tex->tbw || sf.psm != tex->tpsm) continue;
    for (size_t p = 0; p < 512; ++p)
      if (reads[p] && (owner[p] != int(i) || ownerStale[p])) return -1;
    return int(i);
  }
  return -1;
}

ID3D11ShaderResourceView* GsGpu::Impl::snapshot(Surface& sf, int x0, int y0, int x1, int y1) {
  if (!sf.copy) {
    D3D11_TEXTURE2D_DESC td;
    sf.tex->GetDesc(&td);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &sf.copy);
    dev->CreateShaderResourceView(sf.copy.Get(), nullptr, &sf.copySrv);
  }
  const D3D11_BOX box{UINT(std::max(x0, 0)) * scale, UINT(std::max(y0, 0)) * scale, 0, std::min(UINT(x1 + 1), sf.width) * scale,
                      std::min(UINT(y1 + 1), sf.height) * scale, 1};
  if (box.right > box.left && box.bottom > box.top)
    ctx->CopySubresourceRegion(sf.copy.Get(), 0, box.left, box.top, 0, sf.tex.Get(), 0, &box);
  return sf.copySrv.Get();
}

// A blend where Cd is at most added (D) and C is not the destination alpha: the shader computes the
// rest as the GS does, and the GPU adds it to Cd (or subtracts it) exactly.
bool GsGpu::Impl::exactBlend(const Gs::Draw& d) {
  if (!d.blend || d.fb16 || d.pabe || d.ba == 1 || d.bb == 1 || d.bc == 1 || d.ba == 3 || d.bb == 3 || d.bd == 3) return false;
  return d.bd != 1 || d.colclamp;  // the GPU's sum clamps (and subtracts() below 0)
}

// The additive FIX 128 blend without clamping into a 16-bit frame buffer: each 5-bit channel
// gains Cs >> 3, wrapping (Cd's low bits are zero, so nothing carries between them).
// ... and what clears such a buffer: a flat colour written over it.
bool GsGpu::Impl::counting(const Gs::Draw& d) {
  if (d.fb16 && !d.blend && !d.tme) return true;
  return d.fb16 && d.blend && !d.colclamp && !d.pabe && d.ba == 0 && d.bb == 2 && d.bc == 2 && d.fix == 128 && d.bd == 1;
}

bool GsGpu::Impl::subtracts(const Gs::Draw& d) {
  if (!d.blend || d.fb16) return false;
  return (d.ba == 2 && d.bb == 0) || (d.bb == 1 && d.bd != 1 && d.ba != 1);
}

ID3D11DepthStencilState* GsGpu::Impl::depthState(const Gs::Draw& d, bool write) {
  const uint32_t key = uint32_t(d.ztest) | d.ztst << 1 | uint32_t(write) << 3;
  ComPtr<ID3D11DepthStencilState>& state = depthStates[key];
  if (!state) {
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = d.ztest;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    static constexpr D3D11_COMPARISON_FUNC kFunc[4] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS,
                                                       D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_GREATER};
    dd.DepthFunc = kFunc[d.ztst & 3];
    dev->CreateDepthStencilState(&dd, &state);
  }
  return state.Get();
}

// A compute shader over GS memory (u0) with one thread per item.
void GsGpu::Impl::dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* srv0, ID3D11UnorderedAccessView* uav1,
                           uint32_t threads) {
  if (threads == 0) return;
  ID3D11ShaderResourceView* srvs[2] = {srv0, swzSrv.Get()};
  ID3D11UnorderedAccessView* uavs[2] = {memUav.Get(), uav1};
  ctx->CSSetShader(cs, nullptr, 0);
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
  ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
  const uint32_t groups = (threads + 63) / 64;  // 64 per group, 1024 groups per row
  ctx->Dispatch(std::min(groups, 1024u), (groups + 1023) / 1024, 1);
  ID3D11ShaderResourceView* noSrvs[2] = {};
  ID3D11UnorderedAccessView* noUavs[2] = {};
  ctx->CSSetShaderResources(0, 2, noSrvs);
  ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
}

std::unique_ptr<GsGpu> GsGpu::create(void* window, uint32_t scale, std::string& error) {
  std::unique_ptr<GsGpu> gpu(new GsGpu);
  gpu->impl_ = std::make_unique<Impl>();
  gpu->impl_->scale = std::clamp(scale, 1u, 4u);
  if (!gpu->impl_->init(window, error)) return nullptr;
  if (window) {  // texture packs in textures/ next to the memory card folder (not in headless runs)
    std::filesystem::path dir = iconFile().parent_path();
    if (dir.filename() == "build") dir = dir.parent_path();
    gpu->impl_->pack.open(dir / "textures", settings().texturePack, settings().dumpTextures);
  }
  return gpu;
}

GsGpu::~GsGpu() {
  if (impl_) onRefresh({});
  if (impl_ && std::getenv("CODPORT_GPU_STATS"))
    std::printf("   GPU: %llu hardware draw calls (%llu runs at the GS's resolution), %llu GS-memory ones, %llu surface syncs, %zu surfaces\n",
                (unsigned long long)impl_->fastDraws, (unsigned long long)impl_->lowResRuns, (unsigned long long)impl_->slowDraws, (unsigned long long)impl_->syncs,
                impl_->surfaces.size());
}

void GsGpu::draw(const std::vector<Gs::Command>& commands) {
  Impl& m = *impl_;
  std::vector<GpuPrim>& prims = m.primList;
  std::vector<Vtx>& verts = m.vertexList;
  std::vector<uint32_t>& firstVertex = m.firstVertex;  // of each command, and the end
  prims.clear(), verts.clear(), firstVertex.clear();
  const float scale = float(m.scale);
  std::vector<uint8_t>& ways = m.ways;
  ways.resize(commands.size());
  for (size_t k = 0; k < commands.size(); ++k) {
    ways[k] = m.fast(commands[k]);
    if (ways[k] && m.readsDestination(commands[k])) ways[k] = 2;
    if (ways[k] == 1 && (commands[k].d.date || (commands[k].d.blend && (commands[k].d.bc == 1 || commands[k].d.fb16) && !Impl::counting(commands[k].d)))) ways[k] = 0;
  }
  // On the hardware path the vertices carry what varies: equal draws in a row share one state.
  const Gs::Draw* sharedDraw = nullptr;
  uint32_t sharedId = 0;
  m.commandState.resize(commands.size());
  m.states.clear();
  m.stateTex.clear(), m.stateRun.clear();
  auto shared = [&](const Gs::Draw& d) {
    if (!sharedDraw || std::memcmp(sharedDraw, &d, sizeof(d)) != 0) {
      sharedDraw = &d, sharedId = uint32_t(m.states.size());
      m.states.push_back(Impl::stateOf(d));
    }
    return sharedId;
  };
  for (size_t k = 0; k < commands.size(); ++k) {
    const Gs::Command& c = commands[k];
    firstVertex.push_back(uint32_t(verts.size()));
    const Gs::Draw& d = c.d;
    const bool hardware = ways[k] != 0;
    switch (c.type) {
      case 0: {  // point: the pixel its centre rounds to
        const Gs::Vertex& v = c.v[0];
        const float x = float((v.x + 8) >> 4), y = float((v.y + 8) >> 4);
        const Attr at{float(std::min(v.z, d.zmax)), float(v.r), float(v.g), float(v.b), float(v.a), v.s, v.t, v.q, float(v.f),
                      float(int(v.u)), float(int(v.v))};
        if (hardware) {
          quad(verts, x, y, x + 1, y + 1, shared(d), scale, at);
          break;
        }
        GpuPrim p = Impl::stateOf(d);
        constant(p, v.z, rgba(v.r, v.g, v.b, v.a), v.s, v.t, v.q, float(int(v.u)), float(int(v.v)), float(v.f));
        quad(verts, x, y, x + 1, y + 1, uint32_t(prims.size()), scale, at);
        prims.push_back(p);
        break;
      }
      case 1: case 2: {  // line: the software rasterizer's steps, a pixel each
        const Gs::Vertex &a = c.v[0], &b = c.v[1];
        const float x0 = a.x / 16.0f, y0 = a.y / 16.0f, x1 = b.x / 16.0f, y1 = b.y / 16.0f;
        const int steps = std::max(1, int(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0))));
        for (int i = 0; i <= steps; ++i) {
          const float t = float(i) / steps;
          auto lerp = [&](float p, float q) { return p + (q - p) * t; };
          const uint32_t colour = d.iip ? rgba(uint8_t(int(lerp(a.r, b.r))), uint8_t(int(lerp(a.g, b.g))), uint8_t(int(lerp(a.b, b.b))),
                                               uint8_t(int(lerp(a.a, b.a))))
                                        : rgba(b.r, b.g, b.b, b.a);
          const float x = float(std::lround(lerp(x0, x1))), y = float(std::lround(lerp(y0, y1)));
          const Attr at{std::min(float(uint32_t(lerp(float(a.z), float(b.z)))), float(d.zmax)), float(colour & 0xFF), float((colour >> 8) & 0xFF),
                        float((colour >> 16) & 0xFF), float(colour >> 24), lerp(a.s, b.s), lerp(a.t, b.t), lerp(a.q, b.q),
                        float(uint8_t(lerp(a.f, b.f))), float(int(lerp(float(a.u), float(b.u)))), float(int(lerp(float(a.v), float(b.v))))};
          if (hardware) {
            quad(verts, x, y, x + 1, y + 1, shared(d), scale, at);
            continue;
          }
          GpuPrim p = Impl::stateOf(d);
          constant(p, uint32_t(lerp(float(a.z), float(b.z))), colour, lerp(a.s, b.s), lerp(a.t, b.t), lerp(a.q, b.q),
                   float(int(lerp(float(a.u), float(b.u)))), float(int(lerp(float(a.v), float(b.v)))), float(uint8_t(lerp(a.f, b.f))));
          quad(verts, x, y, x + 1, y + 1, uint32_t(prims.size()), scale, at);
          prims.push_back(p);
        }
        break;
      }
      case 3: case 4: case 5: {  // triangle: attribute planes as Gs::drawTriangle sets them up
        const Gs::Vertex &v0 = c.v[0], &v1 = c.v[1], &v2 = c.v[2];
        if (int64_t(v1.x - v0.x) * (v2.y - v0.y) - int64_t(v2.x - v0.x) * (v1.y - v0.y) == 0) break;
        if (hardware) {
          const uint32_t id = shared(d);
          for (const Gs::Vertex* v : {&v0, &v1, &v2})  // flat shading: the last vertex's colour everywhere
            verts.push_back({toTarget(v->x / 16.0f, scale), toTarget(v->y / 16.0f, scale), id,
                             attrOf(*v, float(v->z), d.iip ? *v : v2)});  // depth clamped per pixel (kFastPs)
          break;
        }
        GpuPrim p = Impl::stateOf(d);
        if (d.iip) p.flags |= F_IIP;
        p.flat = rgba(v2.r, v2.g, v2.b, v2.a);  // flat shading takes the last vertex's colour
        const float fx0 = v0.x / 16.0f, fy0 = v0.y / 16.0f;
        const float ax = v1.x / 16.0f - fx0, ay = v1.y / 16.0f - fy0, bx = v2.x / 16.0f - fx0, by = v2.y / 16.0f - fy0;
        const float det = ax * by - bx * ay;
        auto plane = [&](int k, float a0, float a1, float a2) {
          const float da = a1 - a0, db = a2 - a0;
          setPlane(p, k, a0, (da * by - db * ay) / det, (db * ax - da * bx) / det);
        };
        p.fx0 = fx0, p.fy0 = fy0;
        plane(kZ, float(v0.z), float(v1.z), float(v2.z));
        plane(kR, v0.r, v1.r, v2.r), plane(kG, v0.g, v1.g, v2.g), plane(kB, v0.b, v1.b, v2.b), plane(kA, v0.a, v1.a, v2.a);
        plane(kS, v0.s, v1.s, v2.s), plane(kT, v0.t, v1.t, v2.t), plane(kQ, v0.q, v1.q, v2.q);
        plane(kU, float(v0.u), float(v1.u), float(v2.u)), plane(kV, float(v0.v), float(v1.v), float(v2.v));
        plane(kF, v0.f, v1.f, v2.f);
        const uint32_t id = uint32_t(prims.size());
        for (const Gs::Vertex* v : {&v0, &v1, &v2}) verts.push_back({toTarget(v->x / 16.0f, scale), toTarget(v->y / 16.0f, scale), id, {}});
        prims.push_back(p);
        break;
      }
      case 6: {  // sprite: colour, depth and fog from the second vertex, texture across (Gs::drawSprite)
        const Gs::Vertex &a = c.v[0], &b = c.v[1];
        const Gs::Vertex& lo = a.x <= b.x ? a : b;
        const Gs::Vertex& hi = a.x <= b.x ? b : a;
        const Gs::Vertex& top = a.y <= b.y ? a : b;
        const Gs::Vertex& bot = a.y <= b.y ? b : a;
        const float dx = float(hi.x - lo.x), dy = float(bot.y - top.y);
        if (dx <= 0 || dy <= 0) break;
        // S and U run across, T and V down; the rest is the second vertex's.
        const Attr base{float(std::min(b.z, d.zmax)), float(b.r), float(b.g), float(b.b), float(b.a), 0, 0, b.q, float(b.f), 0, 0};
        auto corner = [&](const Gs::Vertex& h, const Gs::Vertex& w) {
          Attr at = base;
          at.s = h.s, at.u = float(h.u), at.t = w.t, at.v = float(w.v);
          return at;
        };
        if (hardware) {
          quad(verts, lo.x / 16.0f, top.y / 16.0f, hi.x / 16.0f, bot.y / 16.0f, shared(d), scale, corner(lo, top), corner(hi, top),
               corner(lo, bot), corner(hi, bot));
          break;
        }
        const float sStep = (hi.s - lo.s) * 16.0f / dx, uStep = (float(hi.u) - float(lo.u)) * 16.0f / dx;
        const float tStep = (bot.t - top.t) * 16.0f / dy, vStep = (float(bot.v) - float(top.v)) * 16.0f / dy;
        GpuPrim p = Impl::stateOf(d);
        constant(p, b.z, rgba(b.r, b.g, b.b, b.a), 0, 0, b.q, 0, 0, float(b.f));
        p.fx0 = lo.x / 16.0f, p.fy0 = top.y / 16.0f;
        setPlane(p, kS, lo.s, sStep), setPlane(p, kU, float(lo.u), uStep);
        setPlane(p, kT, top.t, 0, tStep), setPlane(p, kV, float(top.v), 0, vStep);
        quad(verts, lo.x / 16.0f, top.y / 16.0f, hi.x / 16.0f, bot.y / 16.0f, uint32_t(prims.size()), scale, Attr{}, Attr{}, Attr{}, Attr{});
        prims.push_back(p);
        break;
      }
    }
    m.commandState[k] = sharedId;
  }
  firstVertex.push_back(uint32_t(verts.size()));
  if (verts.empty()) return;
  size_t primsAt, vertsAt;
  if (!prims.empty()) {
    if (!m.fill(m.prims, prims.data(), prims.size() * sizeof(GpuPrim), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                sizeof(GpuPrim), primsAt))
      return;
    if (const uint32_t first = uint32_t(primsAt / sizeof(GpuPrim)))  // vertices name their primitive in the whole ring
      for (Vtx& v : verts) v.prim += first;
  }
  if (!m.fill(m.vertices, verts.data(), verts.size() * sizeof(Vtx), D3D11_BIND_VERTEX_BUFFER, 0, 0, vertsAt)) return;
  ID3D11DeviceContext* ctx = m.ctx.Get();
  const UINT offset = UINT(vertsAt);
  // Runs of primitives that go the same way: the hardware path needs one state per draw call.
  // A hardware run ends before a primitive whose texture reads pages drawn earlier in it (a run
  // draws in one frame and depth buffer: those of the box of what it drew so far).
  for (size_t i = 0; i < commands.size();) {
    const uint8_t way = ways[i];
    const bool fast = way != 0;
    int x0, y0, x1, y1;
    Impl::coverage(commands[i], x0, y0, x1, y1);
    size_t j = i + 1;
    for (; j < commands.size() && ways[j] == way && (!fast || Impl::sameRun(commands[i].d, commands[j].d)); ++j) {
      if (!fast) continue;
      if (commands[j].d.texFeedback && Impl::readsDrawn(commands[j], x0, y0, x1, y1)) break;
      int bx0, by0, bx1, by1;
      Impl::coverage(commands[j], bx0, by0, bx1, by1);
      if (way == 2 && bx0 <= x1 && x0 <= bx1 && by0 <= y1 && y0 <= by1) break;
      x0 = std::min(x0, bx0), y0 = std::min(y0, by0), x1 = std::max(x1, bx1), y1 = std::max(y1, by1);
    }
    const UINT start = firstVertex[i], count = firstVertex[j] - start;
    if (count) {
      m.vertexOffset = offset;
      if (fast) m.drawFast(commands, i, j, start, count, way == 2);
      else m.drawSlow(commands, i, j, start, count);
    }
    i = j;
  }
}

bool GsGpu::Impl::sameRun(const Gs::Draw& a, const Gs::Draw& b) {
  return a.fbp == b.fbp && a.fbw == b.fbw && a.fpsm == b.fpsm && a.fbmsk == b.fbmsk && a.ztest == b.ztest &&
         (!a.ztest || (a.zbp == b.zbp && a.zpsm == b.zpsm && a.ztst == b.ztst && a.zwrite == b.zwrite)) && a.blend == b.blend &&
         (!a.blend || (a.ba == b.ba && a.bb == b.bb && a.bc == b.bc && a.bd == b.bd)) && a.ate == b.ate &&
         (!a.ate || (a.atst == b.atst && a.afail == b.afail)) && counting(a) == counting(b) && exactBlend(a) == exactBlend(b);
}

// The GS pipeline on GS memory (kDrawPs): anything the hardware path cannot do.
void GsGpu::Impl::drawSlow(const std::vector<Gs::Command>& commands, size_t first, size_t end, UINT start, UINT count) {
  Pages reads, writes;
  for (size_t k = first; k < end; ++k) {
    const Gs::Draw& d = commands[k].d;
    int bx0, by0, bx1, by1;
    bounds(commands[k], bx0, by0, bx1, by1);
    writes |= pagesOf(d.fbp, d.fbw, d.fpsm, bx0, by0, bx1, by1);
    if (d.ztest) (writesDepth(d) ? writes : reads) |= pagesOf(d.zbp, d.fbw, d.zpsm, bx0, by0, bx1, by1);
    if (d.tme) reads |= texPages(commands[k]);
  }
  memRead(reads);
  memWrite(writes);
  rendered |= writes;
  bindDrawInput();
  ctx->PSSetShader(drawPs.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[3] = {prims.srv.Get(), swzSrv.Get(), clutSrv.Get()};
  ctx->PSSetShaderResources(0, 3, srvs);
  ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, dummyTarget.GetAddressOf(), nullptr, 1, 1, memUav.GetAddressOf(), nullptr);
  ctx->OMSetBlendState(noWrites.Get(), nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  {
    Busy busy(*this, kSlow);
    ctx->Draw(count, start);
  }
  ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ID3D11ShaderResourceView* none[3] = {};
  ctx->PSSetShaderResources(0, 3, none);
  ++slowDraws;
}

void GsGpu::Impl::drawStates(size_t first, size_t end) {
  ctx->PSSetConstantBuffers(2, 1, primStateBuffer.GetAddressOf());
  ID3D11ShaderResourceView* bound = nullptr;
  int boundSampler = -1;
  for (size_t k = first; k < end;) {
    const uint32_t id = commandState[k];
    size_t e = k + 1;
    while (e < end && commandState[e] == id) ++e;
    const UINT at = firstVertex[k], n = firstVertex[e] - at;
    if (n && (states[id].flags & F_CACHED)) {
      if (stateTex[id] != bound) ctx->PSSetShaderResources(6, 1, &stateTex[id]), bound = stateTex[id];
      const int s = (states[id].wms == 1 ? 1 : 0) | (states[id].wmt == 1 ? 2 : 0);
      if (s != boundSampler) ctx->PSSetSamplers(0, 1, texSamplers[s].GetAddressOf()), boundSampler = s;
    }
    if (n) {
      D3D11_MAPPED_SUBRESOURCE map;
      if (SUCCEEDED(ctx->Map(primStateBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, &states[id], offsetof(GpuPrim, pl));
        ctx->Unmap(primStateBuffer.Get(), 0);
        ctx->Draw(n, at);
      }
    }
    k = e;
  }
}

// Into surfaces, with the GPU's depth test and blending (kFastPs).
void GsGpu::Impl::drawFast(const std::vector<Gs::Command>& commands, size_t first, size_t end, UINT start, UINT count, bool destRead) {
  const Gs::Draw& d = commands[first].d;
  if (d.ztest && d.ztst == 0) return;  // the depth test never passes
  Pages reads;
  int x0 = 2047, y0 = 2047, x1 = 0, y1 = 0;
  for (size_t k = first; k < end; ++k) {
    const Gs::Draw& c = commands[k].d;
    if (c.tme) reads |= texPagesHeld(commands[k]);
    int bx0, by0, bx1, by1;
    bounds(commands[k], bx0, by0, bx1, by1);
    x0 = std::min(x0, bx0), y0 = std::min(y0, by0), x1 = std::max(x1, bx1), y1 = std::max(y1, by1);
  }
  if (x1 < x0 || y1 < y0) return;  // nothing inside the scissor
  const uint32_t width = d.fbw * 64, height = uint32_t(y1) + 1;
  const bool counts = counting(d);
  int colour = surfaceFor(d.fbp, d.fbw, d.fpsm, false, width, height, counts);
  int depth = d.ztest ? surfaceFor(d.zbp, d.fbw, d.zpsm, true, width, height) : -1;
  if (depth >= 0 && (surfaces[size_t(depth)].width != surfaces[size_t(colour)].width ||
                     surfaces[size_t(depth)].height != surfaces[size_t(colour)].height)) {  // the GPU wants them the same size
    const uint32_t w = std::max(surfaces[size_t(depth)].width, surfaces[size_t(colour)].width);
    const uint32_t h = std::max(surfaces[size_t(depth)].height, surfaces[size_t(colour)].height);
    colour = surfaceFor(d.fbp, d.fbw, d.fpsm, false, w, h, counts);
    depth = surfaceFor(d.zbp, d.fbw, d.zpsm, true, w, h);
  }
  const Pages drawnColour = pagesOf(d.fbp, d.fbw, d.fpsm, x0, y0, x1, y1);
  const Pages drawnDepth = depth >= 0 ? pagesOf(d.zbp, d.fbw, d.zpsm, x0, y0, x1, y1) : Pages();
  // A clear: flat sprites that write everything in the box, side by side.
  Pages cleared;
  if (!d.tme && !d.blend && d.fbmsk == 0 && (!d.ate || d.atst == 1 || (d.atst == 0 && d.afail == 1))) {
    int cx0 = 2047, cy0 = 2047, cx1 = -1, cy1 = -1;
    int64_t area = 0;
    bool sprites = true;
    for (size_t k = first; k < end && sprites; ++k) {
      int a0, b0, a1, b1;
      coverage(commands[k], a0, b0, a1, b1);
      sprites = commands[k].type == 6 && a1 >= a0 && b1 >= b0;
      area += int64_t(a1 - a0 + 1) * (b1 - b0 + 1);
      cx0 = std::min(cx0, a0), cy0 = std::min(cy0, b0), cx1 = std::max(cx1, a1), cy1 = std::max(cy1, b1);
    }
    if (sprites && area == int64_t(cx1 - cx0 + 1) * (cy1 - cy0 + 1)) cleared = wholePages(d.fbp, d.fbw, d.fpsm, cx0, cy0, cx1, cy1);
  }
  use(colour, drawnColour, cleared);
  if (depth >= 0) {
    use(depth, drawnDepth);
    lastDepth = depth;
    if (drew3D.size() < surfaces.size()) drew3D.resize(surfaces.size());
    const Surface& ds = surfaces[size_t(depth)];
    if (d.zwrite && d.ztst == 1 && x0 <= 0 && y0 <= 0 && uint32_t(x1 + 1) >= std::min(ds.width, 512u) && uint32_t(y1 + 1) >= std::min(ds.height, 448u)) {
      if (drew3D[size_t(depth)] >= 4) keepSceneDepth(depth);  // a clear of a scene's depth (not of one run, a shadow's): kept first
      drew3D[size_t(depth)] = 0;
    } else if (d.ztst >= 2) {
      ++drew3D[size_t(depth)];
    }
  }
  // The texture: from a colour surface holding all it reads, or from memory.
  ID3D11ShaderResourceView* texSrv = nullptr;
  uint32_t texSourceKind = 0;
  const int ts = textureSurface(commands, first, end, reads);
  if (ts < 0) {
    memRead(reads);
  } else if (ts != colour) {
    texSrv = surfaces[size_t(ts)].srv.Get();
    texSourceKind = surfaces[size_t(ts)].count ? 2 : 1;
  } else {  // the part read of the frame buffer drawn, as it was before
    Surface& sf = surfaces[size_t(colour)];
    int tx0 = INT32_MAX, ty0 = INT32_MAX, tx1 = -1, ty1 = -1;
    for (size_t k = first; k < end; ++k) {
      if (!commands[k].d.tme) continue;
      int a0, b0, a1, b1;
      texRect(commands[k], a0, b0, a1, b1);
      tx0 = std::min(tx0, a0), ty0 = std::min(ty0, b0), tx1 = std::max(tx1, a1), ty1 = std::max(ty1, b1);
    }
    texSrv = snapshot(sf, tx0, ty0, tx1, ty1);
    texSourceKind = sf.count ? 2 : 1;
  }
  ID3D11ShaderResourceView* destSrv = destRead ? snapshot(surfaces[size_t(colour)], x0, y0, x1, y1) : nullptr;
  // Filtered textures from memory: decoded (now that memory is up to date) for the GPU's filter.
  ++runNo;
  stateTex.resize(states.size()), stateRun.resize(states.size());
  for (size_t k = first; k < end; ++k) {
    const uint32_t id = commandState[k];
    if (stateRun[id] == runNo) continue;
    stateRun[id] = runNo;
    stateTex[id] = ts < 0 ? decoded(commands[k].d) : nullptr;
    states[id].flags = stateTex[id] ? states[id].flags | F_CACHED : states[id].flags & ~uint32_t(F_CACHED);
  }
  const Surface& cs = surfaces[size_t(colour)];
  bindDrawInput(true);
  const bool depth16 = d.zpsm == PSMZ16 || d.zpsm == PSMZ16S;
  const float zScale = depth16 ? 1.0f / 65535 : d.zpsm == PSMZ24 ? 1.0f / 16777216 : 1.0f / 4294967296.0f;
  ctx->OMSetRenderTargets(1, cs.rtv.GetAddressOf(), depth >= 0 ? surfaces[size_t(depth)].dsv.Get() : nullptr);
  ctx->PSSetShader((d.ztest && !depth16 ? fastDepthPs : fastPs).Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[6] = {nullptr, swzSrv.Get(), clutSrv.Get(), memSrv.Get(), texSrv, destSrv};
  ctx->PSSetShaderResources(0, 6, srvs);
  ctx->PSSetConstantBuffers(1, 1, texSource[texSourceKind].GetAddressOf());
  // Colour and alpha written as the frame buffer mask and format allow.
  bool rgb = (d.fbmsk & 0x00FFFFFFu) == 0, alpha = !d.fb24 && (d.fbmsk & 0xFF000000u) == 0;
  UINT8 rgbMask = rgb ? D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE : 0;
  UINT8 alphaMask = alpha ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0;
  if (counts) {  // channel by channel
    rgbMask = UINT8(((d.fbmsk & 0xF8u) ? 0 : D3D11_COLOR_WRITE_ENABLE_RED) | ((d.fbmsk & 0xF800u) ? 0 : D3D11_COLOR_WRITE_ENABLE_GREEN) |
                    ((d.fbmsk & 0xF80000u) ? 0 : D3D11_COLOR_WRITE_ENABLE_BLUE));
    alphaMask = (d.fbmsk & 0x80000000u) ? 0 : D3D11_COLOR_WRITE_ENABLE_ALPHA;
    rgb = rgbMask != 0, alpha = alphaMask != 0;
  }
  auto pass = [&](uint32_t mode, bool writeRgb, bool writeAlpha, bool writeZ) {
    const struct {
      uint32_t mode;
      float zScale;
      uint32_t count, exact, weighted, dest;
      float zBias;
    } c{mode, zScale, counts, exactBlend(d), weightedBlend(d) && !destRead, destRead, d.ztest && depth16 ? 0.45f : 0.0f};  // (well clear of what float rounding at high depths can reach)
    setConstants(&c, sizeof(c));
    ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->OMSetBlendState(blendState(d, UINT8((writeRgb ? rgbMask : 0) | (writeAlpha ? alphaMask : 0)), destRead), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depthState(d, writeZ), 0);
    Busy busy(*this, kFast);
    drawStates(first, end);
  };
  const bool zwrite = d.ztest && d.zwrite;
  if (lowResRun(commands, first, end, x0, y0, x1, y1, destRead) && drawLowRes(first, end, d, depth, zScale, alpha, x0, y0, x1, y1, cs)) {
    // drawn at the GS's resolution
  } else if (!d.ate || d.atst == 1 || d.afail == 0) {  // failing samples are dropped (or there are none)
    if (!d.ate || d.atst != 0 || d.afail != 0) pass(0, rgb, alpha, zwrite);
  } else {  // those passing as usual, then those failing with what the AFAIL mode keeps
    if (d.atst != 0) pass(1, rgb, alpha, zwrite);
    if (d.afail == 1) pass(2, rgb, alpha, false);              // frame buffer only
    else if (d.afail == 2) pass(2, false, false, zwrite);      // depth only
    else pass(2, rgb, false, false);                           // colour without alpha
  }
  if (subtracts(d) && rgb) {  // what went below 0 goes back to it, everywhere the run drew
    const struct {
      uint32_t mode;
      float zScale;
    } c{3, zScale};
    setConstants(&c, sizeof(c));
    if (!floorBlend) {
      D3D11_BLEND_DESC bd{};
      D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
      rt.BlendEnable = TRUE;
      rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE, rt.BlendOp = D3D11_BLEND_OP_MAX;
      rt.SrcBlendAlpha = D3D11_BLEND_ONE, rt.DestBlendAlpha = D3D11_BLEND_ONE, rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
      rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
      dev->CreateBlendState(&bd, &floorBlend);
    }
    ctx->OMSetBlendState(floorBlend.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(noDepth.Get(), 0);
    Busy busy(*this, kFast);
    drawStates(first, end);
  }
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ID3D11ShaderResourceView* none[8] = {};
  ctx->PSSetShaderResources(0, 8, none);
  // Those pages now have newer pixels in the surfaces than in memory.
  wrote(drawnColour);
  if (zwrite) wrote(drawnDepth);
  memStale |= drawnColour & regionOwnedBy(colour);
  rendered |= drawnColour;
  if (zwrite) memStale |= drawnDepth & regionOwnedBy(depth);
  ++fastDraws;
}

// Whether a run goes the low resolution way (see lowResPs): above 1x, many layers blended over
// what is under them (Cs * C + Cd * (1 - C), or Cd * (1 - C)), each the same at every sample of a
// pixel but for its depth test, which writes neither depth nor anything the gathering loses (the
// alpha written is the last layer's, never 0, to tell where one was drawn).
bool GsGpu::Impl::lowResRun(const std::vector<Gs::Command>& commands, size_t first, size_t end, int x0, int y0, int x1, int y1,
                            bool destRead) const {
  static const bool off = std::getenv("CODPORT_NOLOWRES") != nullptr;  // for comparisons
  const Gs::Draw& d = commands[first].d;
  if (off || !lowResPs || destRead || counting(d) || !weightedBlend(d) || d.bc == 1 || d.date || (d.fbmsk & 0x00FFFFFFu) ||
      (d.ztest && d.zwrite) || end - first < 8)
    return false;
  if (d.ate && (d.atst == 0 || (d.atst != 1 && d.afail != 0))) return false;  // all of it in one pass
  const int64_t box = int64_t(x1 - x0 + 1) * (y1 - y0 + 1);
  if (box < 128 * 128) return false;
  const bool alpha = !d.fb24 && (d.fbmsk & 0xFF000000u) == 0;
  int64_t area = 0;
  for (size_t k = first; k < end; ++k) {
    const Gs::Draw& c = commands[k].d;
    if (alpha && !c.fba && !(c.ate && ((c.atst == 6) || (c.atst == 5 && c.aref > 0) || (c.atst == 4 && c.aref > 0) || (c.atst == 7 && c.aref == 0))))
      return false;  // it could write an alpha of 0
    int a0, b0, a1, b1;
    coverage(commands[k], a0, b0, a1, b1);
    if (a1 >= a0 && b1 >= b0) area += int64_t(a1 - a0 + 1) * (b1 - b0 + 1);
  }
  return area >= 3 * box;  // layers over layers: where drawing each at every sample costs
}

bool GsGpu::Impl::drawLowRes(size_t first, size_t end, const Gs::Draw& d, int depth, float zScale, bool alpha, int x0, int y0, int x1,
                             int y1, const Surface& cs) {
  if (!lowTex || lowW < cs.width || lowH < cs.height) {
    lowW = std::max(lowW, cs.width), lowH = std::max(lowH, cs.height);
    D3D11_TEXTURE2D_DESC td{lowW, lowH, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    lowTex.Reset(), lowLastTex.Reset(), lowRtv.Reset(), lowLastRtv.Reset(), lowSrv.Reset(), lowLastSrv.Reset();
    dev->CreateTexture2D(&td, nullptr, &lowTex);
    td.Format = DXGI_FORMAT_R8_UNORM;
    dev->CreateTexture2D(&td, nullptr, &lowLastTex);
    if (!lowTex || !lowLastTex) return false;  // drawn as usual
    dev->CreateRenderTargetView(lowTex.Get(), nullptr, &lowRtv), dev->CreateShaderResourceView(lowTex.Get(), nullptr, &lowSrv);
    dev->CreateRenderTargetView(lowLastTex.Get(), nullptr, &lowLastRtv), dev->CreateShaderResourceView(lowLastTex.Get(), nullptr, &lowLastSrv);
  }
  const int w = alpha ? 1 : 0;
  if (!lowBlend[w]) {
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE, rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO, rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bd.RenderTarget[1].RenderTargetWriteMask = alpha ? D3D11_COLOR_WRITE_ENABLE_RED : 0;
    dev->CreateBlendState(&bd, &lowBlend[w]);
    D3D11_BLEND_DESC od{};
    D3D11_RENDER_TARGET_BLEND_DESC& o = od.RenderTarget[0];
    o.BlendEnable = TRUE;
    o.SrcBlend = D3D11_BLEND_ONE, o.DestBlend = D3D11_BLEND_SRC1_COLOR, o.BlendOp = D3D11_BLEND_OP_ADD;
    o.SrcBlendAlpha = D3D11_BLEND_SRC1_ALPHA, o.DestBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA, o.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    o.RenderTargetWriteMask = UINT8(D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE |
                                    (alpha ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    dev->CreateBlendState(&od, &lowOverBlend[w]);
  }
  // The layers, at the GS's resolution.
  const float clearLayers[4] = {0, 0, 0, 1}, clearLast[4] = {};
  ctx->ClearRenderTargetView(lowRtv.Get(), clearLayers);
  ctx->ClearRenderTargetView(lowLastRtv.Get(), clearLast);
  ID3D11RenderTargetView* rtvs[2] = {lowRtv.Get(), lowLastRtv.Get()};
  ctx->OMSetRenderTargets(2, rtvs, nullptr);
  const bool depth16 = d.zpsm == PSMZ16 || d.zpsm == PSMZ16S;
  const bool tested = d.ztest && d.ztst >= 2 && depth >= 0;
  ID3D11ShaderResourceView* depthSrv = tested ? surfaces[size_t(depth)].srv.Get() : nullptr;
  ctx->PSSetShaderResources(7, 1, &depthSrv);
  const struct {
    uint32_t mode;
    float zScale;
    uint32_t count, exact, weighted, dest;
    float zBias;
    uint32_t lowDepth;
  } c{0, zScale, 0, 0, 1, 0, d.ztest && depth16 ? 0.45f : 0.0f, tested ? (depth16 ? 2u : 1u) : 0u};
  setConstants(&c, sizeof(c));
  ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
  ctx->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
  const D3D11_VIEWPORT low{0, 0, 2048.0f, 2048.0f, 0, 1};
  ctx->RSSetViewports(1, &low);
  ctx->PSSetShader(lowResPs.Get(), nullptr, 0);
  ctx->OMSetBlendState(lowBlend[w].Get(), nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(noDepth.Get(), 0);
  {
    Busy busy(*this, kFast);
    drawStates(first, end);
  }
  // Over the surface, in the run's box.
  ID3D11ShaderResourceView* noDepthSrv = nullptr;
  ctx->PSSetShaderResources(7, 1, &noDepthSrv);
  ctx->OMSetRenderTargets(1, cs.rtv.GetAddressOf(), nullptr);
  const float size[4] = {1.0f / float(lowW), 1.0f / float(lowH)};
  setConstants(size, sizeof(size));
  ID3D11ShaderResourceView* layers[2] = {lowSrv.Get(), lowLastSrv.Get()};
  ctx->PSSetShaderResources(0, 2, layers);
  ctx->PSSetSamplers(1, 1, linearSampler.GetAddressOf());
  const D3D11_VIEWPORT box{float(x0 * int(scale)), float(y0 * int(scale)), float((x1 - x0 + 1) * int(scale)), float((y1 - y0 + 1) * int(scale)), 0, 1};
  ctx->RSSetViewports(1, &box);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(presentVs.Get(), nullptr, 0);
  ctx->PSSetShader(lowResOverPs.Get(), nullptr, 0);
  ctx->OMSetBlendState(lowOverBlend[w].Get(), nullptr, 0xFFFFFFFF);
  {
    Busy busy(*this, kFast);
    ctx->Draw(3, 0);
  }
  bindDrawInput(true);
  ++lowResRuns;
  return true;
}

void GsGpu::Impl::keepSceneDepth(int depth) {
  const Surface& ds = surfaces[size_t(depth)];
  D3D11_TEXTURE2D_DESC td{};
  ds.tex->GetDesc(&td);
  D3D11_TEXTURE2D_DESC have{};
  if (sceneDepth) sceneDepth->GetDesc(&have);
  if (!sceneDepth || have.Width != td.Width || have.Height != td.Height || have.Format != td.Format) {
    sceneDepth.Reset(), sceneDepthSrv.Reset();
    D3D11_TEXTURE2D_DESC kd = td;
    kd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&kd, nullptr, &sceneDepth))) return;
    sceneDepth16 = td.Format == DXGI_FORMAT_R16_TYPELESS;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = sceneDepth16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT, sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(sceneDepth.Get(), &sd, &sceneDepthSrv);
  }
  ctx->CopyResource(sceneDepth.Get(), ds.tex.Get());
  sceneDepthNew = true, sceneDepthOf = depth;
}

ID3D11ShaderResourceView* GsGpu::Impl::decoded(const Gs::Draw& d) {
  if (!d.linear) return nullptr;
  const bool paletted = d.tpsm == PSMT8 || d.tpsm == PSMT4 || d.tpsm == PSMT8H || d.tpsm == PSMT4HL || d.tpsm == PSMT4HH;
  const TexKey key{d.tbp, d.tbw, d.tpsm, uint32_t(d.tw), uint32_t(d.th), paletted ? d.clutSlot : 0, paletted ? slotUses[d.clutSlot] : 0,
                   paletted ? d.csa : 0, paletted ? 0 : d.ta0 | d.ta1 << 8 | uint32_t(d.aem) << 16};
  const uint64_t memory = writesOf(d.tbp, d.tbw, d.tpsm, 0, 0, uint32_t(d.tw) - 1, uint32_t(d.th) - 1);
  TexEntry* e;
  if (auto it = texCache.find(key); it != texCache.end()) {
    e = &it->second;
  } else {
    const size_t bytes = size_t(d.tw) * size_t(d.th) * 4;
    if (texCacheBytes + bytes > kTexCacheBytes) {  // the least recently used go, down to three quarters
      std::vector<std::pair<uint64_t, TexKey>> order;
      for (const auto& [k, v] : texCache) order.push_back({v.lastUse, k});
      std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      for (const auto& [use, k] : order) {
        if (texCacheBytes + bytes <= kTexCacheBytes * 3 / 4) break;
        texCacheBytes -= size_t(k.tw) * k.th * 4;
        texCache.erase(k);
      }
    }
    TexEntry fresh;
    D3D11_TEXTURE2D_DESC td{uint32_t(d.tw), uint32_t(d.th), 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &fresh.tex)) || FAILED(dev->CreateUnorderedAccessView(fresh.tex.Get(), nullptr, &fresh.uav)) ||
        FAILED(dev->CreateShaderResourceView(fresh.tex.Get(), nullptr, &fresh.srv)))
      return nullptr;
    texCacheBytes += bytes;
    e = &texCache.emplace(key, std::move(fresh)).first->second;
  }
  e->lastUse = ++texUse;
  if (e->memory != memory) {
    e->memory = memory;
    // Every texel as it is in memory: the wrap modes apply when it is read (in a region mode,
    // the shader's cachedRegionTexel; otherwise the sampler).
    GpuPrim p = stateOf(d);
    p.wms = p.wmt = 0;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(primStateBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return nullptr;
    std::memcpy(map.pData, &p, offsetof(GpuPrim, pl));
    ctx->Unmap(primStateBuffer.Get(), 0);
    ID3D11ShaderResourceView* srvs[4] = {nullptr, swzSrv.Get(), clutSrv.Get(), memSrv.Get()};
    Busy busy(*this, kLoad);
    ctx->CSSetShader(decodeCs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 4, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, e->uav.GetAddressOf(), nullptr);
    ctx->CSSetConstantBuffers(1, 1, texSource[0].GetAddressOf());
    ctx->CSSetConstantBuffers(2, 1, primStateBuffer.GetAddressOf());
    ctx->Dispatch((uint32_t(d.tw) + 7) / 8, (uint32_t(d.th) + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrvs[4] = {};
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetShaderResources(0, 4, noSrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    e->content = contentOf(d);
    if (e->content && pack.shouldDump(e->content)) dumpTexture(e->tex.Get(), uint32_t(d.tw), uint32_t(d.th), e->content);
  }
  // A texture pack's picture in its place, when the GPU's sampler can read it (not the region
  // modes, which address the game's texels).
  if (e->content && d.wms <= 1 && d.wmt <= 1)
    if (ID3D11ShaderResourceView* r = replacement(e->content)) return r;
  return e->srv.Get();
}

uint64_t GsGpu::Impl::contentOf(const Gs::Draw& d) const {
  // Every block of the texture still as one upload left it (the game uploads 4- and 8-bit
  // textures in other formats, and several in one go): that upload, and where in it.
  uint64_t source = 0;
  uint32_t dbp = 0;
  bool whole = true;
  forBlocks(d.tbp, d.tbw, d.tpsm, 0, 0, uint32_t(d.tw) - 1, uint32_t(d.th) - 1, [&](uint32_t b) {
    const BlockSource& s = blockSource[b];
    if (!s.hash || s.writes != blockWrites[b] || (source && (s.hash != source || s.dbp != dbp))) whole = false;
    source = s.hash, dbp = s.dbp;
  });
  if (!whole || !source) return 0;
  const bool paletted = d.tpsm == PSMT8 || d.tpsm == PSMT4 || d.tpsm == PSMT8H || d.tpsm == PSMT4HL || d.tpsm == PSMT4HH;
  const uint64_t palette = paletted ? slotContent[d.clutSlot] : 0;
  if (paletted && !palette) return 0;
  uint64_t h = source;
  for (uint64_t v : {uint64_t(d.tbp - dbp), uint64_t(d.tbw), uint64_t(d.tpsm), uint64_t(d.tw), uint64_t(d.th), palette, uint64_t(paletted ? d.csa : d.ta0 | d.ta1 << 8 | uint32_t(d.aem) << 16)})
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
  return h | 1;
}

void GsGpu::Impl::dumpTexture(ID3D11Texture2D* tex, uint32_t w, uint32_t h, uint64_t hash) {
  D3D11_TEXTURE2D_DESC td{};
  tex->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  D3D11_MAPPED_SUBRESOURCE map;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return;
  ctx->CopyResource(staging.Get(), tex);
  if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* s = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch + x * 4;
      uint8_t* o = rgba.data() + (size_t(y) * w + x) * 4;
      o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = uint8_t(std::min(255, s[3] * 2));  // the PS2's 128 is opaque
    }
  ctx->Unmap(staging.Get(), 0);
  savePng(pack.dumpFile(hash), w, h, rgba.data(), w * 4);
}

ID3D11ShaderResourceView* GsGpu::Impl::replacement(uint64_t hash) {
  const std::filesystem::path* file = pack.replacement(hash);
  if (!file) return nullptr;
  auto [it, fresh] = replaced.try_emplace(hash);
  if (fresh) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (loadPng(*file, w, h, rgba)) {
      for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = uint8_t((rgba[i] + 1) / 2);  // back to the PS2's alpha
      D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
      const D3D11_SUBRESOURCE_DATA data{rgba.data(), w * 4, 0};
      ComPtr<ID3D11Texture2D> tex;
      if (SUCCEEDED(dev->CreateTexture2D(&td, &data, &tex))) dev->CreateShaderResourceView(tex.Get(), nullptr, &it->second);
    }
    if (!it->second) std::printf("texture pack: could not read %s\n", file->string().c_str());
  }
  return it->second.Get();
}

void GsGpu::upload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const std::vector<uint64_t>& data) {
  Impl& m = *impl_;
  const uint32_t dpsm = bits(bitbltbuf, 56, 6), rrw = bits(trxreg, 0, 12), rrh = bits(trxreg, 32, 12);
  const uint32_t bpp = GsMemory::bitsPerPixel(dpsm);
  if (!rrw || !rrh || data.empty()) return;
  const uint32_t count = uint32_t(std::min<uint64_t>(uint64_t(rrw) * rrh, uint64_t(data.size()) * 64 / bpp));
  const int dx = int(bits(trxpos, 32, 11)), dy = int(bits(trxpos, 48, 11));
  const Impl::Pages uploaded = Impl::pagesOf(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), dpsm, dx, dy, dx + int(rrw) - 1, dy + int(rrh) - 1);
  m.memStale &= ~Impl::wholePages(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), dpsm, dx, dy, dx + int(rrw) - 1, dy + int(rrh) - 1);
  m.memWrite(uploaded, false);
  {
    const uint32_t dbp = bits(bitbltbuf, 32, 14), dbw = bits(bitbltbuf, 48, 6);
    const uint32_t x1 = uint32_t(dx) + rrw - 1, y1 = uint32_t(dy) + rrh - 1;
    uint64_t lane[4] = {1, 2, 3, 4};
    size_t i = 0;
    for (; i + 4 <= data.size(); i += 4)
      for (int k = 0; k < 4; ++k) lane[k] = (lane[k] ^ data[i + k]) * 0x9E3779B97F4A7C15ull;
    for (; i < data.size(); ++i) lane[0] = (lane[0] ^ data[i]) * 0x9E3779B97F4A7C15ull;
    const uint64_t hash = ((lane[0] * 31 + lane[1]) * 31 + lane[2]) * 31 + lane[3] + data.size();
    const uint64_t source = (hash ^ (uint64_t(dpsm) << 32 | uint64_t(dx) << 16 | uint32_t(dy))) | 1;
    Impl::forBlocks(dbp, dbw, dpsm, uint32_t(dx), uint32_t(dy), x1, y1, [&](uint32_t b) {
      ++m.blockWrites[b];
      m.blockSource[b] = {source, dbp, m.blockWrites[b]};
    });
    m.uploads[dbp] = {dbw, dpsm, uint32_t(dx), uint32_t(dy), x1, y1, hash, m.writesOf(dbp, dbw, dpsm, uint32_t(dx), uint32_t(dy), x1, y1)};
  }
  m.rendered &= ~uploaded;
  std::vector<uint64_t>& padded = m.uploadScratch;
  padded.assign(data.begin(), data.end());
  padded.push_back(0);  // a pixel straddling the last word reads one beyond
  size_t at;
  if (!m.fill(m.uploadData, padded.data(), padded.size() * 8, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, at))
    return;
  const uint32_t c[9] = {bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), dpsm, bits(trxpos, 32, 11), bits(trxpos, 48, 11), rrw, bpp, count,
                         uint32_t(at / 4)};
  m.setConstants(c, sizeof(c));
  Impl::Busy busy(m, Impl::kUpload);
  m.dispatch(m.uploadCs.Get(), m.uploadData.srv.Get(), nullptr, count);
}

void GsGpu::copy(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg) {
  Impl& m = *impl_;
  const uint32_t rrw = bits(trxreg, 0, 12), rrh = bits(trxreg, 32, 12);
  if (!rrw || !rrh) return;
  const int sx = int(bits(trxpos, 0, 11)), sy = int(bits(trxpos, 16, 11)), dx = int(bits(trxpos, 32, 11)), dy = int(bits(trxpos, 48, 11));
  m.memRead(Impl::pagesOf(bits(bitbltbuf, 0, 14), bits(bitbltbuf, 16, 6), bits(bitbltbuf, 24, 6), sx, sy, sx + int(rrw) - 1, sy + int(rrh) - 1));
  m.memWrite(Impl::pagesOf(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6), dx, dy, dx + int(rrw) - 1, dy + int(rrh) - 1));
  uint32_t c[14] = {bits(bitbltbuf, 0, 14),  bits(bitbltbuf, 16, 6), bits(bitbltbuf, 24, 6), bits(trxpos, 0, 11), bits(trxpos, 16, 11),
                    bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6), bits(trxpos, 32, 11), bits(trxpos, 48, 11),
                    rrw,                     std::min(rrw * rrh, 2048u * 2048u), 0, 0};
  Impl::Busy busy(m, Impl::kCopy);
  for (uint32_t layer = 0; layer < m.scale * m.scale; ++layer) {  // each layer copies within itself
    c[12] = 0, c[13] = layer;  // read the source pixels
    m.setConstants(c, sizeof(c));
    m.dispatch(m.copyCs.Get(), nullptr, m.copyTmpUav.Get(), c[11]);
    c[12] = 1;  // then write them
    m.setConstants(c, sizeof(c));
    m.dispatch(m.copyCs.Get(), nullptr, m.copyTmpUav.Get(), c[11]);
  }
}

uint32_t GsGpu::loadClut(uint32_t previous, uint32_t cbp, uint32_t clutPsm, uint32_t csm, uint32_t csa, bool t8, uint64_t texclut,
                         uint64_t texa) {
  Impl& m = *impl_;
  const Impl::Pages source = csm == 0 ? Impl::pagesOf(cbp, 1, clutPsm, 0, 0, 15, 15)
                                      : Impl::pagesOf(cbp, bits(texclut, 0, 6), clutPsm, int(bits(texclut, 6, 6)) * 16, int(bits(texclut, 12, 10)),
                                                      int(bits(texclut, 6, 6)) * 16 + 255, int(bits(texclut, 12, 10)));
  // A 256-entry palette replaces all the previous one's entries.
  Impl::ClutKey key{t8 ? 0 : previous, t8 ? 0 : m.slotUses[previous], cbp, clutPsm, csm, csa, t8, uint32_t(csm ? texclut & 0x3FFFFF : 0),
                    uint32_t(texa & 0xFF) | uint32_t(texa >> 32 & 0xFF) << 8 | uint32_t(texa >> 15 & 1) << 16, 0, false};
  const uint32_t sbw = csm ? bits(texclut, 0, 6) : 1, sx0 = csm ? bits(texclut, 6, 6) * 16 : 0, sy0 = csm ? bits(texclut, 12, 10) : 0;
  const uint32_t sx1 = csm ? sx0 + (t8 ? 255 : 15) : t8 ? 15 : 7, sy1 = csm ? sy0 : t8 ? 15 : 1;
  if (auto up = m.uploads.find(cbp); up != m.uploads.end()) {
    const Impl::Upload& u = up->second;
    if (u.bw == sbw && u.psm == clutPsm && u.x0 <= sx0 && u.y0 <= sy0 && u.x1 >= sx1 && u.y1 >= sy1 &&
        m.writesOf(cbp, u.bw, u.psm, u.x0, u.y0, u.x1, u.y1) == u.writes)
      key.memory = u.hash, key.known = true;
  }
  if (!key.known) key.memory = m.writesOf(cbp, sbw, clutPsm, sx0, sy0, sx1, sy1);
  if (auto it = m.clutCache.find(key); it != m.clutCache.end()) return it->second;
  m.memRead(source);
  // Slots are reused in turn; draws using an old one were issued before it is overwritten.
  const uint32_t slot = m.nextClut;
  m.nextClut = m.nextClut + 1 == kClutSlots ? 1 : m.nextClut + 1;
  if (auto it = m.clutCache.find(m.clutOfSlot[slot]); it != m.clutCache.end() && it->second == slot) m.clutCache.erase(it);
  ++m.slotUses[slot];
  m.clutOfSlot[slot] = key;
  m.slotContent[slot] = 0;
  if (key.known) {
    uint64_t h = key.memory;
    for (uint64_t v : {uint64_t(clutPsm), uint64_t(csm), uint64_t(csa), uint64_t(t8), uint64_t(key.texclut), uint64_t(key.texa)}) h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    m.slotContent[slot] = h | 1;
  }
  m.clutCache[key] = slot;
  const uint32_t c[13] = {previous, slot, cbp, clutPsm, csm, csa, t8, bits(texclut, 0, 6), bits(texclut, 6, 6), bits(texclut, 12, 10),
                          bits(texa, 0, 8), bits(texa, 32, 8), bits(texa, 15, 1)};
  m.setConstants(c, sizeof(c));
  ID3D11ShaderResourceView* srvs[2] = {nullptr, m.swzSrv.Get()};
  ID3D11UnorderedAccessView* uavs[2] = {m.memUav.Get(), m.clutUav.Get()};
  ID3D11DeviceContext* ctx = m.ctx.Get();
  Impl::Busy busy(m, Impl::kClut);
  ctx->CSSetShader(m.clutCs.Get(), nullptr, 0);
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
  ctx->CSSetConstantBuffers(0, 1, m.constants.GetAddressOf());
  ctx->Dispatch(1, 1, 1);
  ID3D11UnorderedAccessView* noUavs[2] = {};
  ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
  return slot;
}

bool GsGpu::Impl::makePicture(const Gs::Display& d, uint32_t pictureScale) {
  const Pages shown = pagesOf(d.fbp, d.fbw, d.psm, int(d.dbx), int(d.dby), int(d.dbx + d.width) - 1,
                              int(d.dby + (d.field ? d.height / 2 : d.height)) - 1);
  // A colour surface holding all of it: no need for GS memory.
  const Surface* holder = nullptr;
  for (size_t i = 0; i < surfaces.size() && !holder; ++i) {
    const Surface& sf = surfaces[i];
    if (sf.depth || sf.count || sf.bp != d.fbp || sf.bw != d.fbw || sf.psm != d.psm) continue;
    bool all = true;
    for (size_t p = 0; p < 512 && all; ++p) all = !shown[p] || (owner[p] == int(i) && !ownerStale[p]);
    if (all) holder = &sf;
  }
  if (!holder) memRead(shown);
  const uint32_t w = d.width * pictureScale, h = d.height * pictureScale;
  if (pictureW != w || pictureH != h) {
    picture.Reset(), staging.Reset(), pictureUav.Reset(), pictureSrv.Reset();
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &picture))) return pictureW = pictureH = 0, false;
    dev->CreateUnorderedAccessView(picture.Get(), nullptr, &pictureUav);
    dev->CreateShaderResourceView(picture.Get(), nullptr, &pictureSrv);
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &staging);
    pictureW = w, pictureH = h;
  }
  Busy busy(*this, kPicture);
  if (holder) {
    const uint32_t step = scale / pictureScale;
    const uint32_t c[7] = {d.dbx * scale, d.dby * scale, w, h, d.field, step, d.psm == PSMCT16 || d.psm == PSMCT16S};
    setConstants(c, sizeof(c));
    ctx->CSSetShader(surfacePictureCs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, holder->srv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 1, pictureUav.GetAddressOf(), nullptr);
    ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &noSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    return true;
  }
  const uint32_t c[9] = {d.fbp, d.fbw, d.psm, d.dbx, d.dby, d.width, d.height, d.field, pictureScale};
  setConstants(c, sizeof(c));
  ID3D11ShaderResourceView* srvs[2] = {memSrv.Get(), swzSrv.Get()};
  ctx->CSSetShader(pictureCs.Get(), nullptr, 0);
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, pictureUav.GetAddressOf(), nullptr);
  ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
  ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
  ID3D11ShaderResourceView* noSrvs[2] = {};
  ID3D11UnorderedAccessView* noUav = nullptr;
  ctx->CSSetShaderResources(0, 2, noSrvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  return true;
}

void GsGpu::Impl::mark(int work) {
  if (!timing()) return;
  FrameStamps& s = stamps[frameNo % stamps.size()];
  if (!s.open) {
    if (!s.disjoint) {
      D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      dev->CreateQuery(&qd, &s.disjoint);
    }
    ctx->Begin(s.disjoint.Get());
    s.open = true;
  }
  const size_t n = s.works.size();
  if (n == s.queries.size()) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
    s.queries.emplace_back();
    dev->CreateQuery(&qd, &s.queries.back());
  }
  ctx->End(s.queries[n].Get());
  s.works.push_back(work);
}

void GsGpu::Impl::frameTimer() {
  if (!timing()) return;
  mark(kIdle);
  FrameStamps& done = stamps[frameNo % stamps.size()];
  ctx->End(done.disjoint.Get());
  done.pending = true, done.open = false;
  ++frameNo;
  FrameStamps& s = stamps[frameNo % stamps.size()];  // the oldest: finished by now, or soon
  if (s.pending) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    while (ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), 0) != S_OK) {}
    UINT64 prev = 0;
    double busy = 0;
    for (size_t i = 0; i < s.works.size(); ++i) {
      UINT64 at;
      while (ctx->GetData(s.queries[i].Get(), &at, sizeof(at), 0) != S_OK) {}
      if (i && !dj.Disjoint) {
        const double ms = double(at - prev) * 1000.0 / double(dj.Frequency);
        workMs[s.works[i - 1]] += ms;
        if (s.works[i - 1] != kIdle) busy += ms;
      }
      prev = at;
    }
    busySeconds = busySeconds + busy / 1000.0;
    ++gpuFrames;
    s.works.clear();
    s.pending = false;
  }
  static const bool report = std::getenv("CODPORT_GPU_TIME") != nullptr;
  if (frameNo % 100 == 0 && gpuFrames) {
    if (report) {
      double total = 0;
      for (int w = 1; w < kWorks; ++w) total += workMs[w];
      std::printf("   GPU busy %.2f ms/frame (frames %llu-%llu):", total / double(gpuFrames), (unsigned long long)(frameNo - 100), (unsigned long long)frameNo);
      static const char* const names[kWorks] = {"waiting", "GS-memory draws", "hardware draws", "to memory", "from memory", "uploads", "copies", "palettes", "picture"};
      for (int w = 0; w < kWorks; ++w) std::printf(" %s %.2f,", names[w], workMs[w] / double(gpuFrames));
      std::printf("\n");
    }
    for (double& ms : workMs) ms = 0;
    gpuFrames = 0;
  }
}

void GsGpu::frameEnd() { impl_->frameTimer(); }
double GsGpu::busySeconds() const { return impl_->busySeconds; }

void GsGpu::Impl::drawMap(const MinimapView& view, float left, float top, uint32_t size) {
  ID3D11DeviceContext* c = ctx.Get();
  if (view.version != mapVersion) {
    mapVersion = view.version;
    mapMesh.Reset(), mapMeshSrv.Reset();
    if (!mapVertices.empty()) {
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = UINT(mapVertices.size() * sizeof(MinimapVertex));
      bd.Usage = D3D11_USAGE_IMMUTABLE, bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, bd.StructureByteStride = sizeof(MinimapVertex);
      const D3D11_SUBRESOURCE_DATA init{mapVertices.data(), 0, 0};
      if (SUCCEEDED(dev->CreateBuffer(&bd, &init, &mapMesh))) dev->CreateShaderResourceView(mapMesh.Get(), nullptr, &mapMeshSrv);
    }
  }
  if (size != mapSize) {
    mapSize = size;
    mapRtv.Reset(), mapSrv.Reset(), mapMsaa.Reset(), mapResolved.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = size, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {4, 0}, td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &mapMsaa))) return;
    dev->CreateRenderTargetView(mapMsaa.Get(), nullptr, &mapRtv);
    td.SampleDesc = {1, 0}, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &mapResolved))) return;
    dev->CreateShaderResourceView(mapResolved.Get(), nullptr, &mapSrv);
  }
  if (!mapRtv || !mapSrv) return;
  ComPtr<ID3D11RenderTargetView> target;
  c->OMGetRenderTargets(1, &target, nullptr);
  D3D11_VIEWPORT outer;
  UINT viewports = 1;
  c->RSGetViewports(&viewports, &outer);
  static constexpr float kBackground[4] = {0.03f, 0.03f, 0.07f, 0.65f};
  c->ClearRenderTargetView(mapRtv.Get(), kBackground);
  c->OMSetRenderTargets(1, mapRtv.GetAddressOf(), nullptr);
  const D3D11_VIEWPORT area{0, 0, float(size), float(size), 0, 1};
  c->RSSetViewports(1, &area);
  static constexpr float kViewWidth = 2600.0f;  // map units across
  struct {
    float x, z, zoom;
    uint32_t meshVertices;
    float dirX, dirZ, arrow, unused;
  } shape{view.x, view.z, 2.0f / kViewWidth, mapMeshSrv ? uint32_t(mapVertices.size()) : 0, view.dirX, view.dirZ, 0.07f, 1.0f};
  setConstants(&shape, sizeof(shape));
  c->VSSetShader(mapVs.Get(), nullptr, 0);
  c->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
  c->VSSetShaderResources(0, 1, mapMeshSrv.GetAddressOf());
  c->PSSetShader(mapPs.Get(), nullptr, 0);
  c->Draw(shape.meshVertices + 3, 0);
  ID3D11ShaderResourceView* none = nullptr;
  c->VSSetShaderResources(0, 1, &none);
  c->ResolveSubresource(mapResolved.Get(), 0, mapMsaa.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
  // Over the picture.
  c->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
  c->RSSetViewports(1, &outer);
  struct {
    float left, top, size, alpha;
  } over{left, top, float(size), view.alpha};
  setConstants(&over, sizeof(over));
  c->VSSetShader(presentVs.Get(), nullptr, 0);
  c->PSSetShader(mapOverPs.Get(), nullptr, 0);
  c->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
  c->PSSetShaderResources(0, 1, mapSrv.GetAddressOf());
  c->PSSetSamplers(0, 1, linearSampler.GetAddressOf());
  c->OMSetBlendState(overBlend.Get(), nullptr, 0xFFFFFFFF);
  c->Draw(3, 0);
  c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  c->PSSetShaderResources(0, 1, &none);
}

bool GsGpu::Impl::target(RECT& rc) {
  if (!swapChain || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
  if (UINT(rc.right) != backW || UINT(rc.bottom) != backH || !backBuffer) {
    backBuffer.Reset();
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (FAILED(swapChain->ResizeBuffers(0, UINT(rc.right), UINT(rc.bottom), DXGI_FORMAT_UNKNOWN,
                                        refreshWait ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0)))
      return false;
    ComPtr<ID3D11Texture2D> back;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    dev->CreateRenderTargetView(back.Get(), nullptr, &backBuffer);
    backW = UINT(rc.right), backH = UINT(rc.bottom);
  }
  return true;
}

void GsGpu::present(const Gs::Display& d, uint64_t stamp) {
  Impl& m = *impl_;
  if (!m.swapChain || !m.makePicture(d, m.scale)) return;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const double at = stamp ? double(stamp) * 1e-9 : now, gap = at - m.stampB;
  if (gap > 0.005 && gap < 0.1) m.frameSeconds += (gap - m.frameSeconds) * 0.05;
  if (m.presents++ % 60 == 0) {  // the screen's refresh rate, now and then (the window may have moved)
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(MonitorFromWindow(m.hwnd, MONITOR_DEFAULTTONEAREST), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1)
      m.refreshHz = dm.dmDisplayFrequency;
  }
  const double gameHz = m.frameSeconds > 1 / 55.0 ? 50.0 : 59.94;  // PAL or NTSC
  // Interpolation: on a screen at least half again as fast as the game, unless the game is
  // outrunning real time (fast loading), when the pictures are shown as they come.
  const bool interpolate = settings().interpolation && m.playing && m.refreshThread.joinable() && m.refreshHz >= gameHz * 1.5 && gap > 0.011 && m.generator();
  m.fps = fpsText(interpolate ? m.refreshHz / gameHz : 1);
  if (interpolate) {
    m.view.wide = wideScreen();
    const Impl::PictureDepth z = m.pictureDepth();
    m.frameGen->push(m.picture.Get(), m.pictureW, m.pictureH, m.scale, m.view, z.depth, m.scale, z.scene);
    m.stampA = m.stampB, m.stampB = at;
    m.delays[m.presents % m.delays.size()] = now - at;  // the most of late over the last 2 seconds, at most most of a frame
    m.lag = std::min(*std::max_element(m.delays.begin(), m.delays.end()), m.frameSeconds * 0.8);
    m.interpolating = true;
    return;
  }
  m.interpolating = false, m.ticked = false;
  m.stampB = at;
  if (m.frameGen) m.frameGen->forget();
  RECT rc;
  if (!m.target(rc)) return;
  m.show(m.pictureSrv.Get(), rc);
  m.swapChain->Present(0, 0);
}

void GsGpu::tick() {
  Impl& m = *impl_;
  std::unique_lock lock(m.callMutex, std::try_to_lock);
  if (!lock || !m.interpolating || !m.frameGen || !m.frameGen->ready()) return;
  RECT rc;
  if (!m.target(rc)) return;
  // The moment shown: a frame and the most a picture has been late behind now, so that the
  // pictures on both sides of it are always here.
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const double moment = now - m.frameSeconds - m.lag - 0.001;
  const double t = m.stampB > m.stampA ? std::clamp((moment - m.stampA) / (m.stampB - m.stampA), 0.0, 1.0) : 1.0;
  const int aw = wideScreen() ? 16 : 4, ah = wideScreen() ? 9 : 3;  // made as large as show() shows it
  int w = rc.right, h = rc.bottom;
  if (w * ah > h * aw) w = h * aw / ah; else h = w * ah / aw;
  ID3D11ShaderResourceView* made = m.frameGen->make(float(t), uint32_t(w), uint32_t(h));
  if (!made) return;
  m.show(made, rc);
  m.swapChain->Present(1, 0);
  m.ticked = true;
}

void GsGpu::setView(float yaw, float pitch, bool playing) {
  impl_->playing = playing;
  impl_->view.known = std::isfinite(yaw) && std::isfinite(pitch);
  impl_->view.yaw = yaw, impl_->view.pitch = pitch;
}

void GsGpu::onRefresh(std::function<void()> wake) {
  Impl& m = *impl_;
  if (m.refreshThread.joinable()) {
    m.refreshStop = true;
    m.refreshThread.join();
    m.refreshStop = false;
  }
  if (!wake || !m.refreshWait) return;
  m.refreshThread = std::thread([&m, wake = std::move(wake)] {
    while (!m.refreshStop)
      if (const DWORD r = WaitForSingleObjectEx(m.refreshWait, 20, FALSE); m.interpolating && (r == WAIT_OBJECT_0 || (r == WAIT_TIMEOUT && !m.ticked)))
        wake();  // (also when it times out before a first picture: the swap chain waits for one)
  });
}

void GsGpu::Impl::show(ID3D11ShaderResourceView* srv, const RECT& rc) {
  // The PS2 picture is 4:3 (or 16:9, F10) whatever its size in pixels; it is centred with black bars.
  const int aw = wideScreen() ? 16 : 4, ah = wideScreen() ? 9 : 3;
  int w = rc.right, h = rc.bottom;
  if (w * ah > h * aw) w = h * aw / ah; else h = w * ah / aw;
  const float black[4] = {0, 0, 0, 1};
  ctx->ClearRenderTargetView(backBuffer.Get(), black);
  const D3D11_VIEWPORT vp{float((rc.right - w) / 2), float((rc.bottom - h) / 2), float(w), float(h), 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(rasterizer.Get());
  ctx->OMSetRenderTargets(1, backBuffer.GetAddressOf(), nullptr);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(presentVs.Get(), nullptr, 0);
  ctx->PSSetShader(presentPs.Get(), nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &srv);
  // Pixel for pixel at 1x as before; a larger picture is filtered down (or up) to the window.
  ctx->PSSetSamplers(0, 1, (scale > 1 ? linearSampler : pointSampler).GetAddressOf());
  ctx->Draw(3, 0);
  ID3D11ShaderResourceView* none = nullptr;
  ctx->PSSetShaderResources(0, 1, &none);
  const D3D11_VIEWPORT whole{0, 0, float(rc.right), float(rc.bottom), 0, 1};
  ctx->RSSetViewports(1, &whole);
  const float pictureRight = vp.TopLeftX + vp.Width, cellHeight = float(std::max(2, int(rc.bottom) / 180));
  if (const MinimapView view = minimapView(mapVertices, mapVersion); view.visible) {
    const uint32_t size = uint32_t(std::max(64.0f, float(h) * 0.26f));
    const float margin = float(h) * 0.02f;  // and below the counter, in the same corner
    drawMap(view, pictureRight - float(size) - margin, vp.TopLeftY + std::max(margin, cellHeight * 8), size);
    ctx->VSSetShader(presentVs.Get(), nullptr, 0);
  }
  // A screenshot asked for (F12, --shot): the window as shown so far, without the counter.
  if (std::filesystem::path file; takeScreenshotRequest(file)) {
    ComPtr<ID3D11Texture2D> back, staging;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging))) {
      ctx->CopyResource(staging.Get(), back.Get());
      if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        const bool saved = savePng(file, td.Width, td.Height, static_cast<const uint8_t*>(map.pData), map.RowPitch);
        ctx->Unmap(staging.Get(), 0);
        std::printf("screenshot %s: %s\n", saved ? "saved" : "NOT saved", file.string().c_str());
      }
    }
  }
  if (const std::string& text = fps; !text.empty()) {  // the counter, in the window's top right corner
    const float cell = float(std::max(2, int(rc.bottom) / 180));
    struct {
      float x, y, cell;
      uint32_t count, glyphs[8];
    } c{float(rc.right) - (float(text.size() * 4 + 2)) * cell, cell, cell, uint32_t(std::min<size_t>(text.size(), 8)), {}};
    for (size_t i = 0; i < c.count; ++i) c.glyphs[i] = fpsGlyph(text[i]);
    setConstants(&c, sizeof(c));
    ctx->PSSetShader(fpsPs.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->OMSetBlendState(overBlend.Get(), nullptr, 0xFFFFFFFF);
    ctx->Draw(3, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  }
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  }

FrameGen* GsGpu::Impl::generator() {
  if (!frameGen && !frameGenFailed) {
    frameGen = std::make_unique<FrameGen>();
    std::string error;
    if (!frameGen->init(dev.Get(), ctx.Get(), error)) {
      std::printf("interpolation unavailable: %s\n", error.c_str());
      frameGen.reset(), frameGenFailed = true;
    }
  }
  return frameGen.get();
}


uint32_t GsGpu::scale() const { return impl_->scale; }

void GsGpu::readMemory(std::vector<uint32_t>& words) {
  Impl& m = *impl_;
  std::lock_guard lock(m.callMutex);
  m.memRead(Impl::Pages().set());
  D3D11_BUFFER_DESC bd;
  m.mem->GetDesc(&bd);
  bd.Usage = D3D11_USAGE_STAGING, bd.BindFlags = 0, bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, bd.MiscFlags = 0;
  ComPtr<ID3D11Buffer> staging;
  if (FAILED(m.dev->CreateBuffer(&bd, nullptr, &staging))) return;
  m.ctx->CopyResource(staging.Get(), m.mem.Get());
  D3D11_MAPPED_SUBRESOURCE map;
  if (FAILED(m.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
  words.assign(static_cast<const uint32_t*>(map.pData), static_cast<const uint32_t*>(map.pData) + GsMemory::kSize / 4);
  m.ctx->Unmap(staging.Get(), 0);
}

uint32_t GsGpu::picture(const Gs::Display& d, std::vector<uint32_t>& rgba) {
  Impl& m = *impl_;
  std::lock_guard lock(m.callMutex);
  static const bool scaled = std::getenv("CODPORT_DUMP_SCALED") != nullptr;
  const uint32_t scale = scaled ? m.scale : 1, w = d.width * scale, h = d.height * scale;
  rgba.assign(size_t(w) * h, 0);
  if (!m.makePicture(d, scale)) return scale;
  // For testing interpolation: the picture halfway from the last one, as build/frames/mid-N.png.
  if (static const bool mid = std::getenv("CODPORT_INTERP_DUMP") != nullptr; mid && m.generator()) {
    m.view.wide = wideScreen();
    const Impl::PictureDepth z = m.pictureDepth();
    m.frameGen->push(m.picture.Get(), w, h, scale, m.view, z.depth, m.scale, z.scene);
    if (ID3D11ShaderResourceView* made = m.frameGen->ready() ? m.frameGen->make(0.5f, w, h) : nullptr) {
      ComPtr<ID3D11Resource> res;
      made->GetResource(&res);
      m.ctx->CopyResource(m.staging.Get(), res.Get());
      D3D11_MAPPED_SUBRESOURCE map;
      if (SUCCEEDED(m.ctx->Map(m.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        savePng(iconFile().parent_path() / "frames" / std::format("mid-{:05}.png", m.frameNo), w, h, static_cast<const uint8_t*>(map.pData), map.RowPitch);
        m.ctx->Unmap(m.staging.Get(), 0);
      }
    }
  }
  m.ctx->CopyResource(m.staging.Get(), m.picture.Get());
  D3D11_MAPPED_SUBRESOURCE map;
  if (FAILED(m.ctx->Map(m.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return scale;
  for (uint32_t y = 0; y < h; ++y)
    std::memcpy(rgba.data() + size_t(y) * w, static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, size_t(w) * 4);
  m.ctx->Unmap(m.staging.Get(), 0);
  return scale;
}
