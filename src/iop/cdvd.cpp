#include "iop/cdvd.h"

#include "ee/hardware.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <format>

#include "ee/cpu.h"
#include "iso9660.h"

namespace {
constexpr uint32_t kComplete = 2;     // SCECdComplete: drive ready
constexpr uint32_t kDiskPs2Dvd = 0x14;
constexpr uint32_t kStatPause = 0x0A;

uint32_t rd32(Sif& sif, uint32_t iopAddr) {
  uint32_t v = 0;
  if (const uint8_t* p = sif.iop(iopAddr, 4)) std::memcpy(&v, p, 4);
  return v;
}

// Writes `words` at the start of the server's reply area and returns its IOP address.
uint32_t reply(Sif& sif, uint32_t buffer, std::initializer_list<uint32_t> words) {
  uint8_t* p = sif.iop(buffer, uint32_t(words.size() * 4));
  for (uint32_t w : words) {
    std::memcpy(p, &w, 4);
    p += 4;
  }
  return buffer;
}

uint8_t bcd(int v) { return uint8_t((v / 10) << 4 | (v % 10)); }

class CdvdServer : public Sif::RpcServer {
public:
  explicit CdvdServer(uint32_t sid) : sid_(sid) {}
  const char* name() const override { return "cdvdfsv"; }

  uint32_t call(Sif& sif, uint32_t fn, uint32_t buf, uint32_t size) override {
    switch (sid_) {
      case kCdInitServer:  // sceCdInit(mode): byte 12 != 0xFF, both versions >= 0x200
        return reply(sif, buf, {1, 0x200, 0x200, 0});
      case kCdDiskReadyServer:
      case kCdDiskReady2Server:
        return reply(sif, buf, {kComplete});
      case kCdSearchFileServer:
        return searchFile(sif, buf);
      case kCdNCmdServer:
        return ncmd(sif, fn, buf);
      case kCdSCmdServer:
        return scmd(sif, fn, buf);
    }
    sif.unimplemented(std::format("cdvdfsv server 0x{:08X} function {} ({} bytes)", sid_, fn, size));
    return buf;
  }

private:
  // Request: sceCdlFILE (32 bytes) to fill, path at +0x24, EE address of that sceCdlFILE at
  // +0x124. The result is written back to the EE directly; the reply word is 1 if found.
  uint32_t searchFile(Sif& sif, uint32_t buf) {
    const char* path = reinterpret_cast<const char*>(sif.iop(buf + 0x24, 256));
    const std::string name(path, strnlen(path, 255));
    const uint32_t eeFile = rd32(sif, buf + 0x124);
    const auto entry = sif.iso.find(name);
    if (entry) {
      uint8_t* f = sif.cpu.mem.hostPtr(eeFile, 32);
      if (f) {
        std::memset(f, 0, 32);
        std::memcpy(f + 0, &entry->lba, 4);
        std::memcpy(f + 4, &entry->size, 4);
        const std::string base = entry->path.substr(entry->path.find_last_of('/') + 1);
        std::memcpy(f + 8, base.data(), std::min<size_t>(base.size(), 15));
      }
    }
    sif.log(std::format("cdSearchFile \"{}\" -> {}", name, entry ? std::format("lsn {} size {}", entry->lba, entry->size) : "not found"));
    return reply(sif, buf, {entry ? 1u : 0u});
  }

  uint32_t ncmd(Sif& sif, uint32_t fn, uint32_t buf) {
    switch (fn) {
      case 1: {  // sceCdRead: lsn, sectors, EE buffer, mode[3], EE partial-sector struct, EE flag
        const uint32_t lsn = rd32(sif, buf), sectors = rd32(sif, buf + 4), dst = rd32(sif, buf + 8);
        const uint8_t pattern = *sif.iop(buf + 14, 1);
        const uint32_t partial = rd32(sif, buf + 16);
        if (pattern != 0) {
          sif.unimplemented(std::format("sceCdRead with data pattern {}", pattern));
          return buf;
        }
        uint8_t* out = sif.cpu.mem.hostPtr(dst, sectors * Iso9660::kSectorSize);
        if (!out || !sif.iso.readSectors(lsn, sectors, out)) {
          sif.unimplemented(std::format("sceCdRead lsn {} x{} to 0x{:08X} failed", lsn, sectors, dst));
          return buf;
        }
        // The whole transfer went straight to the destination, so no partial-sector bytes are
        // left for the EE end callback to copy.
        if (uint8_t* p = sif.cpu.mem.hostPtr(partial, 16)) std::memset(p, 0, 16);
        ++reads;
        sectorsRead += sectors;
        sif.log(std::format("cdRead lsn {} x{} -> 0x{:08X}", lsn, sectors, dst));
        return buf;
      }
      case 4: {  // sceCdGetToc: TOC to the EE buffer at +0; reply {result, isDvd}
        const uint32_t dst = rd32(sif, buf);
        if (uint8_t* t = sif.cpu.mem.hostPtr(dst, 0x810)) {  // single-layer PS2 DVD
          std::memset(t, 0, 0x810);
          const uint8_t head[] = {0x04, 0x02, 0xF2, 0x00, 0x86, 0x72};
          std::memcpy(t, head, sizeof(head));
          t[17] = 0x03;
        }
        return reply(sif, buf, {1, 1});
      }
      case 5: case 6: case 7: case 8:  // Seek, Standby, Stop, Pause: instant
        return buf;
      case 9:  // streaming (sceCdSt*): {lsn, sectors, EE buffer, command, mode}; reply {result}
        return reply(sif, buf, {stream(sif, rd32(sif, buf + 12), rd32(sif, buf), rd32(sif, buf + 4), rd32(sif, buf + 8))});
      case 0xE:  // drive ready check
        return reply(sif, buf, {kComplete});
    }
    sif.unimplemented(std::format("cdvd N-command {}", fn));
    return buf;
  }

  // The disc stream used for movies. The drive never falls behind: every requested sector is
  // available at once, read straight from the ISO into the EE buffer.
  uint32_t stream(Sif& sif, uint32_t cmd, uint32_t lsn, uint32_t sectors, uint32_t dst) {
    enum { kStart = 1, kRead, kStop, kSeek, kInit, kStat, kPause, kResume, kSeekF };
    switch (cmd) {
      case kInit:  // lsn = ring buffer sectors, sectors = banks, dst = IOP ring buffer
        streamBuffer_ = lsn;
        sif.log(std::format("cdStInit buffer {} sectors, {} banks", lsn, sectors));
        return 1;
      case kStart: case kSeek: case kSeekF:
        streamPos_ = lsn;
        streaming_ = true;
        sif.log(std::format("cdSt{} lsn {}", cmd == kStart ? "Start" : "Seek", lsn));
        return 1;
      case kRead: {
        uint8_t* out = sif.cpu.mem.hostPtr(dst, sectors * Iso9660::kSectorSize);
        if (!streaming_ || !out || !sif.iso.readSectors(streamPos_, sectors, out)) return 0;
        streamPos_ += sectors;
        sectorsRead += sectors;
        return sectors;  // error code in the upper half: none
      }
      case kStop: streaming_ = false; return 1;
      case kStat: return streaming_ ? streamBuffer_ : 0;  // sectors buffered
      case kPause: case kResume: return 1;
    }
    sif.unimplemented(std::format("cdvd stream command {} (lsn {}, {} sectors)", cmd, lsn, sectors));
    return 0;
  }

  uint32_t streamPos_ = 0, streamBuffer_ = 0;
  bool streaming_ = false;

  uint32_t scmd(Sif& sif, uint32_t fn, uint32_t buf) {
    switch (fn) {
      case 0x01: {  // ReadClock: result, then sceCdCLOCK {stat, sec, min, hour, pad, day, month, year} in BCD
        const std::time_t now = std::time_t(sif.clockBase + int64_t(sif.cpu.mem.hw.cycles / Hardware::kEeHz));
        std::tm t{};
        localtime_s(&t, &now);
        const uint8_t clock[8] = {0, bcd(t.tm_sec), bcd(t.tm_min), bcd(t.tm_hour), 0, bcd(t.tm_mday), bcd(t.tm_mon + 1), bcd(t.tm_year % 100)};
        uint32_t w[2];
        std::memcpy(w, clock, 8);
        return reply(sif, buf, {1, w[0], w[1], 0});
      }
      case 0x03: return reply(sif, buf, {kDiskPs2Dvd});  // GetDiskType
      case 0x04: return reply(sif, buf, {0});            // GetError: none
      case 0x0C: return reply(sif, buf, {kStatPause});   // Status
      case 0x16: return reply(sif, buf, {1});            // Break
      case 0x22: return reply(sif, buf, {1});            // Mmode (select CD/DVD media)
    }
    sif.unimplemented(std::format("cdvd S-command 0x{:X}", fn));
    return buf;
  }

  uint32_t sid_;

public:
  uint64_t reads = 0, sectorsRead = 0;
};
}  // namespace

std::unique_ptr<Sif::RpcServer> makeCdvdServer(uint32_t sid) {
  switch (sid) {
    case kCdInitServer: case kCdSCmdServer: case kCdNCmdServer:
    case kCdSearchFileServer: case kCdDiskReadyServer: case kCdDiskReady2Server:
      return std::make_unique<CdvdServer>(sid);
  }
  return nullptr;
}
