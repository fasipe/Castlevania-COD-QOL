#include "iop/iopkernel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "ee/cpu.h"
#include "iop/sif.h"
#include "iop/spu2.h"
#include "iso9660.h"

namespace {
constexpr uint32_t kImportMagic = 0x41E00000, kExportMagic = 0x41C00000;
constexpr uint32_t kJrRa = 0x03E00008;
constexpr double kEeCyclesPerUsec = 294.912;
constexpr uint64_t kEeCyclesPerSysclock = 8;  // the IOP clock is 36.864 MHz
constexpr uint64_t kEeCyclesPerSample = 6144;  // 48 kHz
constexpr int kLoaderPrio = 8;
constexpr uint64_t kMinSlice = 4096;  // EE cycles between IOP time slices
// IOP kernel error codes
constexpr uint32_t kErrSemaZero = uint32_t(-419), kErrSemaOverflow = uint32_t(-420), kErrWaitDelete = uint32_t(-425),
                   kErrUnknownThread = uint32_t(-407), kErrUnknownSema = uint32_t(-408);

inline uint32_t sx16(uint32_t v) { return uint32_t(int32_t(int16_t(v))); }
}  // namespace

IopKernel::IopKernel(Sif& sif, std::vector<uint8_t>& ram, Spu2& spu) : cpu(ram), sif_(sif), spu_(spu) {
  cpu.kernel = this;
  spu_.iopRam = &ram;
  // Return addresses for code the kernel starts itself: thread entry points, interrupt-context
  // handlers and RPC server functions.
  exitStub_ = stub("thread exit", [this] {
    threads_[current_].wait = Wait::Dormant;
    reschedule();
  });
  trampoline_ = stub("callback return", [this] {
    callbackDone_ = true;
    cpu.yield = true;
  });
  rpcReturn_ = stub("rpc return", [this] {
    Thread& th = threads_[current_];
    const uint32_t reply = cpu.s.gpr[2];
    auto done = std::move(th.rpcDone);
    th.rpcDone = nullptr;
    cpu.s = th.rpcLoopCtx;
    th.wait = Wait::RpcLoop;
    reschedule();
    if (done) done(reply);
    startRpcs();
  });
  callbackStack_ = sif_.iopAlloc(0x4000) + 0x4000 - 0x40;
  registerHle();
}

uint32_t IopKernel::stub(const std::string& name, std::function<void()> fn) {
  const uint32_t addr = sif_.iopAlloc(16);
  wr32(addr, kJrRa);
  wr32(addr + 4, 0);
  hleFns_.push_back({name, std::move(fn)});
  cpu.setHle(addr, uint32_t(hleFns_.size()));
  return addr;
}

void IopKernel::hle(const std::string& lib, uint32_t index, std::function<void()> fn) {
  hleFns_.push_back({std::format("{}#{}", lib, index), std::move(fn)});
  hleIds_[lib][index] = uint32_t(hleFns_.size());
}

void IopKernel::call(uint32_t id) {
  ++hleCalls;
  ++hleFns_[id - 1].calls;
  hleFns_[id - 1].fn();
}

uint64_t IopKernel::now() const { return sif_.cpu.mem.hw.cycles; }

uint32_t IopKernel::rd32(uint32_t addr) { return cpu.read32(addr); }
void IopKernel::wr32(uint32_t addr, uint32_t v) { cpu.write32(addr, v); }

uint32_t IopKernel::arg(int i) {
  if (i < 4) return cpu.s.gpr[4 + i];
  return rd32(cpu.s.gpr[29] + 16 + 4 * (i - 4));
}

void IopKernel::ret(uint32_t v) { cpu.s.gpr[2] = v; }

std::string IopKernel::readString(uint32_t addr) {
  std::string s;
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint8_t* p = cpu.ptr(addr + i, 1);
    if (!p || !*p) break;
    s += char(*p);
  }
  return s;
}

std::string IopKernel::format(uint32_t fmtAddr, int firstArg) {
  const std::string fmt = readString(fmtAddr);
  std::string out;
  int next = firstArg;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    size_t j = i + 1;
    std::string spec = "%";
    while (j < fmt.size() && std::strchr("-+ #0123456789.lh", fmt[j])) {
      if (fmt[j] != 'l' && fmt[j] != 'h') spec += fmt[j];
      ++j;
    }
    if (j >= fmt.size()) break;
    const char c = fmt[j];
    char buf[256];
    switch (c) {
      case 'd': case 'i': std::snprintf(buf, sizeof buf, (spec + 'd').c_str(), int32_t(arg(next++))); out += buf; break;
      case 'u': case 'x': case 'X': case 'o':
        std::snprintf(buf, sizeof buf, (spec + c).c_str(), arg(next++)); out += buf; break;
      case 'p': out += std::format("0x{:08X}", arg(next++)); break;
      case 'c': out += char(arg(next++)); break;
      case 's': std::snprintf(buf, sizeof buf, (spec + 's').c_str(), readString(arg(next++)).c_str()); out += buf; break;
      case '%': out += '%'; break;
      default: out += spec + c;
    }
    i = j;
  }
  return out;
}

// ---- Module loading -------------------------------------------------------------------------

bool IopKernel::loadModule(const std::vector<uint8_t>& file, const std::string& name, std::string& error) {
  auto u16 = [&](size_t o) { return o + 2 <= file.size() ? uint32_t(file[o] | file[o + 1] << 8) : 0u; };
  auto u32 = [&](size_t o) {
    uint32_t v = 0;
    if (o + 4 <= file.size()) std::memcpy(&v, file.data() + o, 4);
    return v;
  };
  if (u32(0) != 0x464C457F || u16(0x10) != 0xFF80) {
    error = "not an IRX module";
    return false;
  }
  // Program headers: the module information (.iopmod) and the single loadable segment.
  uint32_t loadOffset = 0, fileSize = 0, memSize = 0, entry = 0, gpValue = 0;
  bool haveInfo = false;
  for (uint32_t i = 0; i < u16(0x2C); ++i) {
    const size_t ph = u32(0x1C) + i * u16(0x2A);
    if (u32(ph) == 0x70000080) {
      entry = u32(u32(ph + 4) + 4);
      gpValue = u32(u32(ph + 4) + 8);
      haveInfo = true;
    } else if (u32(ph) == 1) {
      loadOffset = u32(ph + 4), fileSize = u32(ph + 16), memSize = u32(ph + 20);
    }
  }
  if (!haveInfo || !memSize || loadOffset + uint64_t(fileSize) > file.size()) {
    error = "malformed IRX headers";
    return false;
  }
  const uint32_t base = sif_.iopAlloc(memSize);
  uint8_t* image = cpu.ptr(base, memSize);
  if (!image) {
    error = "out of IOP memory";
    return false;
  }
  std::memset(image, 0, memSize);
  std::memcpy(image, file.data() + loadOffset, fileSize);

  // Relocations (all relative to the load address).
  std::vector<uint32_t> pendingHi;
  for (uint32_t i = 0; i < u16(0x30); ++i) {
    const size_t sh = u32(0x20) + i * u16(0x2E);
    if (u32(sh + 4) != 9) continue;  // SHT_REL
    const uint32_t off = u32(sh + 16), size = u32(sh + 20);
    for (uint32_t r = 0; r + 8 <= size; r += 8) {
      const uint32_t where = u32(off + r), type = u32(off + r + 4) & 0xFF;
      if (where + 4 > memSize) continue;
      uint32_t w;
      std::memcpy(&w, image + where, 4);
      switch (type) {
        case 0: break;
        case 2: w += base; break;  // R_MIPS_32
        case 4: w = (w & 0xFC000000) | ((((w & 0x03FFFFFF) << 2) + base) >> 2 & 0x03FFFFFF); break;
        case 5: pendingHi.push_back(where); break;  // R_MIPS_HI16, completed by the next LO16
        case 6: {                                   // R_MIPS_LO16
          const uint32_t lo = sx16(w);
          for (uint32_t hiAt : pendingHi) {
            uint32_t hw;
            std::memcpy(&hw, image + hiAt, 4);
            const uint32_t full = (hw << 16) + lo + base;
            hw = (hw & 0xFFFF0000) | ((full + 0x8000) >> 16 & 0xFFFF);
            std::memcpy(image + hiAt, &hw, 4);
          }
          pendingHi.clear();
          w = (w & 0xFFFF0000) | ((lo + base) & 0xFFFF);
          break;
        }
        default:
          error = std::format("relocation type {} at +0x{:X}", type, where);
          return false;
      }
      std::memcpy(image + where, &w, 4);
    }
  }
  const size_t slash = name.find_last_of("\\/:");
  modules_.push_back({name.substr(slash + 1, name.find(';') - slash - 1), base, memSize});
  linkImports(base, fileSize);
  if (!cpu.fault.empty()) {
    error = cpu.fault;
    return false;
  }

  // start(argc, argv) runs in a thread of its own, like the module loader's.
  const uint32_t argv = sif_.iopAlloc(16 + uint32_t(name.size()) + 1);
  wr32(argv, argv + 16);
  wr32(argv + 4, 0);
  std::memcpy(cpu.ptr(argv + 16, uint32_t(name.size()) + 1), name.c_str(), name.size() + 1);
  const int t = createThread(base + entry, 0x1000, kLoaderPrio, base + gpValue);
  startThread(t, 1, argv);
  const uint64_t before = cpu.executed;
  if (!runUntilIdle(200'000'000, t)) {
    error = cpu.fault.empty() ? std::format("start routine did not finish ({} instructions)", cpu.executed - before) : cpu.fault;
    return false;
  }
  const uint32_t result = threads_[t].ctx.gpr[2];
  threads_[t].used = false;
  sif_.log(std::format("iop: {} loaded at 0x{:06X} (start returned {}, {} instructions)", name, base, result,
                       cpu.executed - before));
  if ((result & 3) == 1) {
    error = "module chose not to stay resident";
    return false;
  }
  return true;
}

// Binds every import stub in the image: kernel libraries to their C++ implementation, libraries
// exported by earlier modules to a jump into their code.
void IopKernel::linkImports(uint32_t base, uint32_t size) {
  for (uint32_t off = 0; off + 28 <= size; off += 4) {
    const uint32_t table = base + off;
    if (rd32(table) != kImportMagic || rd32(table + 4) != 0) continue;
    std::string lib;
    for (uint32_t i = 0; i < 8; ++i) {
      const char c = char(*cpu.ptr(table + 12 + i, 1));
      if (!c) break;
      lib += c;
    }
    for (uint32_t s = table + 20; rd32(s) == kJrRa; s += 8) {
      const uint32_t index = rd32(s + 4) & 0xFFFF;
      if (auto e = exports_.find(lib); e != exports_.end()) {
        const uint32_t target = rd32(e->second + 20 + 4 * index);
        wr32(s, 0x08000000 | ((target >> 2) & 0x03FFFFFF));  // j target
        continue;
      }
      uint32_t id = 0;
      if (auto l = hleIds_.find(lib); l != hleIds_.end())
        if (auto f = l->second.find(index); f != l->second.end()) id = f->second;
      if (!id) {
        const std::string what = std::format("{}#{}", lib, index);
        hleFns_.push_back({what, [this, what] {
                             if (cpu.fault.empty()) cpu.fault = std::format("IOP function {} not implemented (called from {})", what, where(cpu.s.gpr[31] - 8));
                           }});
        id = uint32_t(hleFns_.size());
        hleIds_[lib][index] = id;
      }
      cpu.setHle(s, id);
    }
  }
}

// ---- Threads --------------------------------------------------------------------------------

int IopKernel::createThread(uint32_t entry, uint32_t stackSize, int prio, uint32_t gp) {
  size_t t = 0;
  while (t < threads_.size() && threads_[t].used) ++t;
  if (t == threads_.size()) threads_.emplace_back();
  Thread& th = threads_[t];
  th = Thread{};
  th.used = true;
  th.entry = entry;
  th.stackSize = (stackSize + 15) & ~15u;
  th.stack = sif_.iopAlloc(th.stackSize);
  th.prio = prio;
  th.gp = gp;
  return int(t);
}

void IopKernel::startThread(int t, uint32_t a0, uint32_t a1) {
  Thread& th = threads_[t];
  th.ctx = IopState{};
  th.ctx.pc = th.entry;
  th.ctx.npc = th.entry + 4;
  th.ctx.gpr[4] = a0;
  th.ctx.gpr[5] = a1;
  th.ctx.gpr[28] = th.gp;
  th.ctx.gpr[29] = th.stack + th.stackSize - 16;
  th.ctx.gpr[31] = exitStub_;
  makeReady(t, 0);
}

void IopKernel::block(Wait w) {
  threads_[current_].wait = w;
  reschedule();
}

void IopKernel::makeReady(int t, uint32_t result) {
  Thread& th = threads_[t];
  th.wait = Wait::None;
  th.readySeq = ++readySeq_;
  if (t == current_)
    cpu.s.gpr[2] = result;
  else
    th.ctx.gpr[2] = result;
  reschedule();
}

int IopKernel::pickNext() const {
  int best = -1;
  for (int t = 0; t < int(threads_.size()); ++t) {
    const Thread& th = threads_[t];
    if (!th.used || th.wait != Wait::None) continue;
    if (best < 0 || th.prio < threads_[best].prio) {
      best = t;
    } else if (th.prio == threads_[best].prio && best != current_ &&
               (t == current_ || th.readySeq < threads_[best].readySeq)) {
      best = t;
    }
  }
  return best;
}

void IopKernel::switchTo(int next) {
  if (next == current_) return;
  if (current_ >= 0) threads_[current_].ctx = cpu.s;
  current_ = next;
  if (next >= 0) cpu.s = threads_[next].ctx;
  cpu.yield = true;  // let advance() see the switch (and stop when nothing is ready)
}

void IopKernel::reschedule() {
  if (!inCallback_) switchTo(pickNext());
}

// Runs ready threads until none is left (or thread `untilDone` has exited); false if the budget
// ran out first or the CPU faulted.
bool IopKernel::runUntilIdle(uint64_t budget, int untilDone) {
  uint64_t done = 0;
  while (done < budget && cpu.fault.empty()) {
    if (untilDone >= 0 && threads_[untilDone].wait == Wait::Dormant) return true;
    reschedule();
    if (current_ < 0) {
      if (untilDone < 0) return true;
      // The start routine waits for time to pass: skip ahead to the earliest delay.
      int soonest = -1;
      for (int t = 0; t < int(threads_.size()); ++t)
        if (threads_[t].used && threads_[t].wait == Wait::Delay && (soonest < 0 || threads_[t].wakeAt < threads_[soonest].wakeAt))
          soonest = t;
      if (soonest < 0) return false;
      makeReady(soonest, 0);
      continue;
    }
    done += cpu.run(budget - done);
  }
  return untilDone < 0 && cpu.fault.empty() && current_ < 0;
}

uint32_t IopKernel::callGuest(const Interrupt& irq) {
  const IopState saved = cpu.s;
  cpu.s = IopState{};
  cpu.s.pc = irq.func;
  cpu.s.npc = irq.func + 4;
  cpu.s.gpr[4] = irq.a0;
  cpu.s.gpr[5] = irq.a1;
  cpu.s.gpr[28] = irq.gp;
  cpu.s.gpr[29] = callbackStack_;
  cpu.s.gpr[31] = trampoline_;
  inCallback_ = true;
  callbackDone_ = false;
  uint64_t done = 0;
  while (!callbackDone_ && cpu.fault.empty() && done < 10'000'000) done += cpu.run(10'000'000 - done);
  if (!callbackDone_ && cpu.fault.empty())
    cpu.fault = std::format("IOP interrupt handler 0x{:08X} did not return", irq.func);
  const uint32_t result = cpu.s.gpr[2];
  cpu.s = saved;
  inCallback_ = false;
  reschedule();
  return result;
}

// ---- Time -----------------------------------------------------------------------------------

void IopKernel::checkTimers() {
  const uint64_t t = now();
  for (Timer& tm : timers_) {
    for (int guard = 0; tm.running && t >= tm.nextAt && guard < 64; ++guard) {
      const uint32_t next = callGuest({tm.handler, tm.gp, tm.arg, 0});
      if (next == 0) {
        tm.running = false;
        break;
      }
      tm.compare = next;
      tm.nextAt += uint64_t(next) * tm.prescale * kEeCyclesPerSysclock;
    }
    if (tm.running && tm.nextAt < t) tm.nextAt = t;  // far behind: drop the missed ticks
  }
}

void IopKernel::transferDone(int channel) {
  if (transHandler_[channel]) interrupts_.push_back({transHandler_[channel], transGp_[channel], uint32_t(channel), transArg_[channel]});
}

void IopKernel::onVblank() {
  for (int t = 0; t < int(threads_.size()); ++t)
    if (threads_[t].used && threads_[t].wait == Wait::Vblank) makeReady(t, 0);
}

uint64_t IopKernel::nextEvent() const {
  constexpr uint64_t kNever = ~0ull;
  if (!cpu.fault.empty()) return kNever;
  if (!interrupts_.empty() || pickNext() >= 0) return lastCycles_ + kMinSlice;
  uint64_t next = kNever;
  for (const Thread& th : threads_)
    if (th.used && th.wait == Wait::Delay) next = std::min(next, th.wakeAt);
  for (const Timer& tm : timers_)
    if (tm.running) next = std::min(next, tm.nextAt);
  for (const Spu2::AutoDma& a : spu_.adma)
    if (a.active && a.size >= 8) next = std::min(next, lastCycles_ + (a.size / 8 - a.samples % (a.size / 8)) * kEeCyclesPerSample);
  return next;
}

void IopKernel::advance() {
  const uint64_t t = now();
  if (t < lastCycles_ + kMinSlice) return;
  const uint64_t elapsed = t - lastCycles_;
  lastCycles_ = t;
  if (!cpu.fault.empty()) return;

  spuCycles_ += elapsed;
  spu_.advance(spuCycles_ / kEeCyclesPerSample);
  spuCycles_ %= kEeCyclesPerSample;
  if (spu_.irqPending) {  // a voice reached the IRQ address of a core
    if (spuIrq_.func) interrupts_.push_back({spuIrq_.func, spuIrq_.gp, spuIrqOldStyle_ ? 0 : spu_.irqPending, spuIrq_.a1});
    spu_.irqPending = 0;
  }
  for (int ch = 0; ch < 2; ++ch) {
    const Spu2::AutoDma& a = spu_.adma[ch];
    if (!a.active) continue;
    const uint64_t halves = a.halves();
    for (int guard = 0; admaHalves_[ch] < halves && guard < 4; ++guard, ++admaHalves_[ch]) transferDone(ch);
    admaHalves_[ch] = std::max(admaHalves_[ch], halves);
  }

  checkTimers();
  while (!interrupts_.empty() && cpu.fault.empty()) {
    const Interrupt irq = interrupts_.front();
    interrupts_.pop_front();
    callGuest(irq);
  }
  for (int i = 0; i < int(threads_.size()); ++i)
    if (threads_[i].used && threads_[i].wait == Wait::Delay && t >= threads_[i].wakeAt) makeReady(i, 0);
  startRpcs();

  runUntilIdle(std::min<uint64_t>(elapsed / kEeCyclesPerSysclock + 1000, 2'000'000), -1);
  if (!cpu.fault.empty()) sif_.unimplemented(std::format("{} [IOP pc {}, ra {}]", cpu.fault, where(cpu.s.pc), where(cpu.s.gpr[31])));
}

std::string IopKernel::where(uint32_t addr) const {
  const uint32_t a = addr & 0x1FFFFF;
  for (const Module& m : modules_)
    if (a >= m.base && a < m.base + m.size) return std::format("{}+0x{:X}", m.name, a - m.base);
  return std::format("0x{:08X}", addr);
}

// ---- SIF RPC --------------------------------------------------------------------------------

bool IopKernel::findServer(uint32_t sid, uint32_t& handle, uint32_t& buffer) const {
  for (const Server& s : servers_)
    if (s.sid == sid) {
      handle = s.handle;
      buffer = s.buffer;
      return true;
    }
  return false;
}

bool IopKernel::callServer(uint32_t handle, uint32_t fn, uint32_t size, std::function<void(uint32_t)> done) {
  for (size_t i = 0; i < servers_.size(); ++i)
    if ((servers_[i].handle & 0x1FFFFF) == (handle & 0x1FFFFF)) {
      requests_.push_back({i, fn, size, std::move(done)});
      startRpcs();
      return true;
    }
  return false;
}

// Hands queued calls to their server threads that are idle in sceSifRpcLoop.
void IopKernel::startRpcs() {
  for (auto it = requests_.begin(); it != requests_.end();) {
    const Server& sv = servers_[it->server];
    auto q = queueThread_.find(sv.queue);
    if (q == queueThread_.end()) {
      ++it;
      continue;
    }
    Thread& th = threads_[q->second];
    if (th.wait != Wait::RpcLoop || th.rpcDone) {
      ++it;
      continue;
    }
    th.rpcLoopCtx = th.ctx;
    th.ctx.pc = sv.func;
    th.ctx.npc = sv.func + 4;
    th.ctx.gpr[4] = it->fn;
    th.ctx.gpr[5] = sv.buffer;
    th.ctx.gpr[6] = it->size;
    th.ctx.gpr[31] = rpcReturn_;
    th.ctx.gpr[29] = th.rpcLoopCtx.gpr[29] - 0x40;
    th.rpcDone = std::move(it->done);
    it = requests_.erase(it);
    makeReady(q->second, 0);
  }
}

// ---- Kernel libraries -----------------------------------------------------------------------

void IopKernel::registerHle() {
  // thbase
  hle("thbase", 4, [this] {  // CreateThread(param {attr, option, entry, stackSize, prio})
    const uint32_t p = arg(0);
    ret(uint32_t(createThread(rd32(p + 8), rd32(p + 12), int(rd32(p + 16)), cpu.s.gpr[28]) + 1));
  });
  hle("thbase", 5, [this] {  // DeleteThread
    const uint32_t t = arg(0) - 1;
    if (t >= threads_.size() || !threads_[t].used) return ret(kErrUnknownThread);
    threads_[t].used = false;
    ret(0);
  });
  hle("thbase", 6, [this] {  // StartThread(tid, arg)
    const uint32_t t = arg(0) - 1;
    if (t >= threads_.size() || !threads_[t].used) return ret(kErrUnknownThread);
    ret(0);
    startThread(int(t), arg(1), 0);
  });
  hle("thbase", 8, [this] { block(Wait::Dormant); });  // ExitThread
  hle("thbase", 14, [this] {                             // ChangeThreadPriority(tid, prio)
    const uint32_t t = arg(0) ? arg(0) - 1 : uint32_t(current_);
    if (t >= threads_.size()) return ret(kErrUnknownThread);
    threads_[t].prio = int(arg(1));
    ret(0);
    reschedule();
  });
  hle("thbase", 20, [this] { ret(inCallback_ ? uint32_t(-1) : uint32_t(current_ + 1)); });  // GetThreadId
  hle("thbase", 24, [this] {                                                              // SleepThread
    Thread& th = threads_[current_];
    ret(0);
    if (th.wakeups > 0)
      --th.wakeups;
    else
      block(Wait::Sleep);
  });
  auto wakeup = [this] {  // WakeupThread / iWakeupThread
    const uint32_t t = arg(0) - 1;
    if (t >= threads_.size() || !threads_[t].used) return ret(kErrUnknownThread);
    ret(0);
    if (threads_[t].wait == Wait::Sleep)
      makeReady(int(t), 0);
    else
      ++threads_[t].wakeups;
  };
  hle("thbase", 25, wakeup);
  hle("thbase", 26, wakeup);
  hle("thbase", 33, [this] {  // DelayThread(usec)
    threads_[current_].wakeAt = now() + uint64_t(arg(0) * kEeCyclesPerUsec);
    ret(0);
    block(Wait::Delay);
  });
  hle("thbase", 34, [this] {  // GetSystemTime(SysClock*)
    const uint64_t clk = now() / kEeCyclesPerSysclock;
    wr32(arg(0), uint32_t(clk));
    wr32(arg(0) + 4, uint32_t(clk >> 32));
    ret(0);
  });
  hle("thbase", 39, [this] {  // USec2SysClock(usec, SysClock*)
    const uint64_t clk = uint64_t(arg(0)) * 36864 / 1000;
    wr32(arg(1), uint32_t(clk));
    wr32(arg(1) + 4, uint32_t(clk >> 32));
    ret(0);
  });
  hle("thbase", 40, [this] {  // SysClock2USec(SysClock*, sec*, usec*)
    const uint64_t clk = rd32(arg(0)) | uint64_t(rd32(arg(0) + 4)) << 32;
    const uint64_t us = clk * 1000 / 36864;
    wr32(arg(1), uint32_t(us / 1000000));
    wr32(arg(2), uint32_t(us % 1000000));
    ret(0);
  });

  // thsemap
  hle("thsemap", 4, [this] {  // CreateSema(param {attr, option, initial, max})
    const uint32_t p = arg(0);
    size_t s = 0;
    while (s < semas_.size() && semas_[s].used) ++s;
    if (s == semas_.size()) semas_.emplace_back();
    semas_[s] = Sema{true, int(rd32(p + 8)), int(rd32(p + 12)), {}};
    ret(uint32_t(s + 1));
  });
  hle("thsemap", 5, [this] {  // DeleteSema
    const uint32_t s = arg(0) - 1;
    if (s >= semas_.size() || !semas_[s].used) return ret(kErrUnknownSema);
    semas_[s].used = false;
    ret(0);
    const auto waiters = std::move(semas_[s].waiters);
    for (int t : waiters) makeReady(t, kErrWaitDelete);
  });
  auto signal = [this] {  // SignalSema / iSignalSema
    const uint32_t s = arg(0) - 1;
    if (s >= semas_.size() || !semas_[s].used) return ret(kErrUnknownSema);
    Sema& sm = semas_[s];
    ret(0);
    if (!sm.waiters.empty()) {
      const int t = sm.waiters.front();
      sm.waiters.pop_front();
      makeReady(t, 0);
    } else if (sm.count < sm.max) {
      ++sm.count;
    } else {
      ret(kErrSemaOverflow);
    }
  };
  hle("thsemap", 6, signal);
  hle("thsemap", 7, signal);
  hle("thsemap", 8, [this] {  // WaitSema
    const uint32_t s = arg(0) - 1;
    if (s >= semas_.size() || !semas_[s].used) return ret(kErrUnknownSema);
    ret(0);
    if (semas_[s].count > 0) {
      --semas_[s].count;
    } else {
      semas_[s].waiters.push_back(current_);
      threads_[current_].sema = int(s + 1);
      block(Wait::Sema);
    }
  });
  hle("thsemap", 9, [this] {  // PollSema
    const uint32_t s = arg(0) - 1;
    if (s >= semas_.size() || !semas_[s].used) return ret(kErrUnknownSema);
    if (semas_[s].count > 0) {
      --semas_[s].count;
      ret(0);
    } else {
      ret(kErrSemaZero);
    }
  });

  // intrman
  hle("intrman", 17, [this] {  // CpuSuspendIntr(int* state)
    if (arg(0)) wr32(arg(0), 0);
    ret(0);
  });
  hle("intrman", 18, [this] { ret(0); });                       // CpuResumeIntr
  hle("intrman", 23, [this] { ret(inCallback_ ? 1 : 0); });    // QueryIntrContext

  // sysmem, loadcore
  hle("sysmem", 4, [this] { ret(sif_.iopAlloc(arg(1))); });  // AllocSysMemory(type, size, addr)
  hle("sysmem", 5, [this] { ret(0); });                      // FreeSysMemory
  hle("sysmem", 14, [this] { sif_.log("iop: " + format(arg(0), 1)); ret(0); });  // Kprintf
  hle("loadcore", 4, [this] { ret(0); });  // FlushIcache
  hle("loadcore", 5, [this] { ret(0); });  // FlushDcache
  hle("loadcore", 6, [this] {              // RegisterLibraryEntries(export table)
    const uint32_t table = arg(0);
    std::string name;
    for (uint32_t i = 0; i < 8; ++i) {
      const char c = char(*cpu.ptr(table + 12 + i, 1));
      if (!c) break;
      name += c;
    }
    exports_[name] = table;
    ret(0);
  });
  hle("loadcore", 7, [this] { ret(0); });  // ReleaseLibraryEntries
  hle("stdio", 4, [this] {                  // printf
    sif_.log("iop: " + format(arg(0), 1));
    ret(0);
  });

  // sysclib
  hle("sysclib", 12, [this] {  // memcpy
    if (arg(2)) std::memmove(cpu.ptr(arg(0), arg(2)), cpu.ptr(arg(1), arg(2)), arg(2));
    ret(arg(0));
  });
  hle("sysclib", 13, [this] {  // memmove
    if (arg(2)) std::memmove(cpu.ptr(arg(0), arg(2)), cpu.ptr(arg(1), arg(2)), arg(2));
    ret(arg(0));
  });
  hle("sysclib", 14, [this] {  // memset
    if (arg(2)) std::memset(cpu.ptr(arg(0), arg(2)), int(arg(1)), arg(2));
    ret(arg(0));
  });
  hle("sysclib", 23, [this] {  // strcpy
    const std::string s = readString(arg(1));
    std::memcpy(cpu.ptr(arg(0), uint32_t(s.size()) + 1), s.c_str(), s.size() + 1);
    ret(arg(0));
  });
  hle("sysclib", 27, [this] { ret(uint32_t(readString(arg(0)).size())); });  // strlen
  hle("sysclib", 22, [this] { ret(uint32_t(readString(arg(0)).compare(readString(arg(1))))); });  // strcmp
  hle("sysclib", 29, [this] {  // strncmp
    ret(uint32_t(readString(arg(0)).substr(0, arg(2)).compare(readString(arg(1)).substr(0, arg(2)))));
  });

  // vblank
  hle("vblank", 4, [this] { ret(0); block(Wait::Vblank); });  // WaitVblankStart
  hle("vblank", 5, [this] { ret(0); block(Wait::Vblank); });  // WaitVblankEnd

  // timrman: the timer counts the IOP clock divided by its prescale.
  hle("timrman", 4, [this] {  // AllocHardTimer(source, size, prescale)
    timers_.emplace_back();
    timers_.back().prescale = std::max<uint32_t>(1, arg(2));
    ret(uint32_t(timers_.size()));
  });
  auto timer = [this](uint32_t id) -> Timer* { return id - 1 < timers_.size() ? &timers_[id - 1] : nullptr; };
  hle("timrman", 6, [this, timer] {  // FreeHardTimer
    if (Timer* t = timer(arg(0))) t->running = false;
    ret(0);
  });
  hle("timrman", 20, [this, timer] {  // SetTimerHandler(id, compare, handler, arg)
    if (Timer* t = timer(arg(0))) t->compare = arg(1), t->handler = arg(2), t->arg = arg(3), t->gp = cpu.s.gpr[28];
    ret(0);
  });
  hle("timrman", 22, [this, timer] {  // SetupHardTimer(id, source, mode, prescale)
    if (Timer* t = timer(arg(0))) t->prescale = std::max<uint32_t>(1, arg(3));
    ret(0);
  });
  hle("timrman", 23, [this, timer] {  // StartHardTimer
    if (Timer* t = timer(arg(0)); t && t->handler && t->compare) {
      t->running = true;
      t->nextAt = now() + uint64_t(t->compare) * t->prescale * kEeCyclesPerSysclock;
    }
    ret(0);
  });
  hle("timrman", 24, [this, timer] {  // StopHardTimer
    if (Timer* t = timer(arg(0))) t->running = false;
    ret(0);
  });

  // sifman, sifcmd: the RPC servers of the modules.
  hle("sifman", 5, [this] { ret(0); });   // sceSifInit
  hle("sifman", 29, [this] { ret(1); });  // sceSifCheckInit
  hle("sifcmd", 14, [this] { ret(0); });  // sceSifInitRpc
  hle("sifcmd", 17, [this] {              // sceSifRegisterRpc(sd, sid, func, buf, cfunc, cbuf, qd)
    servers_.push_back({arg(1), arg(0), arg(2), arg(3), arg(6)});
    sif_.log(std::format("iop: RPC server 0x{:08X} registered (handler 0x{:06X}, buffer 0x{:06X})", arg(1), arg(2), arg(3)));
    ret(0);
  });
  hle("sifcmd", 19, [this] {  // sceSifSetRpcQueue(qd, thread)
    queueThread_[arg(0)] = int(arg(1)) - 1;
    ret(0);
  });
  hle("sifcmd", 22, [this] {  // sceSifRpcLoop(qd)
    threads_[current_].rpcQueue = arg(0);
    block(Wait::RpcLoop);
    startRpcs();
  });

  // ioman: files on the disc.
  hle("ioman", 4, [this] {  // open(path, flags)
    const std::string path = readString(arg(0));
    const auto entry = sif_.iso.find(path);
    sif_.log(std::format("iop: open \"{}\"{}", path, entry ? "" : " (not found)"));
    if (!entry) return ret(uint32_t(-2));
    files_.push_back({entry->lba, entry->size, 0});
    ret(uint32_t(files_.size() - 1));
  });
  hle("ioman", 5, [this] { ret(0); });  // close
  hle("ioman", 6, [this] {              // read(fd, buf, size)
    if (arg(0) >= files_.size()) return ret(uint32_t(-9));
    File& f = files_[arg(0)];
    const uint32_t n = std::min(arg(2), f.size - std::min(f.pos, f.size));
    if (n) {
      const uint32_t first = f.pos / Iso9660::kSectorSize, last = (f.pos + n - 1) / Iso9660::kSectorSize;
      std::vector<uint8_t> tmp((last - first + 1) * Iso9660::kSectorSize);
      uint8_t* dst = cpu.ptr(arg(1), n);
      if (!dst || !sif_.iso.readSectors(f.lba + first, last - first + 1, tmp.data())) return ret(uint32_t(-5));
      std::memcpy(dst, tmp.data() + f.pos % Iso9660::kSectorSize, n);
      f.pos += n;
    }
    ret(n);
  });
  hle("ioman", 8, [this] {  // lseek(fd, offset, whence)
    if (arg(0) >= files_.size()) return ret(uint32_t(-9));
    File& f = files_[arg(0)];
    const int64_t from = arg(2) == 0 ? 0 : arg(2) == 1 ? f.pos : f.size;
    f.pos = uint32_t(std::clamp<int64_t>(from + int32_t(arg(1)), 0, f.size));
    ret(f.pos);
  });

  // cdvdman: reads complete at once.
  hle("cdvdman", 4, [this] { ret(1); });  // sceCdInit
  hle("cdvdman", 6, [this] {              // sceCdRead(lsn, sectors, buf, mode)
    uint8_t* dst = cpu.ptr(arg(2), arg(1) * Iso9660::kSectorSize);
    ret(dst && sif_.iso.readSectors(arg(0), arg(1), dst) ? 1 : 0);
  });
  hle("cdvdman", 8, [this] { ret(0); });  // sceCdGetError
  hle("cdvdman", 10, [this] {             // sceCdSearchFile(sceCdlFILE*, name)
    const std::string name = readString(arg(1));
    const auto entry = sif_.iso.find(name);
    sif_.log(std::format("iop: cdSearchFile \"{}\"{}", name, entry ? "" : " (not found)"));
    if (!entry) return ret(0);
    uint8_t* f = cpu.ptr(arg(0), 32);
    std::memset(f, 0, 32);
    std::memcpy(f, &entry->lba, 4);
    std::memcpy(f + 4, &entry->size, 4);
    const std::string base = entry->path.substr(entry->path.find_last_of('/') + 1) + ";1";
    std::memcpy(f + 8, base.data(), std::min<size_t>(base.size(), 15));
    ret(1);
  });
  hle("cdvdman", 11, [this] { ret(0); });     // sceCdSync: never busy
  hle("cdvdman", 12, [this] { ret(0x14); });  // sceCdGetDiskType: PS2 DVD
  hle("cdvdman", 13, [this] { ret(2); });     // sceCdDiskReady: complete
  hle("cdvdman", 75, [this] { ret(1); });     // sceCdMmode(media)

  // libsd over the SPU2 model (export indices follow the sdrdrv remote command order).
  hle("libsd", 2, [this] { ret(0); });  // sceSdQuit
  hle("libsd", 4, [this] {              // sceSdInit
    spu_.init();
    ret(0);
  });
  hle("libsd", 5, [this] { spu_.setParam(arg(0), arg(1)); ret(0); });
  hle("libsd", 6, [this] { ret(spu_.getParam(arg(0))); });
  hle("libsd", 7, [this] { spu_.setSwitch(arg(0), arg(1)); ret(0); });
  hle("libsd", 8, [this] { ret(spu_.getSwitch(arg(0))); });
  hle("libsd", 9, [this] { spu_.setAddr(arg(0), arg(1)); ret(0); });
  hle("libsd", 10, [this] { ret(spu_.getAddr(arg(0))); });
  hle("libsd", 11, [this] { spu_.setCoreAttr(arg(0), arg(1)); ret(0); });
  hle("libsd", 12, [this] { ret(spu_.getCoreAttr(arg(0))); });
  hle("libsd", 13, [this] {  // sceSdNote2Pitch(centerNote, centerFine, note, fine): 0x1000 = no shift
    const double semis = (int(arg(2) & 0xFFFF) - int(arg(0) & 0xFFFF)) +
                         (int16_t(arg(3)) - int(arg(1) & 0xFFFF)) / 128.0;
    ret(uint32_t(std::clamp(4096.0 * std::exp2(semis / 12.0), 0.0, 16383.0)));
  });
  // Batches: {u16 func, u16 entry, u32 value}; ProcBatchEx repeats voice entries for each voice
  // in a mask.
  auto batch = [this](uint32_t func, uint32_t entry, uint32_t value) -> uint32_t {
    switch (func) {
      case 1: spu_.setParam(entry, value); return 0;
      case 2: spu_.setSwitch(entry, value); return 0;
      case 3: spu_.setAddr(entry, value); return 0;
      case 4: spu_.setCoreAttr(entry, value); return 0;
      case 5: wr32(entry, value); return 0;  // write IOP memory
      case 0x10: return spu_.getParam(entry);
      case 0x12: return spu_.getSwitch(entry);
      case 0x13: return spu_.getAddr(entry);
      case 0x14: return spu_.getCoreAttr(entry);
    }
    return 0;
  };
  hle("libsd", 15, [this, batch] {  // sceSdProcBatch(batch, returns, count)
    const uint32_t list = arg(0), rets = arg(1), count = arg(2);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t head = rd32(list + 8 * i);
      const uint32_t r = batch(head & 0xFFFF, head >> 16, rd32(list + 8 * i + 4));
      if (rets) wr32(rets + 4 * i, r);
    }
    ret(count);
  });
  hle("libsd", 16, [this, batch] {  // sceSdProcBatchEx(batch, returns, count, voiceMask)
    const uint32_t list = arg(0), rets = arg(1), count = arg(2), mask = arg(3);
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t head = rd32(list + 8 * i), func = head & 0xFFFF, entry = head >> 16;
      const uint32_t value = rd32(list + 8 * i + 4);
      const bool perVoice = func == 1 || func == 3 || func == 0x10 || func == 0x13;
      for (uint32_t v = 0; v < 24; ++v) {
        if (perVoice && !(mask & (1u << v))) continue;
        const uint32_t r = batch(func, perVoice ? (entry & ~0x3Eu) | v << 1 : entry, value);
        if (rets) wr32(rets + 4 * n, r);
        ++n;
        if (!perVoice) break;
      }
    }
    ret(n);
  });
  hle("libsd", 17, [this] {  // sceSdVoiceTrans(channel, mode, iopAddr, spuAddr, size)
    const uint32_t ch = arg(0) & 1, mode = arg(1), iop = arg(2), spuAddr = arg(3), size = arg(4);
    uint8_t* p = cpu.ptr(iop, size);
    uint32_t n = 0;
    if (p) n = (mode & 3) == 1 ? spu_.read(spuAddr, p, size) : spu_.write(spuAddr, p, size);
    transferDone(int(ch));
    ret(n);
  });
  hle("libsd", 18, [this] {  // sceSdBlockTrans(channel, mode, iopAddr, size, ...)
    const uint32_t ch = arg(0) & 1, mode = arg(1);
    if ((mode & 3) == 2) {  // stop
      spu_.stopAutoDma(int(ch));
      return ret(0);
    }
    if (mode & 0x10) {  // loop: the core keeps reading the buffer at 48 kHz
      spu_.startAutoDma(int(ch), arg(2), arg(3));
      admaHalves_[ch] = 0;
    } else {
      transferDone(int(ch));
    }
    ret(0);
  });
  hle("libsd", 19, [this] { ret(1); });  // sceSdVoiceTransStatus: finished
  hle("libsd", 20, [this] {              // sceSdBlockTransStatus: current position and half
    ret(spu_.autoDmaStatus(int(arg(0) & 1)));
  });
  auto setHandler = [this](uint32_t ch, uint32_t handler, uint32_t a) {
    transHandler_[ch & 1] = handler, transArg_[ch & 1] = a, transGp_[ch & 1] = cpu.s.gpr[28];
    ret(0);
  };
  hle("libsd", 21, [this, setHandler] { setHandler(arg(0), arg(1), 0); });       // sceSdSetTransCallback
  hle("libsd", 26, [this, setHandler] { setHandler(arg(0), arg(1), arg(2)); });  // sceSdSetTransIntrHandler
  hle("libsd", 22, [this] {  // sceSdSetIRQCallback(callback(void*))
    spuIrq_ = {arg(0), cpu.s.gpr[28], 0, 0};
    spuIrqOldStyle_ = true;
    ret(0);
  });
  hle("libsd", 27, [this] {  // sceSdSetSpu2IntrHandler(handler(coreBits, arg), arg)
    spuIrq_ = {arg(0), cpu.s.gpr[28], 0, arg(1)};
    spuIrqOldStyle_ = false;
    ret(0);
  });
  hle("libsd", 23, [this] {  // sceSdSetEffectAttr(core, {core, mode, depth_L/R, delay, feedback})
    uint32_t attr[3] = {};
    if (const uint8_t* p = cpu.ptr(arg(1), 12)) std::memcpy(attr, p, 12);
    spu_.setEffect(int(arg(0) & 1), attr[1], int16_t(attr[2]), int16_t(attr[2] >> 16));
    ret(0);
  });
  hle("libsd", 30, [this] {              // sceSdStopTrans
    spu_.stopAutoDma(int(arg(0) & 1));
    ret(0);
  });
}

std::vector<std::string> IopKernel::report() const {
  static const char* kWait[] = {"ready", "sleep", "delay", "sema", "vblank", "rpc loop", "dormant"};
  std::vector<std::string> out;
  out.push_back(std::format("{} instructions, {} kernel calls", cpu.executed, hleCalls));
  for (size_t t = 0; t < threads_.size(); ++t) {
    const Thread& th = threads_[t];
    if (!th.used) continue;
    const IopState& s = int(t) == current_ ? cpu.s : th.ctx;
    out.push_back(std::format("thread {} prio {} {}{} pc {} ra {}", t + 1, th.prio, kWait[int(th.wait)],
                              th.wait == Wait::Sema ? std::format(" #{}", th.sema) : "", where(s.pc), where(s.gpr[31])));
  }
  std::vector<const Hle*> called;
  for (const Hle& h : hleFns_)
    if (h.calls) called.push_back(&h);
  std::sort(called.begin(), called.end(), [](const Hle* a, const Hle* b) { return a->calls > b->calls; });
  std::string line;
  for (const Hle* h : called) line += std::format("{} x{}  ", h->name, h->calls);
  out.push_back(line);
  return out;
}
