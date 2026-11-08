#include "iop/fileio.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

#include "ee/cpu.h"
#include "iso9660.h"

namespace {
// The game's stdio library (0x007633F0 onwards) calls FILEIO asynchronously: the RPC only says
// "accepted" (reply word 1), and the result arrives later as a completion record the IOP writes
// to EE memory at 0x00B9AA00, announced by SIF command 0x80000011 (handler 0x00762EB8):
//   record: {semaphore, function, EE result address, result size, result...}
// Requests: {semaphore, EE result address, result size, fd, ...function arguments}.
enum : uint32_t { kOpen = 0, kClose = 1, kRead = 2, kWrite = 3, kLseek = 4, kIoctl = 5, kInit = 0xFF };
constexpr uint32_t kCompletionCmd = 0x80000011;
constexpr uint32_t kRecordBuffer = 0x00B9AA00;
constexpr int32_t kENOENT = -2, kEBADF = -9, kEINVAL = -22, kEROFS = -30;

uint32_t rd32(Sif& sif, uint32_t a) {
  uint32_t v = 0;
  std::memcpy(&v, sif.iop(a, 4), 4);
  return v;
}

class FileioServer : public Sif::RpcServer {
public:
  const char* name() const override { return "fileio"; }

  uint32_t call(Sif& sif, uint32_t fn, uint32_t buf, uint32_t size) override {
    if (fn == kInit) {  // {version "3000", single completion buffer}
      const uint32_t reply[2] = {'3' | '0' << 8 | '0' << 16 | uint32_t('0') << 24, 0};
      std::memcpy(sif.iop(buf, 8), reply, 8);
      return buf;
    }
    const uint32_t sema = rd32(sif, buf), resultAddr = rd32(sif, buf + 4), resultSize = rd32(sif, buf + 8);
    const uint32_t fd = rd32(sif, buf + 12);
    int32_t result;
    switch (fn) {
      case kOpen: {
        const char* p = reinterpret_cast<const char*>(sif.iop(buf + 0x14, 0x400));
        const std::string path(p, strnlen(p, 0x3FF));
        result = open(sif, path, rd32(sif, buf + 12));
        sif.log(std::format("fioOpen \"{}\" -> {}", path, result));
        break;
      }
      case kClose:
        result = fd < files_.size() && files_[fd].open ? (files_[fd].open = false, 0) : kEBADF;
        break;
      case kRead: result = read(sif, fd, rd32(sif, buf + 16), rd32(sif, buf + 20)); break;
      case kWrite: result = kEROFS; break;  // the disc is read-only and there is no host device
      case kLseek: result = seek(fd, int32_t(rd32(sif, buf + 16)), rd32(sif, buf + 20)); break;
      case kIoctl: result = 0; break;
      default:
        sif.unimplemented(std::format("fileio function {} ({} bytes)", fn, size));
        return buf;
    }
    // Accepted now; the result follows as a completion record.
    const uint32_t accepted = 1;
    std::memcpy(sif.iop(buf, 4), &accepted, 4);
    Cpu& cpu = sif.cpu;
    sif.send(kCompletionCmd, {}, 0, [&cpu, sema, fn, resultAddr, resultSize, result] {
      uint32_t rec[8] = {sema, fn, resultAddr, std::min<uint32_t>(resultSize, 4), uint32_t(result), 0, 0, 0};
      if (uint8_t* p = cpu.mem.hostPtr(kRecordBuffer, sizeof(rec))) std::memcpy(p, rec, sizeof(rec));
    });
    return buf;
  }

private:
  struct File {
    bool open = false;
    Iso9660::Entry entry;
    uint32_t pos = 0;
  };

  int32_t open(Sif& sif, const std::string& path, uint32_t flags) {
    if (flags & 2) return kEROFS;  // write access
    if (path.rfind("cdrom0:", 0) != 0 && path.rfind("cdrom:", 0) != 0) return kENOENT;
    const auto entry = sif.iso.find(path);
    if (!entry) return kENOENT;
    auto it = std::find_if(files_.begin(), files_.end(), [](const File& f) { return !f.open; });
    if (it == files_.end()) {
      files_.push_back({});
      it = files_.end() - 1;
    }
    *it = {true, *entry, 0};
    return int32_t(it - files_.begin());
  }

  int32_t read(Sif& sif, uint32_t fd, uint32_t dst, uint32_t size) {
    if (fd >= files_.size() || !files_[fd].open) return kEBADF;
    File& f = files_[fd];
    size = std::min(size, f.entry.size - std::min(f.pos, f.entry.size));
    if (size == 0) return 0;
    uint8_t* out = sif.cpu.mem.hostPtr(dst, size);
    if (!out) return kEINVAL;
    // Read whole sectors around the requested range, then copy the part asked for.
    const uint32_t first = f.pos / Iso9660::kSectorSize, last = (f.pos + size - 1) / Iso9660::kSectorSize;
    std::vector<uint8_t> tmp(size_t(last - first + 1) * Iso9660::kSectorSize);
    if (!sif.iso.readSectors(f.entry.lba + first, last - first + 1, tmp.data())) return kEINVAL;
    std::memcpy(out, tmp.data() + f.pos % Iso9660::kSectorSize, size);
    f.pos += size;
    return int32_t(size);
  }

  int32_t seek(uint32_t fd, int32_t offset, uint32_t whence) {
    if (fd >= files_.size() || !files_[fd].open) return kEBADF;
    File& f = files_[fd];
    const int64_t base = whence == 0 ? 0 : whence == 1 ? int64_t(f.pos) : int64_t(f.entry.size);
    const int64_t pos = base + offset;
    if (pos < 0) return kEINVAL;
    f.pos = uint32_t(pos);
    return int32_t(f.pos);
  }

  std::vector<File> files_;
};
}  // namespace

std::unique_ptr<Sif::RpcServer> makeFileioServer(uint32_t sid) {
  if (sid == kFileioServer) return std::make_unique<FileioServer>();
  return nullptr;
}
