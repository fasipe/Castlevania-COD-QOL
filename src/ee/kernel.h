#pragma once
#include <cstdint>
#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "ee/cpu.h"
#include "iop/sif.h"

// High-level replacement for the PS2 EE kernel (the BIOS part a game calls through
// `syscall`): threads, semaphores, heap setup, interrupt handler registration and SIF.
// Unknown syscalls stop the run with their number, so the next one to implement is known.
class Kernel {
public:
  explicit Kernel(Cpu& cpu);

  // Prepares kernel RAM and makes the code at `entry` the running main thread.
  void boot(uint32_t entry, const std::string& bootPath);
  void syscall();
  void poll();  // advance time and deliver pending interrupts

  uint64_t interrupts[15]{};  // interrupts delivered per INTC cause
  Sif* sif = nullptr;

  std::string describeThreads() const;
  std::vector<std::string> recentCalls() const;  // last syscalls, oldest first
  struct Handler {
    uint32_t cause, func, arg, id;
  };
  std::vector<Handler> intcHandlers, dmacHandlers;

private:
  struct Thread {
    bool used = false;
    uint32_t status = 0;
    bool sleeping = false;
    int waitSema = -1;
    uint32_t entry = 0, stack = 0, stackSize = 0, gp = 0;
    int initPrio = 0, prio = 0, wakeups = 0;
    CpuState ctx;
  };
  struct Sema {
    bool used = false;
    int count = 0, max = 0, init = 0;
    uint32_t attr = 0, option = 0;
    std::deque<int> waiters;
  };

  // Remembers a syscall for the report; the arguments are formatted only when it is printed.
  template <class... A>
  void note(const char* name, const char* format = "", A... args) {
    recent_[noted_++ % recent_.size()] = {name, format, current_, {int64_t(args)...}};
  }
  struct Note {
    const char* name;
    const char* format;
    int thread;
    int64_t args[4];
  };
  std::array<Note, 48> recent_{};  // a ring: every syscall notes itself, and the game makes many
  uint64_t noted_ = 0;
  void ret(uint32_t v);
  uint32_t arg(int reg) const;
  bool validThread(int id) const;
  bool validSema(int id) const;
  void makeReady(int id);
  void block(int id);
  void reschedule();
  void runNextHandler();
  void finishHandler();
  void wakeSemaWaiter(int semaId);
  CpuState& contextOf(int id);
  void unknown(uint32_t n);

  Cpu& cpu_;
  Memory& mem_;
  std::vector<Thread> threads_;  // index = thread id; 0 is unused
  // Index = semaphore id (0 is unused). Deleted ids are given out again, lowest first, as the
  // BIOS does: the game creates and deletes a semaphore for every short wait (thousands per
  // frame), so the table must not grow with them.
  std::deque<Sema> semas_;
  std::vector<int> freeSemas_;   // deleted ids (a min-heap)
  std::deque<int> ready_;        // runnable threads (including the running one), FIFO
  int current_ = 0;
  uint32_t heapStart_ = 0, heapEnd_ = 0;
  uint32_t mainStack_ = 0, mainStackSize_ = 0;
  uint32_t vsyncFlag_ = 0, vsyncCsr_ = 0;  // SetVSyncFlag targets
  std::vector<CpuState> userFrames_;           // callers suspended inside such handlers
  bool inInterrupt_ = false;
  bool needReschedule_ = false;
  CpuState intFrame_;                          // context the interrupt arrived in
  std::deque<Handler> intQueue_;               // handlers still to run for this interrupt
  uint32_t sifDmaId_ = 0;
  uint32_t handlerIds_ = 0;
  std::string bootPath_;
};
