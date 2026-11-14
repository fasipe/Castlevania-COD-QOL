#include "gs/graphics.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "ee/memory.h"
#include "host/sampler.h"
#include "host/window.h"
#include "timer.h"

namespace {
constexpr uint32_t kVu0Code = 0x11000000, kVu0Data = 0x11004000, kVu1Code = 0x11008000, kVu1Data = 0x1100C000;
constexpr uint32_t kGsSiglblid = 0x12001080;
constexpr size_t kMaxQueued = 64 << 20;  // bytes of work a thread may lag behind before its producer waits
bool isVif1Reg(uint32_t phys) { return phys >= 0x10003C00 && phys < 0x10004000; }
// VIF1 registers whose value the threads change: not STAT (always idle here) or ERR (only the
// EE sets it), which games poll.
bool isVif1State(uint32_t phys) { return isVif1Reg(phys) && phys != 0x10003C00 && phys != 0x10003C20; }
bool isGsPriv(uint32_t phys) { return phys >= 0x12000000 && phys < 0x12001000; }
}  // namespace

// ---- WorkThread -------------------------------------------------------------------------------

WorkThread::WorkThread(std::string name, bool fastCore, Handler handler, std::function<void()> batchDone)
    : name_(std::move(name)), fastCore_(fastCore), handler_(std::move(handler)), batchDone_(std::move(batchDone)) {
  thread_ = std::thread([this] { run(); });
}

WorkThread::~WorkThread() {
  publish();
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

uint8_t* WorkThread::postSpace(uint32_t kind, uint32_t qwc) {
  const size_t at = open_.size();
  open_.resize(at + sizeof(Header) + size_t(qwc) * 16);
  const Header h{kind, qwc, 0, 0};
  std::memcpy(open_.data() + at, &h, sizeof h);
  return open_.data() + at + sizeof h;
}

void WorkThread::post(uint32_t kind, uint64_t arg, const void* data, uint32_t qwc, uint32_t addr) {
  const size_t at = open_.size();
  open_.resize(at + sizeof(Header) + size_t(qwc) * 16);
  const Header h{kind, qwc, addr, arg};
  std::memcpy(open_.data() + at, &h, sizeof h);
  if (qwc) std::memcpy(open_.data() + at + sizeof h, data, size_t(qwc) * 16);
}

void WorkThread::publish() {
  if (open_.empty()) return;
  {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queuedBytes_ < kMaxQueued; });
    queuedBytes_ += open_.size();
    queue_.push_back(std::move(open_));
    ++published_;
    open_.clear();
    if (!spare_.empty()) {
      open_ = std::move(spare_.back());
      spare_.pop_back();
    }
  }
  wake_.notify_one();
}

bool WorkThread::wait() {
  std::unique_lock lock(mutex_);
  if (done_ == published_) return false;
  idle_.wait(lock, [&] { return done_ == published_; });
  return true;
}

double WorkThread::cpuSeconds() { return threadCpuSeconds(thread_.native_handle()); }

void WorkThread::setTick(std::function<void()> tick) {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [&] { return !ticking_; });
  tick_ = std::move(tick);
}

void WorkThread::nudge() {
  {
    std::lock_guard lock(mutex_);
    nudged_ = std::min(nudged_ + 1, 4);
  }
  wake_.notify_one();
}

void WorkThread::run() {
  if (fastCore_) claimFastCore();
  sampleThisThread(name_);
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [&] { return stop_ || nudged_ || !queue_.empty(); });
    if (nudged_) {  // one tick per nudge, however many came while it was busy
      --nudged_;
      if (tick_) {
        ticking_ = true;
        lock.unlock();
        tick_();
        lock.lock();
        ticking_ = false;
        idle_.notify_all();
      }
      continue;
    }
    if (queue_.empty()) return;
    std::vector<uint8_t> batch = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    for (size_t pos = 0; pos < batch.size();) {
      Header h;
      std::memcpy(&h, batch.data() + pos, sizeof h);
      handler_(h, reinterpret_cast<const u128*>(batch.data() + pos + sizeof h));
      pos += sizeof h + size_t(h.qwc) * 16;
    }
    if (batchDone_) batchDone_();
    lock.lock();
    queuedBytes_ -= batch.size();
    batch.clear();
    spare_.push_back(std::move(batch));
    ++done_;
    idle_.notify_all();
  }
}

// ---- Graphics ---------------------------------------------------------------------------------

Graphics::Graphics(Memory& mem)
    : dmac(mem),
      gif(gs),
      vu0Micro(VuRegs{nullptr, nullptr, nullptr}, mem.hostPtr(kVu0Code, 4096), 4096, mem.hostPtr(kVu0Data, 4096), 4096),
      vu1(VuRegs{nullptr, nullptr, nullptr}, mem.hostPtr(kVu1Code, 16384), 16384, mem.hostPtr(kVu1Data, 16384), 16384),
      vif0(0, mem.hostPtr(kVu0Code, 4096), 4096, mem.hostPtr(kVu0Data, 4096), 4096, vu0Micro),
      vif1(1, mem.hostPtr(kVu1Code, 16384), 16384, mem.hostPtr(kVu1Data, 16384), 16384, vu1),
      ipu(dmac),
      hw_(mem.hw),
      // The VIF1 thread is the busier: it gets a fast core of its own; the GS thread (above the
      // GS workers' priority) shares theirs, which they need more.
      gsThread_("graphics (GS)", false, [this](const WorkThread::Header& h, const u128* d) { gsWork(h, d); }),
      vuThread_("graphics (VU1)", true, [this](const WorkThread::Header& h, const u128* d) { vuWork(h, d); },
                [this] { gsThread_.publish(); }) {
  vu1.vf_[0].f[3] = 1.0f;
  vu1.vi_[kVuR] = 0x3F800000;
  uint8_t* vu1Data = mem.hostPtr(kVu1Data, 16384);
  vu1.codeTouches = &mem.vuCodeTouches;
  // XGKICK: the packets are copied out of VU1 memory now, for the GS thread.
  vu1.xgkick = [this, vu1Data](uint32_t addr) {
    ScopedTimer timer(kickNanos);
    addr %= 1024;
    const uint32_t n = Gif::kickLength(vu1Data, 1024, addr);
    uint8_t* to = gsThread_.postSpace(kKick, n);
    const uint32_t first = std::min(n, 1024 - addr);
    std::memcpy(to, vu1Data + size_t(addr) * 16, size_t(first) * 16);
    std::memcpy(to + size_t(first) * 16, vu1Data, size_t(n - first) * 16);
  };
  vu1.top = [this] { return vif1.top(); };
  vu1.itop = [this] { return vif1.itop(); };
  vif1.direct = [this](const u128* data, uint32_t qwc) { gsThread_.post(kDirect, 0, data, qwc); };
  dmac.sinks[Dmac::kVif0] = &vif0;
  dmac.sinks[Dmac::kVif1] = &vif1Sink_;
  dmac.sinks[Dmac::kGif] = &gifSink_;
  hw_.beforeGsImr = [this] {
    drain();
    flushEvents();
  };
}

Graphics::~Graphics() {
  gs.onRefresh({});  // no more ticks for the GS thread
  vuThread_.publish();
  hw_.beforeGsImr = nullptr;
}

bool Graphics::read(uint32_t phys, uint64_t& value) {
  if (isVif1State(phys) || isGsPriv(phys) || phys == kGsSiglblid) drain();
  if (phys == kGsSiglblid) {
    value = gs.siglblid;
    return true;
  }
  return dmac.read(phys, value) || vif0.read(phys, value) || vif1.read(phys, value) || gs.readPriv(phys, value) ||
         ipu.read(phys, value);
}

bool Graphics::write(uint32_t phys, uint64_t value) {
  if (isGsPriv(phys)) {  // display settings: in order with the drawing before them
    vuThread_.post(kPrivWrite, value, nullptr, 0, phys);
    return true;
  }
  if (isVif1Reg(phys) || phys == kGsSiglblid) drain();
  if (phys == kGsSiglblid) {
    gs.siglblid = value;
    return true;
  }
  return dmac.write(phys, value) || vif0.write(phys, value) || vif1.write(phys, value) || ipu.write(phys, value);
}

bool Graphics::read128(uint32_t phys, void* qword) { return ipu.read128(phys, qword); }

bool Graphics::write128(uint32_t phys, const void* qword) {
  switch (phys) {
    case 0x10004000: vif0.fifoWrite(qword); return true;
    case 0x10005000: vuThread_.post(kVif1Data, 0, qword, 1); return true;
    case 0x10006000: vuThread_.post(kGifData, 0, qword, 1); return true;
  }
  return ipu.write128(phys, qword);
}

void Graphics::publish() {
  vuThread_.publish();
  if (eventsInterrupt()) {  // raised during the transfer, as on the console
    drain();
    flushEvents();
  }
}

bool Graphics::eventsInterrupt() const { return (~hw_.gsImr >> 8) & 3; }

void Graphics::flushEvents() {
  uint64_t bits = gs.csrEvents;
  for (uint64_t& e : frameEvents_) bits |= e, e = 0;
  gs.csrEvents = 0;
  raiseEvents(bits);
}

void Graphics::raiseEvents(uint64_t bits) {
  for (const uint64_t bit : {1ull, 2ull})  // SIGNAL, FINISH
    if (bits & bit) hw_.gsEvent(bit);
}

bool Graphics::useGpu(void* window, uint32_t scale, std::string& error) {
  drain();
  if (!gs.useGpu(window, scale, error)) return false;
  // Interpolation shows pictures at each refresh of the screen, from the GS thread.
  gsThread_.setTick([this] { gs.tick(); });
  gs.onRefresh([this] { gsThread_.nudge(); });
  return true;
}

bool Graphics::setScale(void* window, uint32_t scale, std::string& error) {
  drain();
  gs.onRefresh({});        // no more ticks while the GPU is made again
  gsThread_.setTick({});
  const bool ok = gs.setScale(window, scale, error);
  if (gs.usingGpu()) {
    gsThread_.setTick([this] { gs.tick(); });
    gs.onRefresh([this] { gsThread_.nudge(); });
  }
  return ok;
}

void Graphics::drain() {
  vuThread_.publish();
  const bool waited = vuThread_.wait();  // once the VIF1 thread is done, it has handed everything on
  if (gsThread_.wait() || waited) ++drains;
}

void Graphics::setUiLayerChain(uint32_t start, uint32_t end, bool narrow) {
  // The game may still be sending the previous frame's chain, which has other addresses.
  uiChains_[uiChainNext_++ % uiChains_.size()] = {start & 0x0FFFFFFFu, end & 0x0FFFFFFFu, narrow};
  if (dmac.vif1TagAt) return;
  dmac.vif1TagAt = [this](uint32_t at) {
    at &= 0x0FFFFFFFu;
    for (const auto& chain : uiChains_) {
      if (at == chain[0]) vuThread_.post(kUiLayer, chain[2], nullptr, 0);
      else if (at == chain[1]) vuThread_.post(kUiLayer, 0, nullptr, 0);
    }
  };
}

void Graphics::vblank(bool picture) {
  // When the VBlank came (steady clock, nanoseconds): the picture's moment, for interpolation.
  struct {
    uint64_t stamp;
    float yaw, pitch;
    uint32_t playing, pad[3];
  } shown{uint64_t(std::chrono::steady_clock::now().time_since_epoch() / std::chrono::nanoseconds(1)), viewYaw, viewPitch, viewPlaying, {}};
  vuThread_.post(kFrame, picture, &shown, 2);
  publish();
  const uint64_t frame = framesPosted_++;
  if (frame == 0) return;
  gsThread_.waitUntil([&] { return framesDone_.load(std::memory_order_acquire) >= frame; });
  raiseEvents(std::exchange(frameEvents_[(frame - 1) & 3], 0));
}

void Graphics::vuWork(const WorkThread::Header& h, const u128* data) {
  switch (h.kind) {
    case kVif1Data: vif1.dmaWrite(data, h.qwc); break;
    case kVif1Tag: vif1.dmaTag(h.arg); break;
    case kGifData: case kPrivWrite: case kFrame: case kUiLayer: gsThread_.post(h.kind, h.arg, data, h.qwc, h.addr); break;
  }
}

void Graphics::gsWork(const WorkThread::Header& h, const u128* data) {
  switch (h.kind) {
    case kKick: gif.kick(reinterpret_cast<const uint8_t*>(data), h.qwc, 0); break;
    case kDirect: gif.feed(2, data, h.qwc); break;
    case kGifData: gif.feed(3, data, h.qwc); break;
    case kPrivWrite: gs.writePriv(h.addr, h.arg); break;
    case kUiLayer: gs.setUiLayer(h.arg != 0); break;
    case kFrame:
      if (h.qwc) {
        uint64_t stamp;
        float view[2];
        uint32_t playing;
        std::memcpy(&stamp, data, 8), std::memcpy(view, reinterpret_cast<const uint8_t*>(data) + 8, 8);
        std::memcpy(&playing, reinterpret_cast<const uint8_t*>(data) + 16, 4);
        gs.setView(view[0], view[1], playing != 0);
        if (h.arg) gs.queueFrame(stamp);
      } else if (h.arg) {
        gs.queueFrame(0);
      }
      gs.frameEnd();
      frameEvents_[gsFrames_ & 3] = std::exchange(gs.csrEvents, 0);
      framesDone_.store(++gsFrames_, std::memory_order_release);
      break;
  }
}
