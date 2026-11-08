#pragma once
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Cpu;
class Iso9660;
class IopKernel;
class Spu2;

// The IOP side of the SIF (the EE <-> IOP link), replaced by C++. The game's own EE libraries
// talk to it exactly as they would to the real IOP: they DMA command packets into the IOP's
// command buffer, and get replies DMA'd into their receive buffer followed by a DMAC channel 5
// interrupt. RPC servers that IOP modules would provide are implemented as RpcServer classes.
class Sif {
public:
  static constexpr uint32_t kIopRamSize = 2 * 1024 * 1024;
  static constexpr uint32_t kCmdBuffer = 0x00010000;  // IOP address the EE sends commands to

  // An RPC server. `call` gets the data the EE sent (already copied into IOP RAM at `buffer`)
  // and returns the IOP address of the reply; `recvSize` bytes from there go back to the EE.
  class RpcServer {
  public:
    virtual ~RpcServer() = default;
    virtual const char* name() const = 0;
    virtual uint32_t call(Sif& sif, uint32_t fn, uint32_t buffer, uint32_t size) = 0;
    virtual void tick(Sif&) {}  // called every frame; for servers that push data to the EE
    uint32_t buffer = 0;  // receive buffer in IOP RAM, assigned at registration
  };

  Sif(Cpu& cpu, Iso9660& iso);
  ~Sif();

  uint32_t getReg(uint32_t reg);
  void setReg(uint32_t reg, uint32_t value);
  void dma(uint32_t eeSrc, uint32_t iopDst, uint32_t size);  // one EE -> IOP transfer
  void poll();  // runs the IOP and hands the next queued reply to the EE when its buffer is free
  void vblank();
  uint64_t nextEvent() const;  // when the IOP side next has something for the EE

  uint8_t* iop(uint32_t addr, uint32_t size);  // host pointer into IOP RAM (nullptr if out of range)
  uint32_t iopAlloc(uint32_t size);
  void unimplemented(const std::string& what);
  void log(const std::string& line);
  // Queues a SIF command packet for the EE; `before` runs right before it is delivered (to put
  // data in EE memory that the packet announces).
  void send(uint32_t cid, const std::vector<uint32_t>& payload, uint32_t opt = 0, std::function<void()> before = {});

  Cpu& cpu;
  Iso9660& iso;
  std::filesystem::path saveDir;    // host folder holding the memory card contents
  // The PS2 clock (sceCdReadClock) starts here and advances with emulated time, so a recorded
  // session replays identically.
  int64_t clockBase = 0;
  std::deque<std::string> recent;  // last SIF commands, oldest first
  std::unique_ptr<Spu2> spu;
  std::unique_ptr<IopKernel> iopKernel;  // runs the IOP modules the port executes (sound driver)

private:
  void command(uint32_t addr);
  void registerServer(uint32_t sid, std::unique_ptr<RpcServer> server);

  std::vector<uint8_t> ram_;
  uint32_t heap_ = 0x00020000;
  uint32_t eeBuffer_ = 0;             // EE address replies are written to
  uint64_t rebootAt_ = 0;             // cycle a requested IOP reboot completes, 0 if none
  std::map<uint32_t, uint32_t> regs_;  // SIF registers (MSCOM/SMCOM/flags and system registers)
  std::map<uint32_t, uint32_t> sregs_; // IOP-side software registers (SIF_CMD_SET_SREG)
  struct Outgoing {
    std::vector<uint32_t> packet;
    std::function<void()> before;
  };
  std::deque<Outgoing> outbox_;
  std::map<uint32_t, std::unique_ptr<RpcServer>> servers_;  // by RPC server id (sid)
  std::map<uint32_t, uint32_t> serverIds_;                  // server handle (IOP address) -> sid
};
