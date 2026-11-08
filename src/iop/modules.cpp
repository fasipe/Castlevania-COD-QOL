#include "iop/modules.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "iop/iopkernel.h"
#include "iso9660.h"

namespace {
bool runsOnIop(const std::string& path) {
  for (const char* name : {"LIBSMF2.IRX", "SDSTR3.IRX", "MODSD.IRX"})
    if (path.find(name) != std::string::npos) return true;
  return false;
}

uint32_t rd32(Sif& sif, uint32_t iopAddr) {
  uint32_t v = 0;
  if (const uint8_t* p = sif.iop(iopAddr, 4)) std::memcpy(&v, p, 4);
  return v;
}

std::string str(Sif& sif, uint32_t iopAddr, uint32_t max) {
  const char* p = reinterpret_cast<const char*>(sif.iop(iopAddr, max));
  return p ? std::string(p, strnlen(p, max)) : std::string();
}

uint32_t reply(Sif& sif, uint32_t buffer, std::initializer_list<uint32_t> words) {
  uint8_t* p = sif.iop(buffer, uint32_t(words.size() * 4));
  for (uint32_t w : words) {
    std::memcpy(p, &w, 4);
    p += 4;
  }
  return buffer;
}

// LOADFILE (rom0 module): sceSifLoadModule and friends. The game's sound driver and the
// libraries it links with run on the emulated IOP; the services of the other modules are C++
// RpcServers, so loading them only records the request. A module the game loads from the disc
// must exist there, which catches path mistakes early.
class LoadfileServer : public Sif::RpcServer {
public:
  const char* name() const override { return "loadfile"; }
  uint32_t call(Sif& sif, uint32_t fn, uint32_t buf, uint32_t) override {
    switch (fn) {
      case 0:      // load module from a device: {argLen, -, path[0xFC] at +8, args[0xFC] at +0x104}
      case 1: {    // load ELF
        const std::string path = str(sif, buf + 8, 0xFC);
        const bool onDisc = path.rfind("cdrom0:", 0) == 0;
        if (onDisc && !sif.iso.find(path)) {
          sif.log(std::format("loadModule \"{}\": not on the disc", path));
          return reply(sif, buf, {0xFFFFFE6E, 0});  // -KE_FILEERR
        }
        if (onDisc && runsOnIop(path)) {
          std::string error;
          if (!sif.iopKernel->loadModule(sif.iso.readFile(*sif.iso.find(path)), path, error)) {
            sif.unimplemented(std::format("loading {} on the IOP: {}", path, error));
            return reply(sif, buf, {0xFFFFFE6E, 0});
          }
        }
        sif.log(std::format("loadModule \"{}\" -> id {}", path, nextId_));
        return reply(sif, buf, {nextId_++, 0});
      }
      case 0xFF:  // version: four characters the EE library compares with "3000"
        return reply(sif, buf, {'3' | '0' << 8 | '0' << 16 | uint32_t('0') << 24});
    }
    sif.unimplemented(std::format("loadfile function {}", fn));
    return buf;
  }

private:
  uint32_t nextId_ = 0x20;
};

// IOPHEAP (rom0 module): IOP memory for the EE to DMA into.
class IopHeapServer : public Sif::RpcServer {
public:
  const char* name() const override { return "iopheap"; }
  uint32_t call(Sif& sif, uint32_t fn, uint32_t buf, uint32_t) override {
    switch (fn) {
      case 1: {  // alloc(size)
        const uint32_t size = rd32(sif, buf);
        const uint32_t addr = sif.iopAlloc(size);
        sif.log(std::format("iopAlloc {} -> 0x{:06X}", size, addr));
        return reply(sif, buf, {addr});
      }
      case 2:  // free(addr): the bump allocator never reuses memory
        return reply(sif, buf, {0});
      case 3: {  // load a file into IOP memory: {addr, path...}
        const uint32_t addr = rd32(sif, buf);
        const std::string path = str(sif, buf + 4, 0xFC);
        const auto entry = sif.iso.find(path);
        uint8_t* dst = entry ? sif.iop(addr, entry->size) : nullptr;
        if (!dst) {
          sif.log(std::format("iopLoadHeap \"{}\" failed", path));
          return reply(sif, buf, {0xFFFFFFFF});
        }
        const auto data = sif.iso.readFile(*entry);
        std::memcpy(dst, data.data(), data.size());
        sif.log(std::format("iopLoadHeap \"{}\" -> 0x{:06X}", path, addr));
        return reply(sif, buf, {0});
      }
    }
    sif.unimplemented(std::format("iopheap function {}", fn));
    return buf;
  }
};
}  // namespace

std::unique_ptr<Sif::RpcServer> makeSystemServer(uint32_t sid) {
  switch (sid) {
    case kLoadfileServer: return std::make_unique<LoadfileServer>();
    case kIopHeapServer: return std::make_unique<IopHeapServer>();
  }
  return nullptr;
}
