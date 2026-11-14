#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gs/gsmem.h"

class GsGpu;

// The Graphics Synthesizer: registers, local memory transfers and a software rasterizer.
// Drawing follows the GS rules closely enough to show the game's frames; the output picture is
// assembled from the display circuits (PMODE/DISPFB/DISPLAY) once per VBlank.
// Drawing runs on worker threads: primitives are queued and drawn in batches. A batch is split by
// groups of 16 rows, each drawn after the group's part of earlier batches. Each group has a
// worker it belongs to, which takes it first so its rows stay in that core's cache; a worker done
// with its own groups takes others still waiting. A new batch starts when a primitive samples memory
// that the batch draws into (or draws into memory it samples). A batch that samples memory
// queued batches draw into, or draws into memory they sample (or draw with another layout),
// starts once those have finished; the others go on in parallel. Transfers and palette loads
// happen right away when no queued drawing touches their memory, otherwise in order as a step
// that waits the same way.
// Only the displayed picture makes the emulated machine wait for the workers.
class Gs {
public:
  Gs();
  ~Gs();

  // Draws on the graphics card from now on (GsGpu); false (with `error`) if it cannot, and the
  // software rasterizer goes on. `window` (an HWND, or null) is where pictures are shown;
  // `scale` multiplies the drawing resolution (GsGpu::create).
  bool useGpu(void* window, uint32_t scale, std::string& error);
  // The GPU made again at another drawing resolution, from GS memory as it is (the frame buffers'
  // finer detail comes back as the game draws again); false if it could not, the old scale kept.
  bool setScale(void* window, uint32_t scale, std::string& error);
  bool usingGpu() const { return gpu_ != nullptr; }
  void frameEnd();  // a frame's drawing is all queued
  void setTextureFilter(bool on) { textureFilter_.store(on, std::memory_order_relaxed); }  // obey TEX1's linear filter (the GPU renderer does)
  void setUiLayer(bool on);  // the drawing that follows is the game's 2D interface: in 16:9, it is narrowed
  double gpuBusySeconds() const;  // the graphics card's working time so far (0 drawing on the CPU)

  // What the display circuits show (from PMODE, SMODE2, DISPFB and DISPLAY); false if none is on.
  struct Display {
    uint32_t fbp, fbw, psm, dbx, dby, width, height;
    bool field;  // interlaced, one field per buffer: each line shown twice
  };
  static bool display(const std::array<uint64_t, 16>& priv, Display& out);

  void flush();  // hands the queued primitives to the workers (drawn after earlier batches)
  void sync();   // waits until the workers have done everything queued

  struct Range {  // GS memory in 256-byte blocks
    uint32_t lo, hi;
    bool operator==(const Range&) const = default;
  };

  // SIGLBLID (set by SIGNAL and LABEL), and the CSR events (SIGNAL = 1, FINISH = 2) not yet
  // handed to Hardware, which keeps the CSR.
  uint64_t siglblid = 0, csrEvents = 0;
  uint64_t signals = 0, finishes = 0;  // events so far

  // Privileged registers at 0x12000000 (CSR and IMR are kept in Hardware).
  bool readPriv(uint32_t phys, uint64_t& value);
  bool writePriv(uint32_t phys, uint64_t value);

  void writeReg(uint32_t reg, uint64_t value);  // general register (A+D or PACKED/REGLIST)
  void writePacked(uint32_t reg, const uint64_t q[2]);
  void writeImage(const uint64_t* data, uint32_t count);  // HWREG data for a host->local transfer

  // Builds the displayed picture now (waiting for the workers); false if no circuit is enabled.
  bool frame(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height);
  // The same without waiting: the workers build the picture once everything queued is drawn,
  // and latestFrame() hands over the newest finished one (false if none since `serial`).
  void queueFrame(uint64_t stamp = 0);  // stamp: the VBlank's time (steady clock, ns), or 0
  // Interpolation (GsGpu): `wake` is called at each refresh of the screen (from another thread);
  // tick() then shows that moment's picture, on the thread that draws. Empty: no more calls.
  void onRefresh(std::function<void()> wake);
  void setView(float yaw, float pitch, bool playing);  // the camera's view in the picture shown next (GsGpu::setView)
  void tick();
  bool latestFrame(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height, uint64_t& serial);

  GsMemory mem;
  uint64_t primitives = 0, pixels = 0, drawNanos = 0, batches = 0;
  uint64_t throttleNanos = 0;
  uint64_t barrierBatches = 0, stepBatches = 0;  // batches waiting for others to finish; steps
  // Per worker: time spent drawing, and (worker 0) running steps; valid after sync().
  std::vector<uint64_t> workerDrawNanos;
  uint64_t stepNanos = 0;

private:
  friend class GsGpu;
  std::unique_ptr<GsGpu> gpu_;
  uint32_t clutSlot_ = 0;  // the palette newer primitives use, on the GPU
  bool uiNarrow_ = false;  // see setUiLayer
  std::atomic<bool> textureFilter_{false};  // the window's thread may change it while drawing goes on
  struct Vertex {
    int32_t x, y;  // 12.4 fixed point, window coordinates (XYOFFSET already subtracted)
    uint32_t z;
    uint8_t r, g, b, a;
    float s, t, q;
    uint32_t u, v;  // 10.4 fixed point (FST)
    uint8_t f;
  };
  struct Context {
    uint64_t tex0 = 0, tex1 = 0, tex2 = 0, clamp = 0, xyoffset = 0, miptbp1 = 0, miptbp2 = 0;
    uint64_t scissor = 0, alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0;
  };

  struct Draw;     // per-primitive decoded state, see raster.cpp
  struct Command;  // a queued primitive
  struct Band {  // the rows a worker draws a primitive in: one group of rows
    int y0, y1;
    bool owns(int y) const { return y >= y0 && y <= y1; }
  };
  static constexpr int kGroupShift = 4, kGroups = 2048 >> kGroupShift;  // GS coordinates reach 2047
  void vertexKick(bool drawing, bool fog);
  void drawPrimitive();
  // The last primitive's decoded state: valid (drawDirty_ false) while no register it depends on
  // changed and it is in the batch being collected, so strips skip decoding and the checks.
  std::unique_ptr<Draw> lastDraw_;
  bool drawDirty_ = true;
  void rasterize(const Command& c, Band band);
  void drawLine(const Draw& d, const Vertex& a, const Vertex& b, Band band);
  void drawTriangle(const Draw& d, const Vertex& a, const Vertex& b, const Vertex& c, Band band);
  void drawSprite(const Draw& d, const Vertex& a, const Vertex& b, Band band);
  enum PlotFeature : unsigned { kPlotTme = 1, kPlotFst = 2, kPlotFge = 4, kPlotAte = 8, kPlotZtest = 16, kPlotBlend = 32,
                                kPlotFb16 = 64 };
  struct Rows {  // the frame and depth buffer rows a pixel is on
    GsMemory::Row fb, z;
  };
  static Rows rowsOf(const Draw& d, int y);
  template <unsigned F>
  void plotT(const Draw& d, const Rows& rows, int x, uint32_t z, int r, int g, int b, int a, float s, float t, float q,
             int u, int v, uint8_t fog);
  using PlotFn = void (Gs::*)(const Draw&, const Rows&, int, uint32_t, int, int, int, int, float, float, float, int, int,
                              uint8_t);
  static const std::array<PlotFn, 128> kPlotFns;
  void plotAt(const Draw& d, int x, int y, uint32_t z, int r, int g, int b, int a, float s, float t, float q, int u,
              int v, uint8_t fog);  // one pixel anywhere (points, lines): scissor test, then plotT
  struct Span {  // attributes at the start of a triangle row and their steps per pixel
    double z, dz;
    float r, g, b, a, dr, dg, db, da, s, t, q, ds, dt, dq, u, v, du, dv, f, df;
  };
  template <unsigned F>
  void spanT(const Draw& d, int y, int xl, int xr, const Span& sp, const Vertex& flat);
  struct Lanes;  // eight pixels' attributes
  template <unsigned F>
  bool pixels8(const Draw& d, const Rows& rows, int x0, int n, const Lanes& l, const Vertex& flat);
  void fillSpan(const Draw& d, const Rows& rows, int xl, int xr, uint32_t z, int r, int g, int b, int a);
  using SpanFn = void (Gs::*)(const Draw&, int, int, int, const Span&, const Vertex&);
  static const std::array<SpanFn, 128> kSpanFns;
  static bool overlaps(const std::vector<Range>& list, Range r);
  // Blocks a w x h rectangle at (x, y) of a buffer touches: exact within one page, else whole pages.
  static Range rectRange(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  void worker(int index);
  void throttle();
  void startWorkers();
  uint32_t fetchTexel(const Draw& d, int u, int v) const;
  // fetchTexel for eight texels at once (in vector lanes); false for formats it does not do. With
  // `bytes`, also the byte address each texel was read from.
  bool fetchTexels8(const Draw& d, const int* u, const int* v, uint32_t* out, uint32_t* bytes = nullptr) const;
  void loadClut(uint64_t tex0);
  void startTransfer();
  void submit(std::function<void()> step, std::vector<Range> reads = {}, std::vector<Range> writes = {});
  struct Batch;
  void enqueue(Batch&& batch);
  void flushSteps();
  std::vector<std::function<void()>> steps_;  // being collected
  std::vector<Range> stepReads_, stepWrites_;
  uint64_t stepAfter_ = 0;  // batches they wait for
  bool clutInSteps_ = false;
  // Runs a memory operation now if no queued drawing touches its memory, else in order.
  void ordered(std::function<void()> op, std::vector<Range> reads, std::vector<Range> writes);
  // Memory a batch draws into, and with which layout (buffer pointer, width and format; steps
  // have their own).
  struct Access {
    Range r;
    uint32_t layout;
  };
  static constexpr uint32_t kStepLayout = ~0u;
  // How many batches must have finished before work with these accesses may run: the queued
  // ones that draw into what it reads, read what it writes, or write it with another layout
  // (the same layout is ordered row group by row group). 0 if none.
  uint64_t waitFor(const std::vector<Range>& reads, const std::vector<Access>& writes);
  // True if the batch being collected conflicts with these accesses in the same way.
  bool collectedConflict(const std::vector<Range>& reads, const std::vector<Access>& writes) const;
  // True if work touching this memory must wait: for the batch being collected, the steps being
  // collected or queued batches (a step's writes count as their own layout).
  bool mustWait(const std::vector<Range>& reads, const std::vector<Range>& writes);
  bool picture(const std::array<uint64_t, 16>& priv, std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height) const;
  std::mutex shownMutex_;
  std::vector<uint32_t> shown_;
  uint32_t shownWidth_ = 0, shownHeight_ = 0;
  uint64_t shownSerial_ = 0;
  uint64_t framesQueued_ = 0;              // pictures queueFrame() asked for
  std::atomic<uint64_t> framesDone_ = 0;  // ... and the workers built

  Context ctx_[2];
  uint64_t prim_ = 0, prmode_ = 0, prmodecont_ = 1, rgbaq_ = 0, st_ = 0, uv_ = 0, fog_ = 0;
  uint64_t texclut_ = 0, scanmsk_ = 0, texa_ = 0, fogcol_ = 0, dimx_ = 0, dthe_ = 0, colclamp_ = 1;
  uint64_t pabe_ = 0, bitbltbuf_ = 0, trxpos_ = 0, trxreg_ = 0, trxdir_ = 0, label_ = 0;
  uint64_t priv_[16] = {};  // PMODE .. BGCOLOR, by (offset >> 4)

  Vertex queue_[3];
  int queued_ = 0;
  int stripToggle_ = 0;

  // Host->local transfer being collected.
  bool trxActive_ = false;
  uint64_t trxNeeded_ = 0;  // 64-bit words the whole image takes
  std::vector<uint64_t> trxData_;
  void submitUpload();

  // The palette (expanded to 32-bit colours) newer primitives use. Each load makes a new one, so
  // queued primitives keep theirs; a load that has to wait for queued drawing is filled by the
  // workers, and clutReady_ is the batch count after which it is filled.
  using Clut = std::array<uint32_t, 256>;
  std::shared_ptr<Clut> clut_ = std::make_shared<Clut>();
  uint64_t clutReady_ = 0;
  uint32_t cbp0_ = ~0u, cbp1_ = ~0u;

  // Queued drawing and the workers that do it.
  struct Batch {
    std::vector<Command> commands;
    std::function<void()> step;  // instead of commands: run by one worker
    std::vector<Range> reads;
    std::vector<Access> writes;
    std::vector<std::shared_ptr<const Clut>> cluts;  // palettes the commands point to
    uint64_t after = 0;  // starts once this many batches have finished
    // Work for the workers: the row groups the commands touch, each with its commands (indices
    // into `order`, in submission order), and the group's previous batch (+ 1) to wait for.
    struct Item {
      uint32_t group, first, count;
      uint64_t after;
    };
    std::vector<Item> items;
    std::vector<uint32_t> order;
  };
  void binCommands(Batch& batch, uint64_t index);  // fills items/order
  static std::pair<int, int> rowsCovered(const Command& c);
  void retire(uint64_t index);                     // a batch's items are all done
  static constexpr uint64_t kMaxPending = 4096;
  struct Slot {  // a pending batch's items being handed out, by batch number % kMaxPending
    // Items taken, 32 per word, with the batch number's low half in the high half of each word.
    std::atomic<uint64_t> taken[kGroups / 32];
    std::atomic<uint32_t> items, left;
    uint8_t group[kGroups];       // each item's row group
    bool complete = false;        // under mutex_
  };
  static bool take(Slot& slot, uint64_t batch, uint32_t item);  // false if taken (or long done)
  std::array<Slot, kMaxPending> slots_;
  std::array<std::atomic<uint64_t>, kGroups> groupDone_{};  // per group: 1 + the last batch drawn in it
  std::array<uint64_t, kGroups> groupLast_{};                // ... submitted with it (front end)
  std::array<uint32_t, kGroups> groupCount_{};               // binCommands scratch
  std::vector<uint32_t> touched_;
  std::vector<Command> batch_;                   // being collected
  std::vector<std::shared_ptr<const Clut>> cluts_;
  std::vector<Access> written_;                  // memory the collected primitives draw into
  std::vector<Range> read_;                      // textures they sample
  uint64_t after_ = 0;                           // batches they wait for
  struct Query {  // what the last primitive accessed, and the batches that made it wait
    Range tex, fb, zb;
    uint32_t fbLayout, zbLayout;
    bool tme, ztest;
    bool operator==(const Query&) const = default;
  };
  Query lastQuery_{};
  uint64_t lastAfter_ = 0;
  std::vector<Range> accessReads_;
  std::vector<Access> accessWrites_;
  std::deque<Batch> pending_;                    // submitted, oldest first
  struct Tally {  // memory pending batches use: how many, and the latest
    Access a;
    int count;
    uint64_t last;
  };
  std::vector<Tally> busyReads_, busyWrites_;    // ranges the pending batches use
  static void tally(std::vector<Tally>& list, const std::vector<Access>& accesses, int delta, uint64_t index);
  std::atomic<uint64_t> submitted_ = 0, finished_ = 0;
  std::atomic<int> sleepers_ = 0;
  int workerCount_ = 0;
  std::vector<std::thread> workers_;
  std::vector<uint64_t> workerPixels_, workerNanos_;
  std::atomic<uint64_t> stepNanos_ = 0;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  bool stop_ = false;
};
