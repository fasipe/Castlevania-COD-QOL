#include "ee/kernel.h"

#include <algorithm>
#include <format>
#include <functional>

namespace {
constexpr uint32_t kExitStubPhys = 0x1000;           // kernel RAM, unused by the game
constexpr uint32_t kExitStub = 0x80000000 | kExitStubPhys;
constexpr uint32_t kUserSyscallReturnPhys = 0x1010;  // where SetSyscall handlers return to
constexpr uint32_t kUserSyscallReturn = 0x80000000 | kUserSyscallReturnPhys;
constexpr uint32_t kUserSyscallReturnNum = 0x7FFF;  // internal, outside the BIOS syscall range
// Like the BIOS, keep the syscall table in kernel RAM: libkernl locates it by searching kernel
// memory for the handlers it installed, and may read or patch entries directly. Entries left at
// zero are the syscalls implemented here in C++.
constexpr uint32_t kSyscallTablePhys = 0x2000;
constexpr uint32_t kSyscallTableSize = 0x200;  // libkernl installs entries up to 0x12C
constexpr uint32_t kIdleStubPhys = 0x1020;  // "b ." run while every thread waits
constexpr uint32_t kIntcReturnPhys = 0x1030;  // where interrupt handlers return to
constexpr uint32_t kIntcReturnNum = 0x7FFE;
constexpr uint32_t kIntcStackTop = 0x80068000;  // interrupt handlers run on their own stack
constexpr uint32_t kRamEnd = Memory::kRamSize;

enum : uint32_t { THS_RUN = 1, THS_READY = 2, THS_WAIT = 4, THS_SUSPEND = 8, THS_DORMANT = 0x10 };

inline uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
}  // namespace

Kernel::Kernel(Cpu& cpu) : cpu_(cpu), mem_(cpu.mem), threads_(2), semas_(1) {}

void Kernel::boot(uint32_t entry, const std::string& bootPath) {
  bootPath_ = bootPath;
  // A thread that returns from its entry function lands here and calls ExitThread.
  mem_.write<uint32_t>(kExitStubPhys + 0, 0x24030023);  // addiu v1, zero, 0x23
  mem_.write<uint32_t>(kExitStubPhys + 4, 0x0000000C);  // syscall
  mem_.write<uint32_t>(kExitStubPhys + 8, 0x1000FFFF);  // b . (never reached)
  mem_.write<uint32_t>(kExitStubPhys + 12, 0);
  mem_.write<uint32_t>(kUserSyscallReturnPhys + 0, 0x24030000 | kUserSyscallReturnNum);  // addiu v1, zero, n
  mem_.write<uint32_t>(kUserSyscallReturnPhys + 4, 0x0000000C);                           // syscall
  mem_.write<uint32_t>(kIdleStubPhys + 0, 0x1000FFFF);                                    // b .
  mem_.write<uint32_t>(kIdleStubPhys + 4, 0);
  mem_.write<uint32_t>(kIntcReturnPhys + 0, 0x24030000 | kIntcReturnNum);                 // addiu v1, zero, n
  mem_.write<uint32_t>(kIntcReturnPhys + 4, 0x0000000C);                                  // syscall

  cpu_.s.reset(entry);
  Thread& main = threads_[1];
  main.used = true;
  main.status = THS_RUN;
  main.entry = entry;
  current_ = 1;
  ready_.push_back(1);
}

uint32_t Kernel::arg(int reg) const { return static_cast<uint32_t>(cpu_.s.gpr[reg].ud[0]); }
void Kernel::ret(uint32_t v) { cpu_.s.gpr[2].ud[0] = sx32(v); }
bool Kernel::validThread(int id) const { return id > 0 && id < int(threads_.size()) && threads_[id].used; }
bool Kernel::validSema(int id) const { return id > 0 && id < int(semas_.size()) && semas_[id].used; }
CpuState& Kernel::contextOf(int id) {
  if (id != current_) return threads_[id].ctx;
  return inInterrupt_ ? intFrame_ : cpu_.s;
}

std::vector<std::string> Kernel::recentCalls() const {
  std::vector<std::string> lines;
  for (uint64_t i = noted_ - std::min<uint64_t>(noted_, recent_.size()); i < noted_; ++i) {
    const Note& n = recent_[i % recent_.size()];
    const int64_t* a = n.args;
    lines.push_back(std::format("[t{}] {}({})", n.thread, n.name, std::vformat(n.format, std::make_format_args(a[0], a[1], a[2], a[3]))));
  }
  return lines;
}

void Kernel::unknown(uint32_t n) {
  cpu_.halt(StopReason::Unimplemented,
            std::format("syscall 0x{:02X} (v1=0x{:08X}) at pc=0x{:08X}", n, arg(3), cpu_.currentPc));
}

void Kernel::makeReady(int id) {
  Thread& t = threads_[id];
  t.status = id == current_ ? THS_RUN : THS_READY;
  if (std::find(ready_.begin(), ready_.end(), id) == ready_.end()) ready_.push_back(id);
}

void Kernel::block(int id) {
  threads_[id].status = THS_WAIT;
  ready_.erase(std::remove(ready_.begin(), ready_.end(), id), ready_.end());
}

// Runs the highest-priority runnable thread (lower number = higher priority); among equals
// the one that has been runnable longest. With nothing runnable the CPU idles (thread 0) until
// an interrupt makes a thread ready. Inside an interrupt handler switching is deferred.
void Kernel::reschedule() {
  if (inInterrupt_) {
    needReschedule_ = true;
    return;
  }
  int best = -1;
  for (int id : ready_)
    if (best < 0 || threads_[id].prio < threads_[best].prio) best = id;
  if (best < 0) best = 0;
  if (best == current_) return;
  if (validThread(current_)) {
    Thread& old = threads_[current_];
    old.ctx = std::move(cpu_.s);
    if (old.status == THS_RUN) old.status = THS_READY;
  }
  current_ = best;
  if (best == 0) {
    cpu_.s.reset(0x80000000 | kIdleStubPhys);
    return;
  }
  cpu_.s = std::move(threads_[best].ctx);
  threads_[best].status = THS_RUN;
}

// Called by the CPU every few dozen instructions: advances time (skipping ahead while idle)
// and starts the interrupt handlers for any pending, unmasked INTC cause or DMA channel.
void Kernel::poll() {
  Hardware& hw = mem_.hw;
  sif->poll();
  const bool dmacPending = hw.dmacStat & hw.dmacMask;
  if (current_ == 0 && !inInterrupt_ && !dmacPending) {
    const uint64_t next = hw.nextEvent();
    if (next == Hardware::kNever) {
      cpu_.halt(StopReason::Idle, "every thread is waiting and no interrupt source is enabled\n" + describeThreads());
      return;
    }
    hw.cycles = std::max(hw.cycles, next);
  }
  hw.update();
  const uint32_t status = cpu_.s.cop0[12];
  const bool enabled = (status & 1) && (status & 0x10000) && !(status & 6);
  if (inInterrupt_ || !enabled || !(hw.pending() || (hw.dmacStat & hw.dmacMask))) return;

  for (int ch = 0; ch < 16; ++ch) {  // DMAC channel handlers (INTC cause 1 is routed here)
    if (!(hw.dmacStat & hw.dmacMask & (1u << ch))) continue;
    hw.dmacStat &= ~(1u << ch);
    ++interrupts[Hardware::kDmac];
    for (const Handler& h : dmacHandlers)
      if (int(h.cause) == ch) intQueue_.push_back(h);
  }
  for (int cause = 0; cause < 15; ++cause) {
    if (!(hw.pending() & (1u << cause))) continue;
    hw.intcStat &= ~(1u << cause);  // the kernel acknowledges the cause before dispatching
    ++interrupts[cause];
    if (cause == Hardware::kVblankStart && vsyncFlag_) {
      uint64_t csr = 0;
      hw.read(0x12001000, csr);
      mem_.write<uint32_t>(vsyncFlag_, 1);
      mem_.write<uint64_t>(vsyncCsr_ & ~7u, csr);
      vsyncFlag_ = vsyncCsr_ = 0;
    }
    for (const Handler& h : intcHandlers)
      if (int(h.cause) == cause) intQueue_.push_back(h);
  }
  if (intQueue_.empty()) return;
  inInterrupt_ = true;
  intFrame_ = std::move(cpu_.s);
  runNextHandler();
}

void Kernel::runNextHandler() {
  const Handler h = intQueue_.front();
  intQueue_.pop_front();
  auto callstack = std::move(intFrame_.callstack);  // the handler starts its own
  cpu_.s = intFrame_;                                // keeps the interrupted code's gp
  intFrame_.callstack = std::move(callstack);
  cpu_.s.callstack.clear();
  cpu_.s.gpr[4].ud[0] = sx32(h.cause);
  cpu_.s.gpr[5].ud[0] = sx32(h.arg);
  cpu_.s.gpr[6].ud[0] = 0;
  cpu_.s.gpr[29].ud[0] = sx32(kIntcStackTop);
  cpu_.s.gpr[31].ud[0] = sx32(0x80000000 | kIntcReturnPhys);
  cpu_.s.cop0[12] |= 2;  // EXL: no nested interrupts
  cpu_.s.pc = h.func;
  cpu_.s.npc = h.func + 4;
}

void Kernel::finishHandler() {
  if (!intQueue_.empty()) {
    runNextHandler();
    return;
  }
  cpu_.s = std::move(intFrame_);
  inInterrupt_ = false;
  // Handlers wake threads with the i* syscalls, which never switch; like the BIOS, pick the
  // best runnable thread on the way out of every interrupt.
  needReschedule_ = false;
  reschedule();
}

void Kernel::wakeSemaWaiter(int semaId) {
  Sema& sema = semas_[semaId];
  const int id = sema.waiters.front();
  sema.waiters.pop_front();
  threads_[id].waitSema = -1;
  contextOf(id).gpr[2].ud[0] = sx32(semaId);
  makeReady(id);
}

std::string Kernel::describeThreads() const {
  std::string out = current_ == 0 ? "cpu idle\n" : "";
  for (size_t id = 1; id < threads_.size(); ++id) {
    const Thread& t = threads_[id];
    if (!t.used) continue;
    const char* st = t.status == THS_RUN ? "running" : t.status == THS_READY ? "ready"
                   : t.status == THS_DORMANT ? "dormant" : t.sleeping ? "sleeping" : t.waitSema > 0 ? "waiting sema" : "waiting";
    out += std::format("thread {} prio {} entry 0x{:08X} {}", id, t.prio, t.entry, st);
    if (t.waitSema > 0) out += std::format(" {}", t.waitSema);
    const uint32_t pc = int(id) == current_ ? cpu_.s.pc : t.ctx.pc;
    out += std::format(" pc=0x{:08X}\n", pc);
  }
  return out;
}

void Kernel::syscall() {
  const int32_t raw = static_cast<int32_t>(arg(3));
  const uint32_t n = static_cast<uint32_t>(raw < 0 ? -raw : raw);  // negative = interrupt-context variant
  const uint32_t a0 = arg(4), a1 = arg(5), a2 = arg(6), a3 = arg(7), t0 = arg(8);

  // Handlers the game installed with SetSyscall run as ordinary guest functions; the
  // caller's context is restored when they return, keeping only their result in v0.
  if (n == kIntcReturnNum && inInterrupt_) {
    finishHandler();
    return;
  }
  if (n == kUserSyscallReturnNum && !userFrames_.empty()) {
    const uint64_t result = cpu_.s.gpr[2].ud[0];
    cpu_.s = std::move(userFrames_.back());
    userFrames_.pop_back();
    cpu_.s.gpr[2].ud[0] = result;
    return;
  }
  if (const uint32_t handler = n < kSyscallTableSize ? mem_.read<uint32_t>(kSyscallTablePhys + 4 * n) : 0) {
    note("UserSyscall", "0x{:02X} -> 0x{:08X}", n, handler);
    userFrames_.push_back(cpu_.s);
    cpu_.s.gpr[31].ud[0] = sx32(kUserSyscallReturn);
    cpu_.s.pc = handler;
    cpu_.s.npc = handler + 4;
    return;
  }

  switch (n) {
    case 0x02:  // SetGsCrt(interlace, mode, field)
      note("SetGsCrt", "{}, {}, {}", a0, a1, a2);
      if (a1 == 2 || a1 == 3) mem_.hw.setNtsc(a1 == 2);  // 2 = NTSC (60 Hz), 3 = PAL
      return;
    case 0x04:  // Exit
    case 0x06:  // LoadExecPS2
    case 0x07:  // ExecPS2
      note("Exit/LoadExecPS2", "0x{:08X}", a0);
      cpu_.halt(StopReason::Exit, std::format("syscall 0x{:02X}", n));
      return;

    case 0x10:  // AddIntcHandler(cause, handler, next, arg): next < 0 appends, otherwise prepends
    case 0x12: {  // AddDmacHandler(channel, handler, next, arg)
      auto& list = n == 0x10 ? intcHandlers : dmacHandlers;
      const Handler h{a0, a1, a3, ++handlerIds_};
      if (int32_t(a2) < 0)
        list.push_back(h);
      else
        list.insert(list.begin(), h);
      note(n == 0x10 ? "AddIntcHandler" : "AddDmacHandler", "{}, 0x{:08X} -> {}", a0, a1, h.id);
      ret(h.id);
      return;
    }
    case 0x11:  // RemoveIntcHandler(cause, id)
    case 0x13: {  // RemoveDmacHandler(channel, id): returns how many handlers the cause still has
      auto& list = n == 0x11 ? intcHandlers : dmacHandlers;
      note("RemoveHandler", "{}, {}", a0, a1);
      std::erase_if(list, [&](const Handler& h) { return h.cause == a0 && h.id == a1; });
      ret(uint32_t(std::count_if(list.begin(), list.end(), [&](const Handler& h) { return h.cause == a0; })));
      return;
    }
    case 0x14: case 0x15: case 0x16: case 0x17:  // _EnableIntc/_DisableIntc/_EnableDmac/_DisableDmac
    case 0x1A: case 0x1B: case 0x1C: case 0x1D:  // interrupt-context versions
      note(n == 0x14 || n == 0x1A ? "EnableIntc" : n == 0x15 || n == 0x1B ? "DisableIntc"
           : n == 0x16 || n == 0x1C ? "EnableDmac" : "DisableDmac", "{}", a0);
      if (a0 < 15 && (n == 0x14 || n == 0x1A || n == 0x15 || n == 0x1B)) {
        const uint32_t bit = 1u << a0;
        const bool was = mem_.hw.intcMask & bit;
        if (n == 0x14 || n == 0x1A)
          mem_.hw.intcMask |= bit;
        else
          mem_.hw.intcMask &= ~bit;
        ret(was ? 0 : 1);
        return;
      }
      if (a0 < 16) {  // EnableDmac / DisableDmac
        const uint32_t bit = 1u << a0;
        const bool was = mem_.hw.dmacMask & bit;
        if (n == 0x16 || n == 0x1C)
          mem_.hw.dmacMask |= bit;
        else
          mem_.hw.dmacMask &= ~bit;
        ret(was ? 0 : 1);
        return;
      }
      ret(1);
      return;

    case 0x20: {  // CreateThread(ee_thread_t*)
      Thread t;
      t.used = true;
      t.status = THS_DORMANT;
      t.entry = mem_.read<uint32_t>(a0 + 4);
      t.stack = mem_.read<uint32_t>(a0 + 8);
      t.stackSize = mem_.read<uint32_t>(a0 + 12);
      t.gp = mem_.read<uint32_t>(a0 + 16);
      t.initPrio = t.prio = static_cast<int>(mem_.read<uint32_t>(a0 + 20));
      threads_.push_back(t);
      const int id = int(threads_.size()) - 1;
      note("CreateThread", "entry 0x{:08X}, prio {} -> {}", t.entry, t.prio, id);
      ret(id);
      return;
    }
    case 0x21:  // DeleteThread
      note("DeleteThread", "{}", a0);
      if (validThread(int(a0)) && threads_[a0].status == THS_DORMANT) threads_[a0].used = false;
      ret(validThread(int(a0)) ? 0 : 0xFFFFFFFF);
      return;
    case 0x22: {  // StartThread(id, arg)
      note("StartThread", "{}, 0x{:08X}", a0, a1);
      const int id = int(a0);
      if (!validThread(id) || threads_[id].status != THS_DORMANT) {
        ret(0xFFFFFFFF);
        return;
      }
      Thread& t = threads_[id];
      t.ctx.reset(t.entry);
      t.ctx.gpr[4].ud[0] = sx32(a1);
      t.ctx.gpr[28].ud[0] = sx32(t.gp);
      t.ctx.gpr[29].ud[0] = sx32((t.stack + t.stackSize - 0x20) & ~15u);
      t.ctx.gpr[31].ud[0] = sx32(kExitStub);
      t.prio = t.initPrio;
      t.wakeups = 0;
      ret(0);
      makeReady(id);
      reschedule();
      return;
    }
    case 0x23:  // ExitThread
    case 0x24:  // ExitDeleteThread
    case 0x25:  // TerminateThread(id)
    case 0x26: {
      const int id = n <= 0x24 ? current_ : int(a0);
      note(n <= 0x24 ? "ExitThread" : "TerminateThread", "{}", id);
      if (!validThread(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      ret(0);
      block(id);
      threads_[id].status = THS_DORMANT;
      if (n == 0x24) threads_[id].used = false;
      if (id == current_) reschedule();
      return;
    }
    case 0x29:  // ChangeThreadPriority(id, prio)
    case 0x2A: {
      const int id = a0 == 0 ? current_ : int(a0);
      note("ChangeThreadPriority", "{}, {}", id, a1);
      if (!validThread(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      ret(uint32_t(threads_[id].prio));
      threads_[id].prio = int(a1);
      if (n == 0x29) reschedule();
      return;
    }
    case 0x2B:  // RotateThreadReadyQueue(prio)
    case 0x2C: {
      note("RotateThreadReadyQueue", "{}", a0);
      auto it = std::find_if(ready_.begin(), ready_.end(), [&](int id) { return threads_[id].prio == int(a0); });
      if (it != ready_.end()) {
        const int id = *it;
        ready_.erase(it);
        ready_.push_back(id);
      }
      ret(a0);
      if (n == 0x2B) reschedule();
      return;
    }
    case 0x2F:  // GetThreadId
      ret(uint32_t(current_));
      return;
    case 0x30:  // ReferThreadStatus(id, ee_thread_status_t*)
    case 0x31: {
      const int id = a0 == 0 ? current_ : int(a0);
      if (!validThread(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      const Thread& t = threads_[id];
      if (a1) {
        const uint32_t v[9] = {t.status, t.entry, t.stack, t.stackSize, t.gp, uint32_t(t.initPrio), uint32_t(t.prio), 0, 0};
        for (int i = 0; i < 9; ++i) mem_.write<uint32_t>(a1 + 4 * i, v[i]);
      }
      ret(t.status);
      return;
    }
    case 0x32: {  // SleepThread
      note("SleepThread");
      Thread& t = threads_[current_];
      ret(uint32_t(current_));
      if (t.wakeups > 0) {
        --t.wakeups;
        return;
      }
      t.sleeping = true;
      block(current_);
      reschedule();
      return;
    }
    case 0x33:  // WakeupThread(id)
    case 0x34: {
      note("WakeupThread", "{}", a0);
      const int id = int(a0);
      if (!validThread(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      Thread& t = threads_[id];
      ret(0);
      if (t.sleeping) {
        t.sleeping = false;
        makeReady(id);
        if (n == 0x33) reschedule();
      } else {
        ++t.wakeups;
      }
      return;
    }
    case 0x35:  // CancelWakeupThread
    case 0x36: {
      const int id = a0 == 0 ? current_ : int(a0);
      if (!validThread(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      ret(uint32_t(threads_[id].wakeups));
      threads_[id].wakeups = 0;
      return;
    }
    case 0x3C: {  // SetupThread(gp, stack, stackSize, args, root): returns the main thread's sp
      const uint32_t size = a2;
      const uint32_t base = a1 == 0xFFFFFFFF ? kRamEnd - size : a1;
      mainStack_ = base;
      mainStackSize_ = size;
      Thread& main = threads_[1];
      main.gp = a0;
      main.stack = base;
      main.stackSize = size;
      if (a3) {  // struct { int argc; char* argv[16]; char payload[256]; }
        mem_.write<uint32_t>(a3, 1);
        mem_.write<uint32_t>(a3 + 4, a3 + 68);
        for (size_t i = 0; i <= bootPath_.size() && i < 255; ++i)
          mem_.write<uint8_t>(a3 + 68 + uint32_t(i), i < bootPath_.size() ? uint8_t(bootPath_[i]) : 0);
      }
      note("SetupThread", "gp 0x{:08X}, stack 0x{:08X}+0x{:X}, root 0x{:08X}", a0, base, size, t0);
      ret((base + size - 0x2A0) & ~15u);
      return;
    }
    case 0x3D:  // SetupHeap(start, size)
      heapStart_ = a0;
      heapEnd_ = a1 == 0xFFFFFFFF ? (mainStack_ ? mainStack_ : kRamEnd) : a0 + a1;
      note("SetupHeap", "0x{:08X}..0x{:08X}", heapStart_, heapEnd_);
      ret(heapEnd_);
      return;
    case 0x3E:  // EndOfHeap
      ret(heapEnd_);
      return;

    case 0x40: {  // CreateSema(ee_sema_t*)
      Sema s;
      s.used = true;
      s.max = int(mem_.read<uint32_t>(a0 + 4));
      s.init = int(mem_.read<uint32_t>(a0 + 8));
      s.count = s.init;  // the struct's own count field is ignored by the BIOS
      s.attr = mem_.read<uint32_t>(a0 + 16);
      s.option = mem_.read<uint32_t>(a0 + 20);
      int id;
      if (!freeSemas_.empty()) {
        std::pop_heap(freeSemas_.begin(), freeSemas_.end(), std::greater<>());
        id = freeSemas_.back();
        freeSemas_.pop_back();
        semas_[id] = std::move(s);
      } else {
        id = int(semas_.size());
        semas_.push_back(std::move(s));
      }
      note("CreateSema", "count {}, max {} -> {}", semas_[id].count, semas_[id].max, id);
      ret(id);
      return;
    }
    case 0x41: {  // DeleteSema: threads still waiting are released, their WaitSema failing
      note("DeleteSema", "{}", a0);
      const int id = int(a0);
      if (validSema(id)) {
        Sema& sema = semas_[id];
        for (const int waiter : sema.waiters) {
          threads_[waiter].waitSema = -1;
          contextOf(waiter).gpr[2].ud[0] = sx32(0xFFFFFFFF);
          makeReady(waiter);
        }
        sema = {};
        freeSemas_.push_back(id);
        std::push_heap(freeSemas_.begin(), freeSemas_.end(), std::greater<>());
      }
      ret(a0);
      return;
    }
    case 0x42:  // SignalSema
    case 0x43: {
      note("SignalSema", "{}", a0);
      const int id = int(a0);
      if (!validSema(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      ret(a0);
      if (!semas_[id].waiters.empty()) {
        wakeSemaWaiter(id);
        if (n == 0x42) reschedule();
      } else {
        ++semas_[id].count;
      }
      return;
    }
    case 0x44: {  // WaitSema
      note("WaitSema", "{}", a0);
      const int id = int(a0);
      if (!validSema(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      ret(a0);
      if (semas_[id].count > 0) {
        --semas_[id].count;
        // A thread that gives a semaphore back and takes it again, over and over, is waiting for
        // an interrupt or another thread (the game does it when its DMA queue is full, while
        // loading): a loop like any other for the idle detection, though it is made of syscalls.
        cpu_.idleCheck(cpu_.currentPc, 0);
        return;
      }
      semas_[id].waiters.push_back(current_);
      threads_[current_].waitSema = id;
      block(current_);
      reschedule();
      return;
    }
    case 0x45:  // PollSema
    case 0x46: {
      const int id = int(a0);
      if (validSema(id) && semas_[id].count > 0) {
        --semas_[id].count;
        ret(a0);
      } else {
        ret(0xFFFFFFFF);
      }
      return;
    }
    case 0x47:  // ReferSemaStatus(id, ee_sema_t*)
    case 0x48: {
      const int id = int(a0);
      if (!validSema(id)) {
        ret(0xFFFFFFFF);
        return;
      }
      const Sema& s = semas_[id];
      const uint32_t v[6] = {uint32_t(s.count), uint32_t(s.max), uint32_t(s.init), uint32_t(s.waiters.size()), s.attr, s.option};
      for (int i = 0; i < 6; ++i) mem_.write<uint32_t>(a1 + 4 * i, v[i]);
      ret(a0);
      return;
    }

    case 0x4A:  // SetOsdConfigParam
    case 0x6F:  // SetOsdConfigParam2
      note("SetOsdConfigParam");
      return;
    case 0x6E:  // GetOsdConfigParam2(buf, size, offset): extended settings, all defaults
      for (uint32_t i = 0; i < a1; ++i) mem_.write<uint8_t>(a0 + i, 0);
      note("GetOsdConfigParam2", "{} bytes", a1);
      return;
    case 0x4B:  // GetOsdConfigParam(ConfigParam*): language English (1), 4:3, RGB
      mem_.write<uint32_t>(a0, (1u << 16) | (1u << 13));
      note("GetOsdConfigParam");
      return;
    case 0x64:  // FlushCache
    case 0x68:  // iFlushCache
      return;
    case 0x70:  // GsGetIMR
      ret(uint32_t(mem_.hw.gsImr));
      return;
    case 0x71:  // GsPutIMR
      mem_.hw.setGsImr(a0);
      note("GsPutIMR", "0x{:X}", a0);
      return;
    case 0x73:  // SetVSyncFlag(flag, csr): filled in at the next VBlank start
      vsyncFlag_ = a0;
      vsyncCsr_ = a1;
      return;
    case 0x74:  // SetSyscall
      note("SetSyscall", "0x{:X}, 0x{:08X}", a0, a1);
      if (a0 < kSyscallTableSize) mem_.write<uint32_t>(kSyscallTablePhys + 4 * a0, a1);
      return;
    case 0x6B:  // SifStopDma (used around an IOP reboot)
      note("SifStopDma");
      ret(0);
      return;
    case 0x76:  // SifDmaStat: negative = finished; transfers complete instantly for now
      ret(0xFFFFFFFF);
      return;
    case 0x77: {  // SifSetDma(SifDmaTransfer_t*, count): {src, dest, size, attr} each, done at once
      for (uint32_t i = 0; i < a1 && cpu_.stop == StopReason::None; ++i)
        sif->dma(mem_.read<uint32_t>(a0 + 16 * i), mem_.read<uint32_t>(a0 + 16 * i + 4),
                 mem_.read<uint32_t>(a0 + 16 * i + 8));
      ret(++sifDmaId_);
      return;
    }
    case 0x78:  // SifSetDChain
      return;
    case 0x79:  // SifSetReg(reg, value)
      sif->setReg(a0, a1);
      ret(0);
      return;
    case 0x7A:  // SifGetReg(reg)
      ret(sif->getReg(a0));
      return;
    case 0x7C:  // Deci2Call: no debugger attached
      ret(0xFFFFFFFF);
      return;
    case 0x7E:  // MachineType
      ret(0);
      return;
    case 0x7F:  // GetMemorySize
      ret(kRamEnd);
      return;
  }
  unknown(n);
}
