#pragma once
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

#include "ee/vu.h"
#include "gs/dmac.h"
#include "gs/gif.h"
#include "gs/gs.h"
#include "gs/vif.h"
#include "ipu/ipu.h"

class Hardware;
class Memory;

// A thread that works through commands handed to it in order. One producer thread collects
// them (post) and hands them over in batches (publish); each command is a header and its data.
class WorkThread {
public:
  struct alignas(16) Header {  // keeps the data after it 16-byte aligned
    uint32_t kind, qwc;        // what to do, and the qwords of data that follow
    uint32_t addr;
    uint64_t arg;
  };
  using Handler = std::function<void(const Header&, const u128* data)>;
  // `batchDone` runs on the thread after each batch, before the batch counts as done. With
  // `fastCore` the thread gets a fast core of its own (claimFastCore).
  WorkThread(std::string name, bool fastCore, Handler handler, std::function<void()> batchDone = {});
  ~WorkThread();

  void post(uint32_t kind, uint64_t arg, const void* data, uint32_t qwc, uint32_t addr = 0);
  uint8_t* postSpace(uint32_t kind, uint32_t qwc);  // like post; the caller fills the data
  void publish();
  bool wait();  // until everything published is done; true if it had to wait
  template <class Ready>
  void waitUntil(Ready ready) {  // checked whenever a batch is done
    std::unique_lock lock(mutex_);
    idle_.wait(lock, ready);
  }
  double cpuSeconds();

private:
  void run();
  std::string name_;
  bool fastCore_;
  Handler handler_;
  std::function<void()> batchDone_;
  std::vector<uint8_t> open_;  // being collected (producer)
  std::mutex mutex_;
  std::condition_variable wake_, idle_;
  std::deque<std::vector<uint8_t>> queue_;   // published, oldest first
  std::vector<std::vector<uint8_t>> spare_;  // done, kept for reuse
  size_t queuedBytes_ = 0;
  uint64_t published_ = 0, done_ = 0;  // batches
  bool stop_ = false;
  std::thread thread_;
};

// Everything between the EE and the picture: the DMA controller, VIF0/VIF1, VU1, the GIF, the
// GS and the IPU (video decoding), wired together and reachable from the EE's memory-mapped
// registers.
//
// As on the console, VU1 and the GS work alongside the EE, in a pipeline of two threads: one runs
// VIF1 and VU1, the other the GIF and the GS's register interface (feeding the GS workers). DMA
// transfers to VIF1 and the GIF still finish the moment the game starts them; their data is
// copied into a queue that the first thread works through in order, and it passes GIF packets
// (copied out of VU1 memory at XGKICK) on to the second, so everything happens exactly as if it
// ran on the spot. The EE waits for the threads only when it looks at state they change (VIF1
// registers, VU1 memory, SIGLBLID, a picture), and at each VBlank until they are at most a frame
// behind. The GS's SIGNAL and FINISH events reach the CSR at the VBlank after the frame whose
// work caused them, so the CSR never waits for the threads; a game that enables their interrupts
// gets them during the transfer instead, with the threads worked through after every transfer.
class Graphics {
public:
  explicit Graphics(Memory& mem);
  ~Graphics();

  bool read(uint32_t phys, uint64_t& value);
  bool write(uint32_t phys, uint64_t value);
  bool read128(uint32_t phys, void* qword);         // IPU output FIFO
  bool write128(uint32_t phys, const void* qword);  // VIF0/VIF1/GIF/IPU FIFOs

  void drain();  // waits until the threads have done everything queued (EE thread only)
  // Draws on the graphics card (Gs::useGpu), showing pictures in `window` (an HWND, or null).
  bool useGpu(void* window, uint32_t scale, std::string& error);
  // Once per VBlank: marks the frame and, with `picture`, has the picture as of now built in the
  // background (Gs::queueFrame). The EE waits here if the threads are more than a frame behind,
  // and the previous frame's GS events reach the CSR.
  void vblank(bool picture);
  double vuThreadCpuSeconds() { return vuThread_.cpuSeconds(); }
  double gsThreadCpuSeconds() { return gsThread_.cpuSeconds(); }

  Dmac dmac;
  Gs gs;
  Gif gif;
  u128 vu1Vf[32]{};
  uint32_t vu1Vi[32]{};
  u128 vu1Acc{};
  VuMicro vu0Micro, vu1;
  Vif vif0, vif1;
  Ipu ipu;
  uint64_t kickNanos = 0;  // time in XGKICK (copying the packets out), part of the VU1 time
  uint64_t drains = 0;     // times the EE waited for the threads

  // Where this frame's 2D layer starts and ends in the DMA chain the game will send to VIF1.
  void setUiLayerChain(uint32_t start, uint32_t end, bool narrow);

private:
  // Commands for the threads.
  enum Kind : uint32_t {
    kVif1Data, kVif1Tag,  // VIF1 thread: DMA data and chain tags for VIF1
    kGifData,             // both: GIF DMA (PATH3), passed on
    kPrivWrite,           // both: a privileged register (addr, arg), passed on
    kFrame,               // both: a VBlank (arg: picture wanted), passed on
    kKick, kDirect,       // GS thread: PATH1 packets (XGKICK), PATH2 (VIF1 DIRECT)
    kUiLayer,             // both: the drawing that follows is the game's 2D layer (arg) or not, passed on
  };
  // Takes VIF1's or the GIF's DMA transfers for the queue.
  struct QueuedSink : DmaSink {
    QueuedSink(Graphics& g, Kind data) : g(g), data(data) {}
    void dmaWrite(const u128* d, uint32_t qwc) override { g.vuThread_.post(data, 0, d, qwc); }
    void dmaTag(uint64_t upper) override {  // only VIF1 takes tags
      if (data == kVif1Data) g.vuThread_.post(kVif1Tag, upper, nullptr, 0);
    }
    void dmaDone() override { g.publish(); }
    Graphics& g;
    Kind data;
  };
  void publish();                // hands the collected work to the VIF1 thread
  bool eventsInterrupt() const;  // the game enabled the SIGNAL or FINISH interrupt
  void flushEvents();            // after drain(): every GS event so far reaches the CSR
  void raiseEvents(uint64_t bits);
  void vuWork(const WorkThread::Header& h, const u128* data);
  void gsWork(const WorkThread::Header& h, const u128* data);

  Hardware& hw_;
  QueuedSink vif1Sink_{*this, kVif1Data}, gifSink_{*this, kGifData};
  std::array<std::array<uint32_t, 3>, 4> uiChains_{};  // start, end, narrow
  uint32_t uiChainNext_ = 0;
  uint64_t framesPosted_ = 0;
  uint64_t gsFrames_ = 0;                 // GS thread
  std::atomic<uint64_t> framesDone_ = 0;  // ... VBlanks it has passed
  uint64_t frameEvents_[4] = {};          // GS events of each frame (by frame number % 4), set by it
  // The GS thread goes first: the VIF1 thread hands work to it, so it must outlive it.
  WorkThread gsThread_;
  WorkThread vuThread_;
};
