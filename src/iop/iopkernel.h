#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "iop/iopcpu.h"

class Sif;
class Spu2;

// The IOP's operating system for the modules the port runs on the IOP CPU (the game's sound
// driver and its libraries): IRX loading (relocation, import binding, library exports), threads,
// semaphores, delays, VBlank waits, hardware timers, the SIF RPC servers the modules register,
// disc access and libsd over the SPU2 model. Each imported kernel function (library name and
// export index) is implemented in C++.
class IopKernel {
public:
  IopKernel(Sif& sif, std::vector<uint8_t>& ram, Spu2& spu);

  // Loads and starts an IRX module; false (with a reason in `error`) if it cannot be linked or
  // its start routine refuses to stay resident.
  bool loadModule(const std::vector<uint8_t>& file, const std::string& name, std::string& error);

  void advance();  // runs the IOP up to the EE's current time
  void onVblank();
  uint64_t nextEvent() const;  // EE cycle the IOP next needs to run (now if it has work)

  // SIF RPC servers registered by the modules.
  bool findServer(uint32_t sid, uint32_t& handle, uint32_t& buffer) const;
  // Queues a call; `done(replyIopAddr)` runs when the server function returns. False if the
  // handle is not an emulated server.
  bool callServer(uint32_t handle, uint32_t fn, uint32_t size, std::function<void(uint32_t)> done);

  void call(uint32_t hleId);  // from IopCpu at an import stub
  std::string where(uint32_t addr) const;  // "MODSD.IRX+0x34C0"
  std::vector<std::string> report() const;  // threads and kernel call counts

  IopCpu cpu;
  uint64_t hleCalls = 0;

private:
  enum class Wait { None, Sleep, Delay, Sema, Vblank, RpcLoop, Dormant };
  struct Thread {
    bool used = false;
    uint32_t entry = 0, stack = 0, stackSize = 0, gp = 0;
    int prio = 0;
    Wait wait = Wait::Dormant;
    int sema = 0;
    uint64_t wakeAt = 0;  // EE cycles, for Delay
    uint64_t readySeq = 0;
    int wakeups = 0;
    uint32_t rpcQueue = 0;
    IopState ctx;
    IopState rpcLoopCtx;  // where the thread sits in sceSifRpcLoop while serving a call
    std::function<void(uint32_t)> rpcDone;
  };
  struct Sema {
    bool used = false;
    int count = 0, max = 0;
    std::deque<int> waiters;
  };
  struct Server {
    uint32_t sid, handle, func, buffer, queue;
  };
  struct Request {
    size_t server;
    uint32_t fn, size;
    std::function<void(uint32_t)> done;
  };
  struct Timer {
    bool running = false;
    uint32_t handler = 0, arg = 0, gp = 0, compare = 0, prescale = 1;
    uint64_t nextAt = 0;
  };
  struct Interrupt {  // a guest handler to run in interrupt context
    uint32_t func, gp, a0, a1;
  };
  struct Hle {
    std::string name;
    std::function<void()> fn;
    uint64_t calls = 0;
  };
  struct File {
    uint32_t lba = 0, size = 0, pos = 0;
  };

  void registerHle();
  void hle(const std::string& lib, uint32_t index, std::function<void()> fn);
  uint32_t stub(const std::string& name, std::function<void()> fn);
  void linkImports(uint32_t base, uint32_t size);
  uint32_t arg(int i);
  void ret(uint32_t v);
  uint32_t rd32(uint32_t addr);
  void wr32(uint32_t addr, uint32_t v);
  std::string readString(uint32_t addr);
  uint64_t now() const;
  std::string format(uint32_t fmtAddr, int firstArg);

  int createThread(uint32_t entry, uint32_t stackSize, int prio, uint32_t gp);
  void startThread(int t, uint32_t a0, uint32_t a1);
  void block(Wait w);
  void makeReady(int t, uint32_t result);
  void reschedule();
  void switchTo(int next);
  int pickNext() const;
  bool runUntilIdle(uint64_t budget, int untilDone);
  uint32_t callGuest(const Interrupt& irq);
  void startRpcs();
  void checkTimers();
  void transferDone(int channel);

  Sif& sif_;
  Spu2& spu_;
  std::vector<Hle> hleFns_;  // by id - 1
  std::map<std::string, std::map<uint32_t, uint32_t>> hleIds_;  // library -> export index -> id
  std::map<std::string, uint32_t> exports_;                    // module library -> export table
  std::vector<Thread> threads_;
  std::vector<Sema> semas_;
  std::vector<Server> servers_;
  std::map<uint32_t, int> queueThread_;  // RPC queue (IOP address) -> serving thread
  std::deque<Request> requests_;
  std::vector<Timer> timers_;
  std::deque<Interrupt> interrupts_;
  std::vector<File> files_;
  struct Module {
    std::string name;
    uint32_t base, size;
  };
  std::vector<Module> modules_;
  uint32_t transHandler_[2] = {}, transArg_[2] = {}, transGp_[2] = {};
  uint64_t admaHalves_[2] = {};  // auto-DMA half buffers already reported to the handler
  Interrupt spuIrq_{};  // libsd SPU2 interrupt handler
  bool spuIrqOldStyle_ = false;
  int current_ = -1;
  bool inCallback_ = false, callbackDone_ = false;
  uint64_t readySeq_ = 0;
  uint32_t exitStub_ = 0, trampoline_ = 0, rpcReturn_ = 0, callbackStack_ = 0;
  uint64_t lastCycles_ = 0, spuCycles_ = 0;
};
