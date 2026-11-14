// GS software rasterizer: primitive setup and the per-pixel pipeline (texture, fog, tests,
// blending, masks). Register state is decoded once per primitive into a Draw block so the
// pixel loop only does arithmetic and memory accesses.
#include <algorithm>
#include <bit>
#include <immintrin.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <utility>

#include "gs/color.h"
#include "gs/draw.h"
#include "gs/gpu.h"
#include "gs/gs.h"
#include "host/sampler.h"
#include "host/window.h"
#include "timer.h"

namespace {
thread_local uint64_t tPixels = 0;

inline int floorToInt(float f) {
  const int i = int(f);
  return i - (float(i) > f);
}

}  // namespace

Gs::Range Gs::rectRange(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  uint32_t pageW = 64, pageH = 32, blockW = 8, blockH = 8, perRow = std::max(bw, 1u);
  uint32_t (*block)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = [](uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    return GsMemory::addr32(bp, bw, x, y, psm == PSMZ32 || psm == PSMZ24) / 64;
  };
  switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
      pageH = 64, blockW = 16;
      block = [](uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return GsMemory::addr16(bp, bw, x, y, psm) / 128; };
      break;
    case PSMT8:
      pageW = 128, pageH = 64, blockW = 16, blockH = 16, perRow = std::max(bw >> 1, 1u);
      block = [](uint32_t, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return GsMemory::addr8(bp, bw, x, y) / 256; };
      break;
    case PSMT4:
      pageW = 128, pageH = 128, blockW = 32, blockH = 16, perRow = std::max(bw >> 1, 1u);
      block = [](uint32_t, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return GsMemory::addr4(bp, bw, x, y) / 512; };
      break;
  }
  const uint32_t x1 = x + std::max(w, 1u) - 1, y1 = y + std::max(h, 1u) - 1;
  if (x / pageW != x1 / pageW || y / pageH != y1 / pageH) {
    const uint32_t first = x / pageW + y / pageH * perRow, last = x1 / pageW + y1 / pageH * perRow;
    return {bp + first * 32, bp + (last + 1) * 32};
  }
  Range r{~0u, 0};
  for (uint32_t by = y / blockH; by <= y1 / blockH; ++by)
    for (uint32_t bx = x / blockW; bx <= x1 / blockW; ++bx) {
      const uint32_t b = block(psm, bp, bw, bx * blockW, by * blockH);
      r.lo = std::min(r.lo, b), r.hi = std::max(r.hi, b + 1);
    }
  return r;
}

Gs::Gs() = default;

bool Gs::useGpu(void* window, uint32_t scale, std::string& error) {
  flush();
  flushSteps();
  sync();
  gpu_ = GsGpu::create(window, scale, error);
  if (!gpu_) return false;
  // GS memory so far goes to the GPU as one upload of the whole 4 MB (as 32-bit pixels).
  const uint64_t bitbltbuf = uint64_t(PSMCT32) << 56 | uint64_t(16) << 48;  // DBP 0, DBW 16 (1024 pixels)
  const uint64_t trxreg = 1024 | uint64_t(1024) << 32;
  std::vector<uint64_t> words(GsMemory::kSize / 8);
  for (uint32_t y = 0; y < 1024; ++y)
    for (uint32_t x = 0; x < 1024; ++x) {
      const uint32_t v = mem.read(PSMCT32, 0, 16, x, y);
      reinterpret_cast<uint32_t*>(words.data())[y * 1024 + x] = v;
    }
  gpu_->upload(bitbltbuf, 0, trxreg, words);
  return true;
}

bool Gs::setScale(void* window, uint32_t scale, std::string& error) {
  if (!gpu_) return false;
  flush();
  flushSteps();
  sync();
  std::vector<uint32_t> words(mem.words(), mem.words() + GsMemory::kSize / 4);
  gpu_->readMemory(words);
  std::copy(words.begin(), words.end(), mem.words());
  const uint32_t old = gpu_->scale();
  gpu_.reset();  // its swap chain first: the window takes one at a time
  if (useGpu(window, scale, error)) return true;
  std::string again;
  useGpu(window, old, again);
  return false;
}

Gs::~Gs() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (std::thread& t : workers_) t.join();
}

bool Gs::overlaps(const std::vector<Range>& list, Range r) {
  for (const Range& w : list)
    if (r.lo < w.hi && w.lo < r.hi) return true;
  return false;
}

void Gs::worker(int index) {
  lowerWorkerThread();  // the emulated CPUs feed the workers; they must not lose their core to them
  sampleThisThread("GS workers");
  // Waits for `ready`, spinning for a while first: batches often follow each other closely.
  auto waitFor = [&](auto ready) {
    for (int spin = 0; spin < 1000 && !ready(); ++spin) _mm_pause();
    if (ready()) return true;
    std::unique_lock lock(mutex_);
    ++sleepers_;
    wake_.wait(lock, [&] { return stop_ || ready(); });
    --sleepers_;
    return !stop_;
  };
  // Every worker goes through the batches in order: first the items of its own groups, then any
  // others still waiting. A group's items run in batch order; a batch that depends on all rows
  // (or is a step) first waits for every batch before it.
  for (uint64_t next = 0;; ++next) {
    if (!waitFor([&] { return next < submitted_.load(std::memory_order_acquire); })) return;
    Slot& slot = slots_[next % kMaxPending];
    const uint32_t count = slot.items.load(std::memory_order_acquire);
    for (int pass = 0; pass < 2; ++pass)
    for (uint32_t i = 0; i < count; ++i) {
      if ((pass == 0) != (slot.group[i] % workerCount_ == index)) continue;
      if (!take(slot, next, i)) continue;
      const Batch* batch;
      {
        std::lock_guard lock(mutex_);
        batch = &pending_[size_t(next - finished_.load())];
      }
      if (batch->after && !waitFor([&] { return finished_.load(std::memory_order_acquire) >= batch->after; })) return;
      const Batch::Item& item = batch->items[i];
      if (batch->step) {
        uint64_t nanos = 0;
        {
          ScopedTimer timer(nanos);
          batch->step();
        }
        stepNanos_ += nanos;
      } else if (item.count) {
        std::atomic<uint64_t>& done = groupDone_[item.group];
        if (!waitFor([&] { return done.load(std::memory_order_acquire) >= item.after; })) return;
        {
          ScopedTimer timer(workerNanos_[index]);
          const Band band{int(item.group) << kGroupShift, ((int(item.group) + 1) << kGroupShift) - 1};
          for (uint32_t k = 0; k < item.count; ++k) rasterize(batch->commands[batch->order[item.first + k]], band);
        }
        done.store(next + 1, std::memory_order_release);
        if (sleepers_.load() > 0) wake_.notify_all();  // someone may wait for this group
      }
      workerPixels_[index] += tPixels;
      tPixels = 0;
      if (slot.left.fetch_sub(1, std::memory_order_acq_rel) == 1) retire(next);
    }
  }
}

bool Gs::take(Slot& slot, uint64_t batch, uint32_t item) {
  std::atomic<uint64_t>& word = slot.taken[item / 32];
  const uint64_t bit = 1ull << (item % 32), tag = (batch & 0xFFFFFFFF) << 32;
  uint64_t w = word.load(std::memory_order_acquire);
  do {
    if ((w & ~0xFFFFFFFFull) != tag || (w & bit)) return false;
  } while (!word.compare_exchange_weak(w, w | bit, std::memory_order_acq_rel));
  return true;
}

// Batches finish in any order but retire in order.
void Gs::retire(uint64_t index) {
  {
    std::lock_guard lock(mutex_);
    slots_[index % kMaxPending].complete = true;
    while (finished_.load() < submitted_.load() && slots_[finished_.load() % kMaxPending].complete) {
      slots_[finished_.load() % kMaxPending].complete = false;
      std::vector<Access> reads;
      for (const Range& r : pending_.front().reads) reads.push_back({r, 0});
      tally(busyReads_, reads, -1, 0);
      tally(busyWrites_, pending_.front().writes, -1, 0);
      pending_.pop_front();
      finished_.fetch_add(1, std::memory_order_release);
    }
  }
  if (sleepers_.load() > 0) wake_.notify_all();
  done_.notify_all();
}

// The rows a primitive can draw in (empty if y0 > y1), as the draw functions below decide.
std::pair<int, int> Gs::rowsCovered(const Command& c) {
  const Vertex* v = c.v;
  int y0 = 0, y1 = -1;
  switch (c.type) {
    case 0: y0 = y1 = (v[0].y + 8) >> 4; break;
    case 1: case 2: y0 = std::min(v[0].y, v[1].y) >> 4, y1 = (std::max(v[0].y, v[1].y) + 15) >> 4; break;
    case 3: case 4: case 5:
      y0 = (std::min({v[0].y, v[1].y, v[2].y}) + 15) >> 4, y1 = (std::max({v[0].y, v[1].y, v[2].y}) - 1) >> 4;
      break;
    case 6: y0 = (std::min(v[0].y, v[1].y) + 15) >> 4, y1 = ((std::max(v[0].y, v[1].y) + 15) >> 4) - 1; break;
  }
  return {std::max(y0, c.d.sy0), std::min(y1, c.d.sy1)};
}

void Gs::binCommands(Batch& batch, uint64_t index) {
  for (const Command& c : batch.commands) {
    const auto [y0, y1] = rowsCovered(c);
    for (int g = std::max(y0, 0) >> kGroupShift; y0 <= y1 && g <= (y1 >> kGroupShift); ++g)
      if (groupCount_[g]++ == 0) touched_.push_back(uint32_t(g));
  }
  std::sort(touched_.begin(), touched_.end());
  uint32_t total = 0;
  for (uint32_t g : touched_) {
    batch.items.push_back({g, total, groupCount_[g], groupLast_[g]});
    groupLast_[g] = index + 1;
    const uint32_t n = groupCount_[g];
    groupCount_[g] = total;  // now where the group's next command goes
    total += n;
  }
  batch.order.resize(total);
  for (uint32_t i = 0; i < batch.commands.size(); ++i) {
    const Command& c = batch.commands[i];
    const auto [y0, y1] = rowsCovered(c);
    for (int g = std::max(y0, 0) >> kGroupShift; y0 <= y1 && g <= (y1 >> kGroupShift); ++g) batch.order[groupCount_[g]++] = i;
  }
  for (uint32_t g : touched_) groupCount_[g] = 0;
  touched_.clear();
}

void Gs::flush() {
  if (batch_.empty()) return;
  if (gpu_) {  // one draw call, after everything before it
    ++batches;
    gpu_->draw(batch_);
    batch_.clear();
    written_.clear();
    read_.clear();
    cluts_.clear();
    return;
  }
  ++batches;
  enqueue({std::move(batch_), {}, std::move(read_), std::move(written_), std::move(cluts_), after_});
  after_ = 0;
  batch_.clear();
  written_.clear();
  read_.clear();
  cluts_.clear();
}

void Gs::enqueue(Batch&& batch) {
  startWorkers();
  const uint64_t index = submitted_.load();
  if (batch.after <= finished_.load()) batch.after = 0;  // already there
  lastQuery_ = {};  // a new pending batch: ask again
  drawDirty_ = true;
  barrierBatches += batch.after > 0;
  stepBatches += bool(batch.step);
  if (!batch.step) binCommands(batch, index);
  if (batch.items.empty()) batch.items.push_back({0, 0, 0, 0});  // a step, or nothing to draw
  {
    std::lock_guard lock(mutex_);
    Slot& slot = slots_[index % kMaxPending];
    slot.items.store(uint32_t(batch.items.size()), std::memory_order_relaxed);
    slot.left.store(uint32_t(batch.items.size()), std::memory_order_relaxed);
    for (std::atomic<uint64_t>& w : slot.taken) w.store((index & 0xFFFFFFFF) << 32, std::memory_order_relaxed);
    for (size_t i = 0; i < batch.items.size(); ++i) slot.group[i] = uint8_t(batch.items[i].group);
    std::vector<Access> reads;
    for (const Range& r : batch.reads) reads.push_back({r, 0});
    tally(busyReads_, reads, +1, index);
    tally(busyWrites_, batch.writes, +1, index);
    pending_.push_back(std::move(batch));
    submitted_.fetch_add(1, std::memory_order_release);
  }
  if (sleepers_.load() > 0) wake_.notify_all();
  throttle();
}

// Counts how many unfinished batches use each range (batches mostly repeat the same few), and
// the latest of them: batches finish in order, so while any is unfinished, that one is.
void Gs::tally(std::vector<Tally>& list, const std::vector<Access>& accesses, int delta, uint64_t index) {
  for (const Access& a : accesses) {
    auto it = std::find_if(list.begin(), list.end(), [&](const Tally& t) {
      return t.a.r.lo == a.r.lo && t.a.r.hi == a.r.hi && t.a.layout == a.layout;
    });
    if (it == list.end()) it = list.insert(list.end(), {a, 0, 0});
    it->count += delta;
    if (delta > 0) it->last = index;
    if (it->count == 0) list.erase(it);
  }
}

void Gs::startWorkers() {
  if (workers_.empty()) {
    // One per logical processor the workers get, less one for the GS thread that shares them.
    int n = workerCpuCount() > 1 ? workerCpuCount() - 1 : int(std::clamp(std::thread::hardware_concurrency() * 3 / 8, 1u, 8u));
    if (const char* t = std::getenv("CODPORT_GS_THREADS")) n = std::max(1, std::atoi(t));
    workerCount_ = std::max(n, 1);
    workerPixels_.assign(size_t(workerCount_), 0);
    workerNanos_.assign(size_t(workerCount_), 0);
    for (int i = 0; i < workerCount_; ++i) workers_.emplace_back([this, i] { worker(i); });
  }
}

// Steps in a row are collected and handed over together: each hand-over makes the workers wait
// for one another, and movies upload hundreds of pieces per frame.
void Gs::submit(std::function<void()> step, std::vector<Range> reads, std::vector<Range> writes) {
  std::vector<Access> accesses;
  for (const Range& r : writes) accesses.push_back({r, kStepLayout});
  const bool afterCollected = collectedConflict(reads, accesses);
  flush();  // steps come after the primitives collected so far, waiting for them if they conflict
  stepAfter_ = std::max({stepAfter_, waitFor(reads, accesses), afterCollected ? submitted_.load() : 0});
  steps_.push_back(std::move(step));
  stepReads_.insert(stepReads_.end(), reads.begin(), reads.end());
  stepWrites_.insert(stepWrites_.end(), writes.begin(), writes.end());
  if (steps_.size() >= 1024) flushSteps();
}

void Gs::flushSteps() {
  if (steps_.empty()) return;
  std::vector<Access> writes;
  for (const Range& r : stepWrites_) writes.push_back({r, kStepLayout});
  enqueue({{}, [steps = std::move(steps_)] { for (const auto& step : steps) step(); }, std::move(stepReads_),
           std::move(writes), {}, stepAfter_});
  steps_.clear();
  stepReads_.clear();
  stepWrites_.clear();
  stepAfter_ = 0;
  if (clutInSteps_) clutReady_ = submitted_.load(), clutInSteps_ = false;
}

namespace {
bool conflict(Gs::Range a, Gs::Range b) { return a.lo < b.hi && b.lo < a.hi; }
}  // namespace

uint64_t Gs::waitFor(const std::vector<Range>& reads, const std::vector<Access>& writes) {
  uint64_t after = 0;
  std::lock_guard lock(mutex_);
  for (const Tally& t : busyWrites_) {
    for (const Range& r : reads)
      if (conflict(r, t.a.r)) after = std::max(after, t.last + 1);
    for (const Access& w : writes)
      if (conflict(w.r, t.a.r) && (w.layout != t.a.layout || w.layout == kStepLayout)) after = std::max(after, t.last + 1);
  }
  for (const Tally& t : busyReads_)
    for (const Access& w : writes)
      if (conflict(w.r, t.a.r)) after = std::max(after, t.last + 1);
  return after;
}

bool Gs::collectedConflict(const std::vector<Range>& reads, const std::vector<Access>& writes) const {
  for (const Access& c : written_) {
    for (const Range& r : reads)
      if (conflict(r, c.r)) return true;
    for (const Access& w : writes)
      if (conflict(w.r, c.r) && (w.layout != c.layout || w.layout == kStepLayout)) return true;
  }
  for (const Range& c : read_)
    for (const Access& w : writes)
      if (conflict(w.r, c)) return true;
  return false;
}

bool Gs::mustWait(const std::vector<Range>& reads, const std::vector<Range>& writes) {
  std::vector<Access> accesses;
  for (const Range& r : writes) accesses.push_back({r, kStepLayout});
  if (collectedConflict(reads, accesses)) return true;
  for (const Range& w : stepWrites_)  // in order with collected steps that touch the same memory
    for (const Range& r : reads)
      if (conflict(r, w)) return true;
  for (const Access& a : accesses) {
    for (const Range& r : stepReads_)
      if (conflict(a.r, r)) return true;
    for (const Range& w : stepWrites_)
      if (conflict(a.r, w)) return true;
  }
  return waitFor(reads, accesses) > finished_.load();
}

void Gs::ordered(std::function<void()> op, std::vector<Range> reads, std::vector<Range> writes) {
  if (mustWait(reads, writes))
    submit(std::move(op), std::move(reads), std::move(writes));
  else
    op();
}

// Keeps the emulated machine from running too far ahead of the drawing: a few frames' worth of
// batches, and (when pictures are queued for display) two pictures.
void Gs::throttle() {
  static_assert(kMaxPending > 1024 + 1);
  if (submitted_.load() - finished_.load() < 1024 && framesQueued_ - framesDone_.load() <= 2) return;
  ScopedTimer timer(throttleNanos);
  std::unique_lock lock(mutex_);
  done_.wait(lock, [&] { return submitted_.load() - finished_.load() < 512 && framesQueued_ - framesDone_.load() <= 2; });
}

void Gs::sync() {
  flush();
  flushSteps();
  ScopedTimer timer(drawNanos);
  std::unique_lock lock(mutex_);
  done_.wait(lock, [&] { return finished_.load() == submitted_.load(); });
  pixels = 0;
  for (uint64_t p : workerPixels_) pixels += p;
  workerDrawNanos = workerNanos_;
  stepNanos = stepNanos_;
}

void Gs::drawPrimitive() {
  ++primitives;
  flushSteps();
  if (!drawDirty_ && !batch_.empty()) {  // the state of the last primitive, in this batch
    batch_.push_back({*lastDraw_, {queue_[0], queue_[1], queue_[2]}, uint32_t(bits(prim_, 0, 3))});
    if (batch_.size() >= 16384) flush();
    return;
  }
  const uint64_t attr = prmodecont_ & 1 ? prim_ : prmode_;
  const Context& c = ctx_[bits(attr, 9, 1)];
  Draw d;
  const uint64_t frame = c.frame, test = c.test, zbuf = c.zbuf, alpha = c.alpha, tex0 = c.tex0, clamp = c.clamp;
  d.fbp = bits(frame, 0, 9) * 32;
  d.fbw = bits(frame, 16, 6);
  d.fpsm = bits(frame, 24, 6);
  d.fbmsk = bits(frame, 32, 32);
  d.fb16 = d.fpsm == PSMCT16 || d.fpsm == PSMCT16S;
  d.fb24 = d.fpsm == PSMCT24;
  d.zbp = bits(zbuf, 0, 9) * 32;
  d.zpsm = bits(zbuf, 24, 4) | 0x30;
  d.zmax = d.zpsm == PSMZ32 ? 0xFFFFFFFFu : d.zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
  d.ztest = bits(test, 16, 1);
  d.ztst = bits(test, 17, 2);
  d.zwrite = d.ztest && !bits(zbuf, 32, 1);
  d.ate = bits(test, 0, 1);
  d.atst = bits(test, 1, 3);
  d.aref = int(bits(test, 4, 8));
  d.afail = bits(test, 12, 2);
  d.date = bits(test, 14, 1) && !d.fb24;
  d.datm = bits(test, 15, 1);
  d.blend = bits(attr, 6, 1);
  d.pabe = bits(pabe_, 0, 1);
  d.fba = bits(c.fba, 0, 1);
  d.colclamp = bits(colclamp_, 0, 1);
  d.ba = int(bits(alpha, 0, 2));
  d.bb = int(bits(alpha, 2, 2));
  d.bc = int(bits(alpha, 4, 2));
  d.bd = int(bits(alpha, 6, 2));
  d.fix = int(bits(alpha, 32, 8));
  d.tme = bits(attr, 4, 1);
  d.fst = bits(attr, 8, 1);
  // No mipmaps in this game (MXL 0), so MMIN's linear kinds and MMAG are all there is to it.
  const uint32_t mmin = bits(c.tex1, 6, 3);
  d.linear = textureFilter_.load(std::memory_order_relaxed) && d.tme && (bits(c.tex1, 5, 1) || mmin == 1 || mmin >= 4);
  d.tbp = bits(tex0, 0, 14);
  d.tbw = bits(tex0, 14, 6);
  d.tpsm = bits(tex0, 20, 6);
  d.tw = 1 << std::min(bits(tex0, 26, 4), 10u);
  d.th = 1 << std::min(bits(tex0, 30, 4), 10u);
  d.tcc = bits(tex0, 34, 1);
  d.tfx = bits(tex0, 35, 2);
  d.csa = bits(tex0, 56, 5) * 16;
  d.wms = bits(clamp, 0, 2);
  d.wmt = bits(clamp, 2, 2);
  d.minu = bits(clamp, 4, 10);
  d.maxu = bits(clamp, 14, 10);
  d.minv = bits(clamp, 24, 10);
  d.maxv = bits(clamp, 34, 10);
  d.aem = bits(texa_, 15, 1);
  d.ta0 = bits(texa_, 0, 8);
  d.ta1 = bits(texa_, 32, 8);
  d.fge = bits(attr, 5, 1);
  d.fogR = int(bits(fogcol_, 0, 8));
  d.fogG = int(bits(fogcol_, 8, 8));
  d.fogB = int(bits(fogcol_, 16, 8));
  d.sx0 = int(bits(c.scissor, 0, 11));
  d.sx1 = int(bits(c.scissor, 16, 11));
  d.sy0 = int(bits(c.scissor, 32, 11));
  d.sy1 = int(bits(c.scissor, 48, 11));
  d.iip = bits(attr, 3, 1);
  d.fill = !d.tme && !d.fge && !d.blend && !d.date && (!d.ztest || d.ztst == 1);
  const unsigned features = (d.tme ? kPlotTme : 0) | (d.fst ? kPlotFst : 0) | (d.fge ? kPlotFge : 0) | (d.ate ? kPlotAte : 0) |
                            (d.ztest ? kPlotZtest : 0) | (d.blend ? kPlotBlend : 0) | (d.fb16 ? kPlotFb16 : 0);
  d.plot = kPlotFns[features];
  d.span = kSpanFns[features];

  // Queued primitives finish first if this one samples memory they draw into, or draws into
  // memory they sample.
  const uint32_t sw = uint32_t(std::max(d.sx1 - d.sx0 + 1, 1)), sh = uint32_t(std::max(d.sy1 - d.sy0 + 1, 1));
  const Range tex = rectRange(d.tpsm, d.tbp, d.tbw, 0, 0, d.tw, d.th);
  const Range fb = rectRange(d.fpsm, d.fbp, d.fbw, d.sx0, d.sy0, sw, sh);
  const Range zb = rectRange(d.zpsm, d.zbp, d.fbw, d.sx0, d.sy0, sw, sh);
  auto touch = [](Range p, Range q) { return p.lo < q.hi && q.lo < p.hi; };
  d.texFeedback = d.tme && (touch(tex, fb) || (d.ztest && touch(tex, zb)));
  d.selfRead = d.ztest && touch(fb, zb);
  // A picture read while it is drawn stays unfiltered: half a texel back is what it has just written,
  // which only GS memory can do, at a cost far above what the filter gives there.
  if (d.texFeedback) d.linear = false;
  // The same memory drawn with another layout maps its pixels to other row groups.
  const uint32_t fbLayout = d.fbp | d.fbw << 14 | d.fpsm << 20, zbLayout = d.zbp | d.fbw << 14 | d.zpsm << 20;
  std::vector<Range>& reads = accessReads_;
  std::vector<Access>& writes = accessWrites_;
  reads.clear(), writes.clear();
  if (d.tme) reads.push_back(tex);
  writes.push_back({fb, fbLayout});
  if (d.ztest) writes.push_back({zb, zbLayout});
  if (collectedConflict(reads, writes)) {
    flush();
    after_ = submitted_.load();  // the batch just handed over finishes first
  }
  d.clutSlot = clutSlot_;
  // The batches queued before: asked once per change of what the primitives access (the GPU
  // keeps draw calls in order by itself).
  const Query query{tex, fb, zb, fbLayout, zbLayout, d.tme, d.ztest};
  if (!gpu_ && !(query == lastQuery_)) {
    lastQuery_ = query;
    lastAfter_ = waitFor(reads, writes);
  }
  after_ = std::max({after_, lastAfter_, finished_.load() < clutReady_ ? clutReady_ : 0});  // and the palette's step
  if (cluts_.empty() || cluts_.back() != clut_) cluts_.push_back(clut_);
  d.clut = clut_->data();
  auto addWrite = [&](Access a) {
    for (const Access& w : written_)
      if (w.r.lo == a.r.lo && w.r.hi == a.r.hi && w.layout == a.layout) return;
    written_.push_back(a);
  };
  addWrite({fb, fbLayout});
  if (d.ztest) addWrite({zb, zbLayout});
  if (d.tme && std::find_if(read_.begin(), read_.end(), [&](const Range& r) { return r.lo == tex.lo && r.hi == tex.hi; }) == read_.end())
    read_.push_back(tex);
  batch_.push_back({d, {queue_[0], queue_[1], queue_[2]}, uint32_t(bits(prim_, 0, 3))});
  if (!lastDraw_) lastDraw_ = std::make_unique<Draw>();
  *lastDraw_ = d;
  drawDirty_ = false;
  if (batch_.size() >= 16384) flush();
}

void Gs::rasterize(const Command& c, Band band) {
  const Draw& d = c.d;
  switch (c.type) {
    case 0: {
      const Vertex& v = c.v[0];
      const int y = (v.y + 8) >> 4;
      if (band.owns(y)) plotAt(d, (v.x + 8) >> 4, y, v.z, v.r, v.g, v.b, v.a, v.s, v.t, v.q, int(v.u), int(v.v), v.f);
      break;
    }
    case 1: case 2: drawLine(d, c.v[0], c.v[1], band); break;
    case 3: case 4: case 5: drawTriangle(d, c.v[0], c.v[1], c.v[2], band); break;
    case 6: drawSprite(d, c.v[0], c.v[1], band); break;
  }
}

void Gs::drawLine(const Draw& d, const Vertex& a, const Vertex& b, Band band) {
  const float x0 = a.x / 16.0f, y0 = a.y / 16.0f, x1 = b.x / 16.0f, y1 = b.y / 16.0f;
  const int steps = std::max(1, int(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0))));
  for (int i = 0; i <= steps; ++i) {
    const float t = float(i) / steps;
    auto lerp = [&](float p, float q) { return p + (q - p) * t; };
    const int y = int(std::lround(lerp(y0, y1)));
    if (!band.owns(y)) continue;
    plotAt(d, int(std::lround(lerp(x0, x1))), y, uint32_t(lerp(float(a.z), float(b.z))),
         d.iip ? int(lerp(a.r, b.r)) : b.r, d.iip ? int(lerp(a.g, b.g)) : b.g, d.iip ? int(lerp(a.b, b.b)) : b.b,
         d.iip ? int(lerp(a.a, b.a)) : b.a, lerp(a.s, b.s), lerp(a.t, b.t), lerp(a.q, b.q),
         int(lerp(float(a.u), float(b.u))), int(lerp(float(a.v), float(b.v))), uint8_t(lerp(a.f, b.f)));
  }
}

void Gs::drawSprite(const Draw& d, const Vertex& a, const Vertex& b, Band band) {
  // Pixels whose top-left corner lies in [x0, x1) x [y0, y1); colour, depth and fog come from
  // the second vertex, texture coordinates are interpolated across the rectangle.
  const Vertex& lo = a.x <= b.x ? a : b;
  const Vertex& hi = a.x <= b.x ? b : a;
  const Vertex& top = a.y <= b.y ? a : b;
  const Vertex& bot = a.y <= b.y ? b : a;
  const int x0 = std::max((lo.x + 15) >> 4, d.sx0), x1 = std::min((hi.x + 15) >> 4, d.sx1 + 1);
  const int y0 = std::max((top.y + 15) >> 4, d.sy0), y1 = std::min((bot.y + 15) >> 4, d.sy1 + 1);
  if (x0 >= x1 || std::max(y0, band.y0) > std::min(y1 - 1, band.y1)) return;
  const float dx = float(hi.x - lo.x), dy = float(bot.y - top.y);
  const float sStep = dx > 0 ? (hi.s - lo.s) * 16.0f / dx : 0, uStep = dx > 0 ? (float(hi.u) - float(lo.u)) * 16.0f / dx : 0;
  const float tStep = dy > 0 ? (bot.t - top.t) * 16.0f / dy : 0, vStep = dy > 0 ? (float(bot.v) - float(top.v)) * 16.0f / dy : 0;
  const float s0 = lo.s + sStep * (x0 * 16.0f - lo.x) / 16.0f, u0 = float(lo.u) + uStep * (x0 * 16.0f - lo.x) / 16.0f;
  for (int y = std::max(y0, band.y0); y < y1 && y <= band.y1; ++y) {
    const float fy = (y * 16.0f - top.y) / 16.0f;
    const float t = top.t + tStep * fy, v = float(top.v) + vStep * fy;
    if (b.z <= 4294967040u) {  // spans hold depth as a double they clamp below 2^32
      const float r = b.r, g = b.g, bl = b.b, al = b.a, f = b.f;
      const Span span{double(b.z), 0, r, g, bl, al, 0, 0, 0, 0, s0, t, b.q, sStep, 0, 0, u0, v, uStep, 0, f, 0};
      (this->*d.span)(d, y, x0, x1 - 1, span, b);
      continue;
    }
    const Rows rows = rowsOf(d, y);
    float s = s0, u = u0;
    for (int x = x0; x < x1; ++x, s += sStep, u += uStep)
      (this->*d.plot)(d, rows, x, b.z, b.r, b.g, b.b, b.a, s, t, b.q, int(u), int(v), b.f);
  }
}

void Gs::drawTriangle(const Draw& d, const Vertex& v0, const Vertex& v1, const Vertex& v2, Band band) {
  const int64_t area = int64_t(v1.x - v0.x) * (v2.y - v0.y) - int64_t(v2.x - v0.x) * (v1.y - v0.y);
  if (area == 0) return;
  const int minX = std::max(d.sx0, (std::min({v0.x, v1.x, v2.x}) + 15) >> 4);
  const int maxX = std::min(d.sx1, (std::max({v0.x, v1.x, v2.x}) - 1) >> 4);
  const int minY = std::max(d.sy0, (std::min({v0.y, v1.y, v2.y}) + 15) >> 4);
  const int maxY = std::min(d.sy1, (std::max({v0.y, v1.y, v2.y}) - 1) >> 4);
  if (minX > maxX || std::max(minY, band.y0) > std::min(maxY, band.y1)) return;

  // Edge functions E(x, y) = A*x + B*y + C, oriented so the inside is positive; a pixel exactly
  // on an edge belongs to it only for top or left edges.
  struct Edge {
    int64_t a, b, c;
    bool owns;
  };
  auto makeEdge = [&](const Vertex& p, const Vertex& q) {
    Edge e{-(int64_t(q.y) - p.y), int64_t(q.x) - p.x, 0, false};
    e.c = -(e.a * p.x + e.b * p.y);
    if (area < 0) e.a = -e.a, e.b = -e.b, e.c = -e.c;
    e.owns = area > 0 ? ((q.y < p.y) || (q.y == p.y && q.x > p.x)) : ((q.y > p.y) || (q.y == p.y && q.x < p.x));
    return e;
  };
  const Edge e0 = makeEdge(v1, v2), e1 = makeEdge(v2, v0), e2 = makeEdge(v0, v1);

  // Attribute planes: value(x, y) = base + dx * (x - x0) + dy * (y - y0), in pixel units.
  const float fx0 = v0.x / 16.0f, fy0 = v0.y / 16.0f;
  const float ax = v1.x / 16.0f - fx0, ay = v1.y / 16.0f - fy0, bx = v2.x / 16.0f - fx0, by = v2.y / 16.0f - fy0;
  const float det = ax * by - bx * ay;
  struct Plane {
    float base, dx, dy;
    float at(float x, float y) const { return base + dx * x + dy * y; }
  };
  auto plane = [&](float a0, float a1, float a2) {
    const float da = a1 - a0, db = a2 - a0;
    return Plane{a0, (da * by - db * ay) / det, (db * ax - da * bx) / det};
  };
  const Plane pz = plane(float(v0.z), float(v1.z), float(v2.z));
  const Plane pr = plane(v0.r, v1.r, v2.r), pg = plane(v0.g, v1.g, v2.g), pb = plane(v0.b, v1.b, v2.b), pa = plane(v0.a, v1.a, v2.a);
  const Plane ps = plane(v0.s, v1.s, v2.s), pt = plane(v0.t, v1.t, v2.t), pq = plane(v0.q, v1.q, v2.q);
  const Plane pu = plane(float(v0.u), float(v1.u), float(v2.u)), pv = plane(float(v0.v), float(v1.v), float(v2.v));
  const Plane pf = plane(v0.f, v1.f, v2.f);
  const Vertex& flat = v2;  // flat shading takes the last vertex's colour
  const bool stq = d.tme && !d.fst, uv = d.tme && d.fst;
  const Edge* edges[3] = {&e0, &e1, &e2};

  for (int y = std::max(minY, band.y0); y <= std::min(maxY, band.y1); ++y) {
    // The covered pixels of a row form one span; find it from the edge equations instead of
    // testing every pixel of the bounding box.
    const int64_t py = int64_t(y) * 16;
    int64_t base[3];
    int xl = minX, xr = maxX;
    bool empty = false;
    for (int i = 0; i < 3; ++i) {
      const Edge& e = *edges[i];
      base[i] = e.b * py + e.c;
      if (e.a > 0) {
        xl = std::max(xl, int(std::floor(-double(base[i]) / double(e.a * 16))) - 1);
      } else if (e.a < 0) {
        xr = std::min(xr, int(std::ceil(-double(base[i]) / double(e.a * 16))) + 1);
      } else if (e.owns ? base[i] < 0 : base[i] <= 0) {
        empty = true;
      }
    }
    if (empty) continue;
    auto inside = [&](int x) {
      for (int i = 0; i < 3; ++i) {
        const int64_t w = edges[i]->a * x * 16 + base[i];
        if (edges[i]->owns ? w < 0 : w <= 0) return false;
      }
      return true;
    };
    while (xl <= xr && !inside(xl)) ++xl;
    while (xr >= xl && !inside(xr)) --xr;
    if (xl > xr) continue;

    const float ry = float(y) - fy0, rx = float(xl) - fx0;
    double z = double(pz.base) + double(pz.dx) * rx + double(pz.dy) * ry;
    float r = 0, g = 0, b = 0, a = 0, fs = 0, ft = 0, fq = 0, fu = 0, fv = 0, f = 255;
    if (d.iip) r = pr.at(rx, ry), g = pg.at(rx, ry), b = pb.at(rx, ry), a = pa.at(rx, ry);
    if (stq) fs = ps.at(rx, ry), ft = pt.at(rx, ry), fq = pq.at(rx, ry);
    if (uv) fu = pu.at(rx, ry), fv = pv.at(rx, ry);
    if (d.fge) f = pf.at(rx, ry);
    const Span span{z, pz.dx, r, g, b, a, pr.dx, pg.dx, pb.dx, pa.dx, fs, ft, fq, ps.dx, pt.dx, pq.dx, fu, fv, pu.dx, pv.dx, f, pf.dx};
    (this->*d.span)(d, y, xl, xr, span, flat);
  }
}

// A row of a triangle: attributes stepped across the span, each pixel through plotT inlined.
template <unsigned F>
void Gs::spanT(const Draw& d, int y, int xl, int xr, const Span& sp, const Vertex& flat) {
  constexpr bool stq = (F & kPlotTme) && !(F & kPlotFst), uv = (F & kPlotTme) && (F & kPlotFst), fge = F & kPlotFge;
  double z = sp.z;
  float r = sp.r, g = sp.g, b = sp.b, a = sp.a, fs = sp.s, ft = sp.t, fq = sp.q, fu = sp.u, fv = sp.v, f = sp.f;
  const Rows rows = rowsOf(d, y);
  if (d.fill && sp.dz == 0 && (!d.iip || (sp.dr == 0 && sp.dg == 0 && sp.db == 0 && sp.da == 0))) {
    if (d.iip)
      fillSpan(d, rows, xl, xr, uint32_t(std::clamp(z, 0.0, 4294967040.0)), std::clamp(int(r + 0.5f), 0, 255),
               std::clamp(int(g + 0.5f), 0, 255), std::clamp(int(b + 0.5f), 0, 255), std::clamp(int(a + 0.5f), 0, 255));
    else
      fillSpan(d, rows, xl, xr, uint32_t(std::clamp(z, 0.0, 4294967040.0)), flat.r, flat.g, flat.b, flat.a);
    return;
  }
  if (!d.selfRead) {  // eight pixels at a time; attributes stepped exactly as below
    Lanes l{};
    for (int x0 = xl; x0 <= xr; x0 += 8) {
      const int n = std::min(8, xr - x0 + 1);
      for (int k = 0; k < n; ++k) {
        l.z[k] = z;
        z += sp.dz;
        if (d.iip) l.r[k] = r, l.g[k] = g, l.b[k] = b, l.a[k] = a, r += sp.dr, g += sp.dg, b += sp.db, a += sp.da;
        if constexpr (stq) l.s[k] = fs, l.t[k] = ft, l.q[k] = fq, fs += sp.ds, ft += sp.dt, fq += sp.dq;
        if constexpr (uv) l.u[k] = fu, l.v[k] = fv, fu += sp.du, fv += sp.dv;
        if constexpr (fge) l.f[k] = f, f += sp.df;
      }
      if (pixels8<F>(d, rows, x0, n, l, flat)) continue;
      for (int k = 0; k < n; ++k) {  // the lanes' values, as the loop below would have them
        int ir = flat.r, ig = flat.g, ib = flat.b, ia = flat.a;
        if (d.iip) {
          ir = std::clamp(int(l.r[k] + 0.5f), 0, 255), ig = std::clamp(int(l.g[k] + 0.5f), 0, 255);
          ib = std::clamp(int(l.b[k] + 0.5f), 0, 255), ia = std::clamp(int(l.a[k] + 0.5f), 0, 255);
        }
        plotT<F>(d, rows, x0 + k, uint32_t(std::clamp(l.z[k], 0.0, 4294967040.0)), ir, ig, ib, ia, l.s[k], l.t[k], l.q[k],
                 int(l.u[k]), int(l.v[k]), uint8_t(fge ? std::clamp(l.f[k], 0.0f, 255.0f) : 255.0f));
      }
    }
    return;
  }
  for (int x = xl; x <= xr; ++x) {
    int ir = flat.r, ig = flat.g, ib = flat.b, ia = flat.a;
    if (d.iip) {
      ir = std::clamp(int(r + 0.5f), 0, 255), ig = std::clamp(int(g + 0.5f), 0, 255);
      ib = std::clamp(int(b + 0.5f), 0, 255), ia = std::clamp(int(a + 0.5f), 0, 255);
      r += sp.dr, g += sp.dg, b += sp.db, a += sp.da;
    }
    plotT<F>(d, rows, x, uint32_t(std::clamp(z, 0.0, 4294967040.0)), ir, ig, ib, ia, fs, ft, fq, int(fu), int(fv),
             uint8_t(fge ? std::clamp(f, 0.0f, 255.0f) : 255.0f));
    z += sp.dz;
    if constexpr (stq) fs += sp.ds, ft += sp.dt, fq += sp.dq;
    if constexpr (uv) fu += sp.du, fv += sp.dv;
    if constexpr (fge) f += sp.df;
  }
}

struct Gs::Lanes {
  alignas(32) double z[8];
  alignas(32) float r[8], g[8], b[8], a[8], s[8], t[8], q[8], u[8], v[8], f[8];
};

namespace {
using V = __m256i;
inline V clamp255(V v) { return _mm256_min_epi32(_mm256_max_epi32(v, _mm256_setzero_si256()), _mm256_set1_epi32(255)); }
inline V ones() { return _mm256_set1_epi32(-1); }
inline int laneMask(V m) { return _mm256_movemask_ps(_mm256_castsi256_ps(m)); }
inline V geU(V a, V b) { return _mm256_cmpeq_epi32(_mm256_max_epu32(a, b), a); }  // a >= b, unsigned
// GsMemory::Row::at for x0 + lane.
inline V rowAt(const GsMemory::Row& row, int x0) {
  const V x = _mm256_add_epi32(_mm256_set1_epi32(x0), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
  const V t = _mm256_and_si256(_mm256_i32gather_epi32(reinterpret_cast<const int*>(row.swizzle), _mm256_and_si256(x, _mm256_set1_epi32(63)), 2),
                               _mm256_set1_epi32(0xFFFF));
  const V page = _mm256_mullo_epi32(_mm256_srli_epi32(x, 6), _mm256_set1_epi32(int(row.pageUnits)));
  return _mm256_and_si256(_mm256_add_epi32(_mm256_add_epi32(_mm256_set1_epi32(int(row.base)), page), t), _mm256_set1_epi32(int(row.mask)));
}
// uint32_t(std::clamp(z, 0.0, 4294967040.0)) for eight doubles.
inline V depthLanes(const double* z) {
  auto half = [](__m256d v) {
    v = _mm256_floor_pd(_mm256_min_pd(_mm256_max_pd(v, _mm256_setzero_pd()), _mm256_set1_pd(4294967040.0)));
    return _mm_xor_si128(_mm256_cvttpd_epi32(_mm256_sub_pd(v, _mm256_set1_pd(2147483648.0))), _mm_set1_epi32(INT32_MIN));
  };
  return _mm256_setr_m128i(half(_mm256_load_pd(z)), half(_mm256_load_pd(z + 4)));
}
inline V from16v(V c) {
  const V m = _mm256_set1_epi32(0x1F);
  const V r = _mm256_slli_epi32(_mm256_and_si256(c, m), 3);
  const V g = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 5), m), 11);
  const V b = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 10), m), 19);
  const V a = _mm256_slli_epi32(_mm256_and_si256(c, _mm256_set1_epi32(0x8000)), 16);
  return _mm256_or_si256(_mm256_or_si256(r, g), _mm256_or_si256(b, a));
}
inline V to16v(V c) {
  const V m = _mm256_set1_epi32(0x1F);
  const V r = _mm256_and_si256(_mm256_srli_epi32(c, 3), m);
  const V g = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 11), m), 5);
  const V b = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 19), m), 10);
  const V a = _mm256_slli_epi32(_mm256_srli_epi32(c, 31), 15);
  return _mm256_or_si256(_mm256_or_si256(r, g), _mm256_or_si256(b, a));
}
}  // namespace

// plotT for up to eight pixels of a row at once (lanes k < n are x0 + k), with the same results:
// the per-pixel arithmetic in vector lanes, addressing, texel fetches and stores per lane. All
// reads come before any write, so for a texture overlapping the buffers it returns false
// (having changed nothing) when a texel read is among the bytes the group writes.
template <unsigned F>
bool Gs::pixels8(const Draw& d, const Rows& rows, int x0, int n, const Lanes& l, const Vertex& flat) {
  const V lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  V alive = _mm256_cmpgt_epi32(_mm256_set1_epi32(n), lane);
  uint32_t* words = mem.words();
  uint16_t* halves = reinterpret_cast<uint16_t*>(words);
  V r, g, b, a;
  if (d.iip) {
    auto colour = [](const float* p) { return clamp255(_mm256_cvttps_epi32(_mm256_add_ps(_mm256_load_ps(p), _mm256_set1_ps(0.5f)))); };
    r = colour(l.r), g = colour(l.g), b = colour(l.b), a = colour(l.a);
  } else {
    r = _mm256_set1_epi32(flat.r), g = _mm256_set1_epi32(flat.g), b = _mm256_set1_epi32(flat.b), a = _mm256_set1_epi32(flat.a);
  }

  const bool z16 = d.zpsm == PSMZ16 || d.zpsm == PSMZ16S;
  V z = _mm256_setzero_si256();
  alignas(32) uint32_t zi[8] = {};
  if constexpr (F & kPlotZtest) {
    z = _mm256_min_epu32(depthLanes(l.z), _mm256_set1_epi32(int(d.zmax)));
    _mm256_store_si256(reinterpret_cast<V*>(zi), rowAt(rows.z, x0));
    if (d.ztst == 0) return true;
    if (d.ztst >= 2) {
      const V zix = _mm256_load_si256(reinterpret_cast<const V*>(zi));
      V zdst = z16 ? _mm256_and_si256(_mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int*>(halves), zix, alive, 2),
                                      _mm256_set1_epi32(0xFFFF))
                   : _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int*>(words), zix, alive, 4);
      if (d.zpsm == PSMZ24) zdst = _mm256_and_si256(zdst, _mm256_set1_epi32(0xFFFFFF));
      const V ge = geU(z, zdst);
      alive = _mm256_and_si256(alive, d.ztst == 2 ? ge : _mm256_andnot_si256(_mm256_cmpeq_epi32(z, zdst), ge));
      if (!laneMask(alive)) return true;
    }
  }

  if constexpr (F & kPlotTme) {
    alignas(32) int tu[8], tv[8];
    if constexpr (F & kPlotFst) {
      _mm256_store_si256(reinterpret_cast<V*>(tu), _mm256_srai_epi32(_mm256_cvttps_epi32(_mm256_load_ps(l.u)), 4));
      _mm256_store_si256(reinterpret_cast<V*>(tv), _mm256_srai_epi32(_mm256_cvttps_epi32(_mm256_load_ps(l.v)), 4));
    } else {
      const __m256 q = _mm256_load_ps(l.q), one = _mm256_set1_ps(1.0f);
      const __m256 inv = _mm256_blendv_ps(one, _mm256_div_ps(one, q), _mm256_cmp_ps(q, _mm256_setzero_ps(), _CMP_NEQ_UQ));
      auto floorLanes = [](__m256 f) {  // floorToInt
        const V i = _mm256_cvttps_epi32(f);
        return _mm256_add_epi32(i, _mm256_castps_si256(_mm256_cmp_ps(_mm256_cvtepi32_ps(i), f, _CMP_GT_OQ)));
      };
      _mm256_store_si256(reinterpret_cast<V*>(tu),
                         floorLanes(_mm256_mul_ps(_mm256_mul_ps(_mm256_load_ps(l.s), inv), _mm256_set1_ps(float(d.tw)))));
      _mm256_store_si256(reinterpret_cast<V*>(tv),
                         floorLanes(_mm256_mul_ps(_mm256_mul_ps(_mm256_load_ps(l.t), inv), _mm256_set1_ps(float(d.th)))));
    }
    alignas(32) uint32_t tc[8] = {}, tbytes[8];
    if (fetchTexels8(d, tu, tv, tc, d.texFeedback ? tbytes : nullptr)) {
      if (d.texFeedback) {  // a texel among the bytes the group's frame and depth buffer writes cover
        alignas(32) uint32_t fw[8], zw[8];
        _mm256_store_si256(reinterpret_cast<V*>(fw), rowAt(rows.fb, x0));
        _mm256_store_si256(reinterpret_cast<V*>(zw), rowAt(rows.z, x0));
        const bool z16 = d.zpsm == PSMZ16 || d.zpsm == PSMZ16S;
        uint32_t lo = ~0u, hi = 0;
        const int live = laneMask(alive);
        for (int m = live; m; m &= m - 1) {
          const int k = std::countr_zero(unsigned(m));
          const uint32_t f0 = d.fb16 ? fw[k] * 2 : fw[k] * 4;
          lo = std::min(lo, f0), hi = std::max(hi, f0 + (d.fb16 ? 2 : 4));
          if (d.ztest) {
            const uint32_t z0 = z16 ? zw[k] * 2 : zw[k] * 4;
            lo = std::min(lo, z0), hi = std::max(hi, z0 + (z16 ? 2 : 4));
          }
        }
        for (int m = live; m; m &= m - 1) {
          const int k = std::countr_zero(unsigned(m));
          if (tbytes[k] + 4 > lo && tbytes[k] < hi) return false;
        }
      }
    } else if (d.texFeedback) {
      return false;
    } else {
      for (int m = laneMask(alive); m; m &= m - 1) {
        const int k = std::countr_zero(unsigned(m));
        tc[k] = fetchTexel(d, tu[k], tv[k]);
      }
    }
    const V c = _mm256_load_si256(reinterpret_cast<const V*>(tc)), byte = _mm256_set1_epi32(0xFF), max = _mm256_set1_epi32(255);
    const V tr = _mm256_and_si256(c, byte), tg = _mm256_and_si256(_mm256_srli_epi32(c, 8), byte);
    const V tb = _mm256_and_si256(_mm256_srli_epi32(c, 16), byte), ta = _mm256_srli_epi32(c, 24);
    auto mod = [&](V t, V v) { return _mm256_srai_epi32(_mm256_mullo_epi32(t, v), 7); };
    switch (d.tfx) {
      case 0:  // MODULATE
        r = _mm256_min_epi32(max, mod(tr, r)), g = _mm256_min_epi32(max, mod(tg, g)), b = _mm256_min_epi32(max, mod(tb, b));
        if (d.tcc) a = _mm256_min_epi32(max, mod(ta, a));
        break;
      case 1:  // DECAL
        r = tr, g = tg, b = tb;
        if (d.tcc) a = ta;
        break;
      default:  // HIGHLIGHT, HIGHLIGHT2
        r = _mm256_min_epi32(max, _mm256_add_epi32(mod(tr, r), a)), g = _mm256_min_epi32(max, _mm256_add_epi32(mod(tg, g), a));
        b = _mm256_min_epi32(max, _mm256_add_epi32(mod(tb, b), a));
        if (d.tcc) a = d.tfx == 2 ? _mm256_min_epi32(max, _mm256_add_epi32(ta, a)) : ta;
        break;
    }
  }
  if constexpr (F & kPlotFge) {
    const V f = _mm256_cvttps_epi32(_mm256_min_ps(_mm256_max_ps(_mm256_load_ps(l.f), _mm256_setzero_ps()), _mm256_set1_ps(255.0f)));
    const V nf = _mm256_sub_epi32(_mm256_set1_epi32(255), f);
    auto fog = [&](V c, int col) { return _mm256_srai_epi32(_mm256_add_epi32(_mm256_mullo_epi32(f, c), _mm256_mullo_epi32(nf, _mm256_set1_epi32(col))), 8); };
    r = fog(r, d.fogR), g = fog(g, d.fogG), b = fog(b, d.fogB);
  }

  V writeFb = ones(), writeZ = d.zwrite ? ones() : _mm256_setzero_si256(), writeAlpha = ones();
  if constexpr (F & kPlotAte) {
    const V ref = _mm256_set1_epi32(d.aref);
    V pass;
    switch (d.atst) {
      case 0: pass = _mm256_setzero_si256(); break;
      case 1: pass = ones(); break;
      case 2: pass = _mm256_cmpgt_epi32(ref, a); break;
      case 3: pass = _mm256_andnot_si256(_mm256_cmpgt_epi32(a, ref), ones()); break;
      case 4: pass = _mm256_cmpeq_epi32(a, ref); break;
      case 5: pass = _mm256_andnot_si256(_mm256_cmpgt_epi32(ref, a), ones()); break;
      case 6: pass = _mm256_cmpgt_epi32(a, ref); break;
      default: pass = _mm256_andnot_si256(_mm256_cmpeq_epi32(a, ref), ones()); break;
    }
    switch (d.afail) {
      case 0: alive = _mm256_and_si256(alive, pass); break;
      case 1: writeZ = _mm256_and_si256(writeZ, pass); break;
      case 2: writeFb = _mm256_and_si256(writeFb, pass); break;
      default: writeZ = _mm256_and_si256(writeZ, pass), writeAlpha = _mm256_and_si256(writeAlpha, pass); break;
    }
    if (!laneMask(alive)) return true;
  }

  constexpr bool fb16 = F & kPlotFb16;
  alignas(32) uint32_t fi[8];
  const V fix = rowAt(rows.fb, x0);
  _mm256_store_si256(reinterpret_cast<V*>(fi), fix);
  const V dst = fb16 ? _mm256_and_si256(_mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int*>(halves), fix, alive, 2),
                                        _mm256_set1_epi32(0xFFFF))
                     : _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int*>(words), fix, alive, 4);
  const V dst32 = fb16 ? from16v(dst) : dst;
  if (d.date) alive = _mm256_and_si256(alive, _mm256_cmpeq_epi32(_mm256_srli_epi32(dst32, 31), _mm256_set1_epi32(d.datm)));

  const V byte = _mm256_set1_epi32(0xFF);
  if constexpr (F & kPlotBlend) {
    const V on = d.pabe ? _mm256_cmpeq_epi32(_mm256_and_si256(a, _mm256_set1_epi32(0x80)), _mm256_set1_epi32(0x80)) : ones();
    const V ad = d.fb24 ? _mm256_set1_epi32(0x80) : _mm256_srli_epi32(dst32, 24);
    const V cc = d.bc == 0 ? a : d.bc == 1 ? ad : _mm256_set1_epi32(d.fix);
    auto blend = [&](V cs, V cd) {
      auto pick = [&](int s) { return s == 0 ? cs : s == 1 ? cd : _mm256_setzero_si256(); };
      return _mm256_add_epi32(_mm256_srai_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(pick(d.ba), pick(d.bb)), cc), 7), pick(d.bd));
    };
    r = _mm256_blendv_epi8(r, blend(r, _mm256_and_si256(dst32, byte)), on);
    g = _mm256_blendv_epi8(g, blend(g, _mm256_and_si256(_mm256_srli_epi32(dst32, 8), byte)), on);
    b = _mm256_blendv_epi8(b, blend(b, _mm256_and_si256(_mm256_srli_epi32(dst32, 16), byte)), on);
  }
  if (d.colclamp) {
    r = clamp255(r), g = clamp255(g), b = clamp255(b);
  } else {
    r = _mm256_and_si256(r, byte), g = _mm256_and_si256(g, byte), b = _mm256_and_si256(b, byte);
  }
  if (d.fba) a = _mm256_or_si256(a, _mm256_set1_epi32(0x80));
  V out = _mm256_or_si256(_mm256_or_si256(r, _mm256_slli_epi32(g, 8)),
                          _mm256_or_si256(_mm256_slli_epi32(b, 16), _mm256_slli_epi32(_mm256_and_si256(a, byte), 24)));
  const V rgb = _mm256_set1_epi32(0xFFFFFF);
  out = _mm256_blendv_epi8(_mm256_or_si256(_mm256_and_si256(out, rgb), _mm256_andnot_si256(rgb, dst32)), out, writeAlpha);
  alignas(32) uint32_t val[8];
  if constexpr (fb16) {
    const V m = _mm256_set1_epi32(int(to16(d.fbmsk) | ((d.fbmsk >> 31) << 15)));
    _mm256_store_si256(reinterpret_cast<V*>(val), _mm256_or_si256(_mm256_andnot_si256(m, to16v(out)), _mm256_and_si256(dst, m)));
  } else {
    const V m = _mm256_set1_epi32(int(d.fb24 ? d.fbmsk | 0xFF000000 : d.fbmsk));
    _mm256_store_si256(reinterpret_cast<V*>(val), _mm256_or_si256(_mm256_andnot_si256(m, out), _mm256_and_si256(dst, m)));
  }
  const int fbLanes = laneMask(_mm256_and_si256(alive, writeFb));
  for (int m = fbLanes; m; m &= m - 1) {
    const int k = std::countr_zero(unsigned(m));
    if constexpr (fb16) halves[fi[k]] = uint16_t(val[k]);
    else words[fi[k]] = val[k];
  }
  tPixels += uint64_t(std::popcount(unsigned(fbLanes)));
  if constexpr (F & kPlotZtest) {
    alignas(32) uint32_t zv[8];
    _mm256_store_si256(reinterpret_cast<V*>(zv), z);
    for (int m = laneMask(_mm256_and_si256(alive, writeZ)); m; m &= m - 1) {
      const int k = std::countr_zero(unsigned(m));
      if (z16) halves[zi[k]] = uint16_t(zv[k]);
      else if (d.zpsm == PSMZ24) words[zi[k]] = (words[zi[k]] & 0xFF000000) | (zv[k] & 0xFFFFFF);
      else words[zi[k]] = zv[k];
    }
  }
  return true;
}

// plotT for a run of pixels of one colour and depth (Draw::fill): the tests and the pixel value
// are worked out once, then stored along the row.
void Gs::fillSpan(const Draw& d, const Rows& rows, int xl, int xr, uint32_t z, int r, int g, int b, int a) {
  bool writeFb = true, writeZ = d.zwrite, writeAlpha = true;
  if (d.ztest) z = std::min(z, d.zmax);
  if (d.ate) {
    bool pass;
    switch (d.atst) {
      case 0: pass = false; break;
      case 1: pass = true; break;
      case 2: pass = a < d.aref; break;
      case 3: pass = a <= d.aref; break;
      case 4: pass = a == d.aref; break;
      case 5: pass = a >= d.aref; break;
      case 6: pass = a > d.aref; break;
      default: pass = a != d.aref; break;
    }
    if (!pass) {
      switch (d.afail) {
        case 0: return;
        case 1: writeZ = false; break;
        case 2: writeFb = false; break;
        default: writeZ = false, writeAlpha = false; break;
      }
    }
  }
  if (d.colclamp) {
    r = std::clamp(r, 0, 255), g = std::clamp(g, 0, 255), b = std::clamp(b, 0, 255);
  } else {
    r &= 0xFF, g &= 0xFF, b &= 0xFF;
  }
  if (d.fba) a |= 0x80;
  const uint32_t out = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a & 0xFF) << 24);
  uint32_t* words = mem.words();
  uint16_t* halves = reinterpret_cast<uint16_t*>(words);
  if (writeFb) {  // bits the mask keeps (and the alpha when it is not written) come from memory
    if (d.fb16) {
      const uint32_t m = to16(d.fbmsk) | ((d.fbmsk >> 31) << 15) | (writeAlpha ? 0 : 0x8000);
      const uint16_t v = uint16_t(to16(out) & ~m);
      for (int x = xl; x <= xr; ++x) {
        uint16_t& p = halves[rows.fb.at(uint32_t(x))];
        p = uint16_t(v | (p & m));
      }
    } else {
      const uint32_t m = d.fbmsk | (d.fb24 || !writeAlpha ? 0xFF000000u : 0), v = out & ~m;
      if (m == 0) {
        for (int x = xl; x <= xr; ++x) words[rows.fb.at(uint32_t(x))] = v;
      } else {
        for (int x = xl; x <= xr; ++x) {
          uint32_t& p = words[rows.fb.at(uint32_t(x))];
          p = v | (p & m);
        }
      }
    }
    tPixels += uint64_t(xr - xl + 1);
  }
  if (writeZ) {
    if (d.zpsm == PSMZ16 || d.zpsm == PSMZ16S) {
      for (int x = xl; x <= xr; ++x) halves[rows.z.at(uint32_t(x))] = uint16_t(z);
    } else if (d.zpsm == PSMZ24) {
      for (int x = xl; x <= xr; ++x) {
        uint32_t& p = words[rows.z.at(uint32_t(x))];
        p = (p & 0xFF000000) | (z & 0xFFFFFF);
      }
    } else {
      for (int x = xl; x <= xr; ++x) words[rows.z.at(uint32_t(x))] = z;
    }
  }
}

bool Gs::fetchTexels8(const Draw& d, const int* u, const int* v, uint32_t* out, uint32_t* bytes) const {
  using namespace gsmem_detail;
  auto wrap = [](V c, int size, uint32_t mode, uint32_t mn, uint32_t mx) {
    switch (mode) {
      case 0: return _mm256_and_si256(c, _mm256_set1_epi32(size - 1));
      case 1: return _mm256_min_epi32(_mm256_max_epi32(c, _mm256_setzero_si256()), _mm256_set1_epi32(size - 1));
      case 2: return _mm256_min_epi32(_mm256_max_epi32(c, _mm256_set1_epi32(int(mn))), _mm256_set1_epi32(int(mx)));
      default: return _mm256_or_si256(_mm256_and_si256(c, _mm256_set1_epi32(int(mn))), _mm256_set1_epi32(int(mx)));
    }
  };
  const V x = wrap(_mm256_load_si256(reinterpret_cast<const V*>(u)), d.tw, d.wms, d.minu, d.maxu);
  const V y = wrap(_mm256_load_si256(reinterpret_cast<const V*>(v)), d.th, d.wmt, d.minv, d.maxv);
  const int* words = reinterpret_cast<const int*>(mem.words());
  auto table = [](const uint16_t* t, V i) {  // 16-bit table entries
    return _mm256_and_si256(_mm256_i32gather_epi32(reinterpret_cast<const int*>(t), i, 2), _mm256_set1_epi32(0xFFFF));
  };
  auto bp = _mm256_set1_epi32(int(d.tbp));
  // Word index of a 32-bit format texel (addr32).
  auto addr32 = [&] {
    const V page = _mm256_add_epi32(_mm256_srli_epi32(x, 6), _mm256_mullo_epi32(_mm256_srli_epi32(y, 5), _mm256_set1_epi32(int(d.tbw))));
    const V t = table(kSwizzle.t32, _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(y, _mm256_set1_epi32(31)), 6),
                                                    _mm256_and_si256(x, _mm256_set1_epi32(63))));
    return _mm256_and_si256(_mm256_add_epi32(_mm256_slli_epi32(_mm256_add_epi32(bp, _mm256_slli_epi32(page, 5)), 6), t),
                            _mm256_set1_epi32(int(kMemWords - 1)));
  };
  const int* clut = reinterpret_cast<const int*>(d.clut);
  const V byte = _mm256_set1_epi32(0xFF);
  V c, ba;  // the texels, and the byte addresses they come from
  switch (d.tpsm) {
    case PSMCT32: c = _mm256_i32gather_epi32(words, ba = addr32(), 4), ba = _mm256_slli_epi32(ba, 2); break;
    case PSMCT24: {
      ba = addr32();
      const V raw = _mm256_and_si256(_mm256_i32gather_epi32(words, ba, 4), _mm256_set1_epi32(0xFFFFFF));
      ba = _mm256_slli_epi32(ba, 2);
      V a = _mm256_set1_epi32(int(d.ta0 << 24));
      if (d.aem) a = _mm256_andnot_si256(_mm256_cmpeq_epi32(raw, _mm256_setzero_si256()), a);
      c = _mm256_or_si256(raw, a);
      break;
    }
    case PSMT8H:
      ba = addr32();
      c = _mm256_i32gather_epi32(clut, _mm256_srli_epi32(_mm256_i32gather_epi32(words, ba, 4), 24), 4);
      ba = _mm256_slli_epi32(ba, 2);
      break;
    case PSMT4HL: case PSMT4HH: {
      ba = addr32();
      const V w = _mm256_i32gather_epi32(words, ba, 4);
      ba = _mm256_slli_epi32(ba, 2);
      const V n = d.tpsm == PSMT4HL ? _mm256_and_si256(_mm256_srli_epi32(w, 24), _mm256_set1_epi32(0xF)) : _mm256_srli_epi32(w, 28);
      c = _mm256_i32gather_epi32(clut, _mm256_and_si256(_mm256_add_epi32(n, _mm256_set1_epi32(int(d.csa))), byte), 4);
      break;
    }
    case PSMT8: {
      const V page = _mm256_add_epi32(_mm256_srli_epi32(x, 7), _mm256_mullo_epi32(_mm256_srli_epi32(y, 6), _mm256_set1_epi32(int(d.tbw >> 1))));
      const V t = table(kSwizzle.t8, _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(y, _mm256_set1_epi32(63)), 7),
                                                     _mm256_and_si256(x, _mm256_set1_epi32(127))));
      const V i = _mm256_and_si256(_mm256_add_epi32(_mm256_slli_epi32(_mm256_add_epi32(bp, _mm256_slli_epi32(page, 5)), 8), t),
                                   _mm256_set1_epi32(int(kMemWords * 4 - 1)));
      c = _mm256_i32gather_epi32(clut, _mm256_and_si256(_mm256_i32gather_epi32(words, i, 1), byte), 4);
      ba = i;
      break;
    }
    case PSMT4: {
      const V page = _mm256_add_epi32(_mm256_srli_epi32(x, 7), _mm256_mullo_epi32(_mm256_srli_epi32(y, 7), _mm256_set1_epi32(int(d.tbw >> 1))));
      const V t = table(kSwizzle.t4, _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(y, _mm256_set1_epi32(127)), 7),
                                                     _mm256_and_si256(x, _mm256_set1_epi32(127))));
      const V n = _mm256_and_si256(_mm256_add_epi32(_mm256_slli_epi32(_mm256_add_epi32(bp, _mm256_slli_epi32(page, 5)), 9), t),
                                   _mm256_set1_epi32(int(kMemWords * 8 - 1)));
      ba = _mm256_srli_epi32(n, 1);
      const V b = _mm256_i32gather_epi32(words, ba, 1);
      const V nib = _mm256_and_si256(_mm256_srlv_epi32(b, _mm256_slli_epi32(_mm256_and_si256(n, _mm256_set1_epi32(1)), 2)),
                                     _mm256_set1_epi32(0xF));
      c = _mm256_i32gather_epi32(clut, _mm256_and_si256(_mm256_add_epi32(nib, _mm256_set1_epi32(int(d.csa))), byte), 4);
      break;
    }
    default: return false;
  }
  _mm256_store_si256(reinterpret_cast<V*>(out), c);
  if (bytes) _mm256_store_si256(reinterpret_cast<V*>(bytes), ba);
  return true;
}

// Texel at (u, v) in texel units as RGBA (alpha expanded per TEXA / the CLUT).
uint32_t Gs::fetchTexel(const Draw& d, int u, int v) const {
  auto wrap = [](int c, int size, uint32_t mode, uint32_t mn, uint32_t mx) {
    switch (mode) {
      case 0: return c & (size - 1);
      case 1: return std::clamp(c, 0, size - 1);
      case 2: return std::clamp(c, int(mn), int(mx));
      default: return int((uint32_t(c) & mn) | mx);
    }
  };
  const uint32_t x = uint32_t(wrap(u, d.tw, d.wms, d.minu, d.maxu));
  const uint32_t y = uint32_t(wrap(v, d.th, d.wmt, d.minv, d.maxv));
  const uint32_t* words = mem.words();
  switch (d.tpsm) {
    case PSMCT32: return words[GsMemory::addr32(d.tbp, d.tbw, x, y)];
    case PSMCT24: {
      const uint32_t raw = words[GsMemory::addr32(d.tbp, d.tbw, x, y)] & 0xFFFFFF;
      return raw | ((d.aem && raw == 0) ? 0 : d.ta0 << 24);
    }
    case PSMCT16: case PSMCT16S: {
      const uint32_t raw = reinterpret_cast<const uint16_t*>(words)[GsMemory::addr16(d.tbp, d.tbw, x, y, d.tpsm)];
      const uint32_t a = (raw & 0x8000) ? d.ta1 : ((d.aem && (raw & 0x7FFF) == 0) ? 0 : d.ta0);
      return (from16(raw) & 0xFFFFFF) | (a << 24);
    }
    case PSMT8: return d.clut[reinterpret_cast<const uint8_t*>(words)[GsMemory::addr8(d.tbp, d.tbw, x, y)]];
    case PSMT4: {
      const uint32_t n = GsMemory::addr4(d.tbp, d.tbw, x, y);
      return d.clut[(d.csa + ((reinterpret_cast<const uint8_t*>(words)[n >> 1] >> ((n & 1) * 4)) & 0xF)) & 0xFF];
    }
    case PSMT8H: return d.clut[words[GsMemory::addr32(d.tbp, d.tbw, x, y)] >> 24];
    case PSMT4HL: return d.clut[(d.csa + ((words[GsMemory::addr32(d.tbp, d.tbw, x, y)] >> 24) & 0xF)) & 0xFF];
    case PSMT4HH: return d.clut[(d.csa + (words[GsMemory::addr32(d.tbp, d.tbw, x, y)] >> 28)) & 0xFF];
  }
  return 0;
}

// The per-pixel pipeline, compiled once per combination of the features that branch the most
// (the PlotFeature bits), so each primitive runs a version without their tests.
Gs::Rows Gs::rowsOf(const Draw& d, int y) {
  const bool z16 = d.zpsm == PSMZ16 || d.zpsm == PSMZ16S;
  return {d.fb16 ? GsMemory::row16(d.fbp, d.fbw, uint32_t(y), d.fpsm) : GsMemory::row32(d.fbp, d.fbw, uint32_t(y)),
          z16 ? GsMemory::row16(d.zbp, d.fbw, uint32_t(y), d.zpsm) : GsMemory::row32(d.zbp, d.fbw, uint32_t(y), true)};
}

void Gs::plotAt(const Draw& d, int x, int y, uint32_t z, int r, int g, int b, int a, float s, float t, float q, int u,
                int v, uint8_t f) {
  if (x < d.sx0 || x > d.sx1 || y < d.sy0 || y > d.sy1) return;
  (this->*d.plot)(d, rowsOf(d, y), x, z, r, g, b, a, s, t, q, u, v, f);
}

// The pixel pipeline at (x, rows): texture, fog, alpha test, destination alpha test, depth test,
// blending, masks. The depth test goes first: a pixel that fails it writes nothing whatever the
// other tests say, so its texel need not be fetched.
template <unsigned F>
__forceinline void Gs::plotT(const Draw& d, const Rows& rows, int x, uint32_t z, int r, int g, int b, int a, float s,
                             float t, float q, int u, int v, uint8_t f) {
  uint32_t* words = mem.words();
  uint16_t* halves = reinterpret_cast<uint16_t*>(words);
  const bool z16 = d.zpsm == PSMZ16 || d.zpsm == PSMZ16S;
  uint32_t zIndex = 0;
  if constexpr (F & kPlotZtest) {
    z = std::min(z, d.zmax);
    zIndex = rows.z.at(uint32_t(x));
    auto zdst = [&] { return z16 ? uint32_t(halves[zIndex]) : d.zpsm == PSMZ24 ? words[zIndex] & 0xFFFFFF : words[zIndex]; };
    switch (d.ztst) {
      case 0: return;
      case 1: break;
      case 2: if (z < zdst()) return; break;
      default: if (z <= zdst()) return; break;
    }
  }

  if constexpr (F & kPlotTme) {
    int tu, tv;
    if constexpr (F & kPlotFst) {
      tu = u >> 4;
      tv = v >> 4;
    } else {
      const float inv = q != 0.0f ? 1.0f / q : 1.0f;
      tu = floorToInt(s * inv * float(d.tw));
      tv = floorToInt(t * inv * float(d.th));
    }
    const uint32_t tc = fetchTexel(d, tu, tv);
    const int tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF, ta = int(tc >> 24);
    switch (d.tfx) {
      case 0:  // MODULATE
        r = std::min(255, (tr * r) >> 7), g = std::min(255, (tg * g) >> 7), b = std::min(255, (tb * b) >> 7);
        if (d.tcc) a = std::min(255, (ta * a) >> 7);
        break;
      case 1:  // DECAL
        r = tr, g = tg, b = tb;
        if (d.tcc) a = ta;
        break;
      case 2:  // HIGHLIGHT
        r = std::min(255, ((tr * r) >> 7) + a), g = std::min(255, ((tg * g) >> 7) + a), b = std::min(255, ((tb * b) >> 7) + a);
        if (d.tcc) a = std::min(255, ta + a);
        break;
      default:  // HIGHLIGHT2
        r = std::min(255, ((tr * r) >> 7) + a), g = std::min(255, ((tg * g) >> 7) + a), b = std::min(255, ((tb * b) >> 7) + a);
        if (d.tcc) a = ta;
        break;
    }
  }
  if constexpr (F & kPlotFge) {
    r = (f * r + (255 - f) * d.fogR) >> 8;
    g = (f * g + (255 - f) * d.fogG) >> 8;
    b = (f * b + (255 - f) * d.fogB) >> 8;
  }

  bool writeFb = true, writeZ = d.zwrite, writeAlpha = true;
  if constexpr (F & kPlotAte) {
    bool pass;
    switch (d.atst) {
      case 0: pass = false; break;
      case 1: pass = true; break;
      case 2: pass = a < d.aref; break;
      case 3: pass = a <= d.aref; break;
      case 4: pass = a == d.aref; break;
      case 5: pass = a >= d.aref; break;
      case 6: pass = a > d.aref; break;
      default: pass = a != d.aref; break;
    }
    if (!pass) {
      switch (d.afail) {
        case 0: return;
        case 1: writeZ = false; break;
        case 2: writeFb = false; break;
        default: writeZ = false, writeAlpha = false; break;
      }
    }
  }

  constexpr bool fb16 = F & kPlotFb16;
  const uint32_t fbIndex = rows.fb.at(uint32_t(x));
  const uint32_t dst = fb16 ? halves[fbIndex] : words[fbIndex];
  const uint32_t dst32 = fb16 ? from16(dst) : dst;
  if (d.date && bool(dst32 >> 31) != d.datm) return;

  if ((F & kPlotBlend) && (!d.pabe || (a & 0x80))) {
    const int ad = d.fb24 ? 0x80 : int(dst32 >> 24);
    const int cc = d.bc == 0 ? a : d.bc == 1 ? ad : d.fix;
    const int cs[3] = {r, g, b};
    const int cd[3] = {int(dst32 & 0xFF), int((dst32 >> 8) & 0xFF), int((dst32 >> 16) & 0xFF)};
    int out[3];
    for (int i = 0; i < 3; ++i) {
      const int A = d.ba == 0 ? cs[i] : d.ba == 1 ? cd[i] : 0;
      const int B = d.bb == 0 ? cs[i] : d.bb == 1 ? cd[i] : 0;
      const int D = d.bd == 0 ? cs[i] : d.bd == 1 ? cd[i] : 0;
      out[i] = (((A - B) * cc) >> 7) + D;
    }
    r = out[0], g = out[1], b = out[2];
  }
  if (d.colclamp) {
    r = std::clamp(r, 0, 255), g = std::clamp(g, 0, 255), b = std::clamp(b, 0, 255);
  } else {
    r &= 0xFF, g &= 0xFF, b &= 0xFF;
  }
  if (d.fba) a |= 0x80;

  if (writeFb) {
    uint32_t out = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a & 0xFF) << 24);
    if (!writeAlpha) out = (out & 0xFFFFFF) | (dst32 & 0xFF000000);
    if constexpr (fb16) {
      const uint32_t m = to16(d.fbmsk) | ((d.fbmsk >> 31) << 15);
      halves[fbIndex] = uint16_t((to16(out) & ~m) | (dst & m));
    } else {
      const uint32_t m = d.fb24 ? d.fbmsk | 0xFF000000 : d.fbmsk;
      words[fbIndex] = (out & ~m) | (dst & m);
    }
    ++tPixels;
  }
  if (writeZ) {
    if (z16) halves[zIndex] = uint16_t(z);
    else if (d.zpsm == PSMZ24) words[zIndex] = (words[zIndex] & 0xFF000000) | (z & 0xFFFFFF);
    else words[zIndex] = z;
  }
}

// The versions of plotT and spanT for every feature combination, indexed by the PlotFeature bits.
const std::array<Gs::PlotFn, 128> Gs::kPlotFns = []<size_t... I>(std::index_sequence<I...>) {
  return std::array<Gs::PlotFn, 128>{&Gs::plotT<unsigned(I)>...};
}(std::make_index_sequence<128>{});
const std::array<Gs::SpanFn, 128> Gs::kSpanFns = []<size_t... I>(std::index_sequence<I...>) {
  return std::array<Gs::SpanFn, 128>{&Gs::spanT<unsigned(I)>...};
}(std::make_index_sequence<128>{});
