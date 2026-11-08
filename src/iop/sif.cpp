#include "iop/sif.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "ee/cpu.h"
#include "iop/cdvd.h"
#include "iop/fileio.h"
#include "iop/iopkernel.h"
#include "iop/modules.h"
#include "iop/memcard.h"
#include "iop/pad.h"
#include "iop/sound.h"
#include "iop/spu2.h"
#include "iso9660.h"

namespace {
enum : uint32_t {
  kCmdChangeSaddr = 0x80000000,
  kCmdSetSreg = 0x80000001,
  kCmdInit = 0x80000002,
  kCmdReset = 0x80000003,
  kCmdRpcEnd = 0x80000008,
  kCmdRpcBind = 0x80000009,
  kCmdRpcCall = 0x8000000A,
  kCmdRpcRdata = 0x8000000C,
};
constexpr size_t kRecent = 4096;
constexpr uint32_t kSifStatusBooted = 0x10000 | 0x20000 | 0x40000;  // SIFINIT | CMDINIT | BOOTEND

// A server some IOP module would provide but the port does not implement yet: binding works
// (so the game proceeds to its first call), and the first call stops the run with its details.
class MissingServer : public Sif::RpcServer {
public:
  explicit MissingServer(uint32_t sid) : sid_(sid) {}
  const char* name() const override { return "missing"; }
  uint32_t call(Sif& sif, uint32_t fn, uint32_t buffer, uint32_t size) override {
    std::string bytes;
    const uint8_t* p = sif.iop(buffer, std::min<uint32_t>(size, 32));
    for (uint32_t i = 0; p && i < std::min<uint32_t>(size, 32); ++i) bytes += std::format("{:02X}", p[i]);
    sif.unimplemented(std::format("RPC server 0x{:08X} function {} ({} bytes: {})", sid_, fn, size, bytes));
    return buffer;
  }

private:
  uint32_t sid_;
};
}  // namespace

Sif::Sif(Cpu& c, Iso9660& i) : cpu(c), iso(i), ram_(kIopRamSize) {
  regs_[2] = kCmdBuffer;         // SIF_REG_SUBADDR: where the IOP receives commands
  regs_[4] = kSifStatusBooted;   // SIF_REG_SMFLAG
  spu = std::make_unique<Spu2>();
  iopKernel = std::make_unique<IopKernel>(*this, ram_, *spu);
  cpu.mem.hw.eventSources.push_back([this] { return nextEvent(); });
}

Sif::~Sif() = default;

uint8_t* Sif::iop(uint32_t addr, uint32_t size) {
  addr &= 0x1FFFFF;  // IOP RAM is mirrored; addresses arrive with kseg bits from libraries
  if (uint64_t(addr) + size > ram_.size()) return nullptr;
  return ram_.data() + addr;
}

uint32_t Sif::iopAlloc(uint32_t size) {
  const uint32_t addr = heap_;
  heap_ = (heap_ + size + 63) & ~63u;
  if (heap_ > kIopRamSize) unimplemented(std::format("IOP memory exhausted ({} bytes requested)", size));
  return addr;
}

void Sif::unimplemented(const std::string& what) { cpu.halt(StopReason::Unimplemented, what); }

void Sif::log(const std::string& line) {
  if (recent.size() == kRecent) recent.pop_front();
  recent.push_back(line);
}

uint32_t Sif::getReg(uint32_t reg) { return regs_[reg]; }
void Sif::setReg(uint32_t reg, uint32_t value) {
  if (reg == 4)  // SMFLAG: written by the IOP, the EE can only clear bits
    regs_[4] &= ~value;
  else
    regs_[reg] = value;
}

void Sif::registerServer(uint32_t sid, std::unique_ptr<RpcServer> server) {
  server->buffer = iopAlloc(0x2000);  // requests are at most about 1 KB
  serverIds_[server->buffer] = sid;
  servers_[sid] = std::move(server);
}

void Sif::dma(uint32_t eeSrc, uint32_t iopDst, uint32_t size) {
  const uint8_t* src = cpu.mem.hostPtr(eeSrc, size);
  uint8_t* dst = iop(iopDst, size);
  if (!src || !dst) {
    unimplemented(std::format("SIF DMA 0x{:08X} -> iop 0x{:08X} ({} bytes) out of range", eeSrc, iopDst, size));
    return;
  }
  std::memcpy(dst, src, size);
  if ((iopDst & 0x1FFFFF) == kCmdBuffer) command(kCmdBuffer);
}

void Sif::send(uint32_t cid, const std::vector<uint32_t>& payload, uint32_t opt, std::function<void()> before) {
  std::vector<uint32_t> packet = {uint32_t(16 + payload.size() * 4), 0, cid, opt};
  packet.insert(packet.end(), payload.begin(), payload.end());
  outbox_.push_back({std::move(packet), std::move(before)});
}

void Sif::poll() {
  // A rebooted IOP comes back a little later and announces itself through SMFLAG; by then the
  // EE has finished clearing the flags it watches.
  if (rebootAt_ && cpu.mem.hw.cycles >= rebootAt_) {
    rebootAt_ = 0;
    regs_[4] = kSifStatusBooted;
    log("IOP reboot finished");
  }
  iopKernel->advance();
  if (outbox_.empty() || !eeBuffer_) return;
  uint8_t* buf = cpu.mem.hostPtr(eeBuffer_, 128);
  if (!buf || buf[0] != 0) return;  // the EE clears the size byte once it has taken a packet
  Outgoing out = std::move(outbox_.front());
  outbox_.pop_front();
  if (out.before) out.before();
  std::memcpy(buf, out.packet.data(), out.packet.size() * 4);
  cpu.mem.hw.dmacStat |= 1u << 5;  // SIF0 (IOP -> EE) transfer complete
}

void Sif::vblank() {
  iopKernel->onVblank();
  for (auto& [sid, server] : servers_) server->tick(*this);
}

uint64_t Sif::nextEvent() const {
  if (!outbox_.empty() && eeBuffer_) return cpu.mem.hw.cycles;
  return iopKernel->nextEvent();
}

void Sif::command(uint32_t addr) {
  const uint8_t* p = iop(addr, 16);
  uint32_t h[16] = {};
  std::memcpy(h, p, std::min<size_t>(64, p[0] ? p[0] : 16));
  const uint32_t cid = h[2], opt = h[3];

  switch (cid) {
    case kCmdChangeSaddr:
      eeBuffer_ = h[4];
      log(std::format("CHANGE_SADDR ee buffer 0x{:08X}", h[4]));
      return;
    case kCmdSetSreg:
      sregs_[h[4]] = h[5];
      log(std::format("SET_SREG {} = 0x{:X}", h[4], h[5]));
      return;
    case kCmdInit:
      if (opt == 0) {  // sceSifInitCmd: tells the IOP where the EE receives packets
        eeBuffer_ = h[4];
        log(std::format("INIT_CMD ee buffer 0x{:08X}", h[4]));
      } else {  // sceSifInitRpc: the IOP answers by setting software register 0
        log("INIT_CMD rpc");
        send(kCmdSetSreg, {0, 1});
      }
      return;
    case kCmdRpcBind: {  // rec_id, pkt_addr, rpc_id, client, sid
      const uint32_t sid = h[8];
      uint32_t handle = 0, buffer = 0;
      if (iopKernel->findServer(sid, handle, buffer)) {  // registered by a module on the IOP
        log(std::format("RPC_BIND 0x{:08X} (IOP module)", sid));
        send(kCmdRpcEnd, {h[4], h[5], h[6], h[7], kCmdRpcBind, handle, buffer, buffer});
        return;
      }
      if (!servers_.count(sid)) {
        std::unique_ptr<RpcServer> server = makeCdvdServer(sid);
        if (!server) server = makeSystemServer(sid);
        if (!server) server = makeSoundServer(sid, *spu);
        if (!server) server = makeMemcardServer(sid, saveDir);
        if (!server) server = makePadServer(sid);
        if (!server) server = makeFileioServer(sid);
        registerServer(sid, server ? std::move(server) : std::make_unique<MissingServer>(sid));
      }
      RpcServer& server = *servers_[sid];
      log(std::format("RPC_BIND 0x{:08X} ({})", sid, server.name()));
      // rec_id, pkt_addr, rpc_id, client, cid, server handle, buff, cbuff
      send(kCmdRpcEnd, {h[4], h[5], h[6], h[7], kCmdRpcBind, server.buffer, server.buffer, server.buffer});
      return;
    }
    case kCmdRpcCall: {  // rec_id, pkt_addr, rpc_id, client, rpc_number, send_size, receive, recv_size, rmode, server
      const uint32_t fn = h[8], sendSize = h[9], receive = h[10], recvSize = h[11], serverHandle = h[13];
      auto id = serverIds_.find(serverHandle & 0x1FFFFF);
      if (id == serverIds_.end() && iopKernel->callServer(serverHandle, fn, sendSize, [this, h, receive, recvSize](uint32_t reply) {
            if (recvSize && receive) {
              uint8_t* dst = cpu.mem.hostPtr(receive, recvSize);
              const uint8_t* src = iop(reply, recvSize);
              if (dst && src) std::memcpy(dst, src, recvSize);
            }
            send(kCmdRpcEnd, {h[4], h[5], h[6], h[7], kCmdRpcCall});
          })) {
        log(std::format("RPC_CALL IOP server 0x{:06X} fn 0x{:X} send {} recv {}", serverHandle, fn, sendSize, recvSize));
        return;
      }
      if (id == serverIds_.end()) {
        unimplemented(std::format("RPC call to unknown server handle 0x{:08X}", serverHandle));
        return;
      }
      RpcServer& server = *servers_[id->second];
      log(std::format("RPC_CALL 0x{:08X} fn {} send {} recv {}", id->second, fn, sendSize, recvSize));
      const uint32_t reply = server.call(*this, fn, server.buffer, sendSize);
      if (recvSize && receive) {
        uint8_t* dst = cpu.mem.hostPtr(receive, recvSize);
        const uint8_t* src = iop(reply, recvSize);
        if (dst && src) std::memcpy(dst, src, recvSize);
      }
      send(kCmdRpcEnd, {h[4], h[5], h[6], h[7], kCmdRpcCall});
      return;
    }
    case kCmdRpcRdata: {  // rec_id, pkt_addr, rpc_id, receive(client), src (IOP), dest (EE), size
      const uint32_t src = h[8], dest = h[9], size = h[10];
      uint8_t* d = cpu.mem.hostPtr(dest, size);
      const uint8_t* s = iop(src, size);
      if (d && s) std::memcpy(d, s, size);
      log(std::format("RPC_RDATA iop 0x{:08X} -> ee 0x{:08X} ({} bytes)", src, dest, size));
      send(kCmdRpcEnd, {h[4], h[5], h[6], h[7], kCmdRpcRdata});
      return;
    }
  }
  if (cid == kCmdReset) {  // sceSifRebootIop: {size, path...}; the replacement IOP needs no image
    const char* arg = reinterpret_cast<const char*>(iop(addr + 24, 80));
    log(std::format("RESET \"{}\"", std::string(arg, strnlen(arg, 79))));
    outbox_.clear();
    eeBuffer_ = 0;
    rebootAt_ = cpu.mem.hw.cycles + 300'000;  // about 1 ms
    return;
  }
  unimplemented(std::format("SIF command 0x{:08X} (opt 0x{:X})", cid, opt));
}
