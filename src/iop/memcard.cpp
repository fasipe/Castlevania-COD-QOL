#include "iop/memcard.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <vector>

#include "ee/cpu.h"

namespace fs = std::filesystem;

namespace {
// libmc result codes
constexpr int32_t kOk = 0, kNewCard = -1, kFull = -3, kNoEntry = -4, kDenied = -5, kNotEmpty = -6,
                  kTooManyHandles = -7, kNoCard = -10;
constexpr uint32_t kCardType = 2;            // PS2 memory card
constexpr uint32_t kCardClusters = 8000;     // usable 1 KB clusters on an 8 MB card
constexpr uint32_t kOpenWrite = 2, kOpenCreate = 0x200, kOpenDir = 0x40;
constexpr uint16_t kAttrFile = 0x8497, kAttrDir = 0x8427;
constexpr uint32_t kRenameFlag = 0x10;

uint32_t rd32(Sif& sif, uint32_t iopAddr) {
  uint32_t v = 0;
  if (const uint8_t* p = sif.iop(iopAddr, 4)) std::memcpy(&v, p, 4);
  return v;
}

std::string str(const uint8_t* p, size_t max) {
  return p ? std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), max)) : "";
}

bool wildcard(const char* pat, const char* s) {
  if (*pat == 0) return *s == 0;
  if (*pat == '*') return wildcard(pat + 1, s) || (*s && wildcard(pat, s + 1));
  if (*s && (*pat == '?' || *pat == *s)) return wildcard(pat + 1, s + 1);
  return false;
}

// sceMcStDateTime {resv, sec, min, hour, day, month, u16 year}
void dateTime(uint8_t* out, fs::file_time_type t) {
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
  std::tm tm{};
  localtime_s(&tm, &tt);
  const uint16_t year = uint16_t(tm.tm_year + 1900);
  const uint8_t v[6] = {0, uint8_t(tm.tm_sec), uint8_t(tm.tm_min), uint8_t(tm.tm_hour), uint8_t(tm.tm_mday), uint8_t(tm.tm_mon + 1)};
  std::memcpy(out, v, 6);
  std::memcpy(out + 6, &year, 2);
}

class McServer : public Sif::RpcServer {
public:
  explicit McServer(fs::path root) : root_(std::move(root)) {}
  const char* name() const override { return "mcserv"; }

  uint32_t call(Sif& sif, uint32_t fn, uint32_t buf, uint32_t size) override {
    int32_t r;
    switch (fn) {
      case 0xFE: {  // init: {result, mcserv version, mcman version}
        const uint32_t v[3] = {0, 0x20A, 0x20E};
        std::memcpy(sif.iop(buf, 12), v, 12);
        return buf;
      }
      case 0x01: r = getInfo(sif, buf); break;
      case 0x02: r = open(sif, buf); break;
      case 0x03: r = close(rd32(sif, buf)); break;
      case 0x04: r = seek(rd32(sif, buf), int32_t(rd32(sif, buf + 0x10)), rd32(sif, buf + 0x14)); break;
      case 0x05: r = read(sif, buf); break;
      case 0x06: r = write(sif, buf); break;
      case 0x0A: r = kOk; break;  // flush: files are written through
      case 0x0D: r = getDir(sif, buf); break;
      case 0x0E: r = setInfo(sif, buf); break;
      case 0x0F: r = remove(sif, buf); break;
      case 0x10: r = format(sif, buf); break;
      case 0x11: r = kOk; break;  // unformat: keep the files
      case 0x12: r = entrySpace(sif, buf); break;
      case 0x15: case 0x35: r = kOk; break;  // thread priority / housekeeping
      default:
        sif.unimplemented(std::format("mcserv function 0x{:X} ({} bytes)", fn, size));
        return buf;
    }
    std::memcpy(sif.iop(buf, 4), &r, 4);
    return buf;
  }

private:
  bool present(uint32_t port) const { return port == 0; }
  fs::path card() const { return root_ / "card0"; }

  fs::path hostPath(const std::string& cardPath) {
    std::string p = cardPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.empty() || p[0] != '/') p = cwd_ + "/" + p;
    fs::path out = card();
    size_t start = 0;
    while (start < p.size()) {  // walk components; ".." cannot leave the card
      const size_t end = std::min(p.find('/', start), p.size());
      const std::string part = p.substr(start, end - start);
      if (part == "..") {
        if (out != card()) out = out.parent_path();
      } else if (!part.empty() && part != ".") {
        out /= part;
      }
      start = end + 1;
    }
    return out;
  }

  uint32_t freeClusters() {
    std::error_code ec;
    uint64_t used = 0;
    for (auto& e : fs::recursive_directory_iterator(card(), ec))
      used += e.is_regular_file() ? (e.file_size() + 1023) / 1024 + 1 : 2;
    return used >= kCardClusters ? 0 : uint32_t(kCardClusters - used);
  }

  // {-, port, slot, wantFormat, wantFree, wantType, -, EE result block}; the block gets
  // type at +0, free clusters at +4 and the format flag at +0x90.
  int32_t getInfo(Sif& sif, uint32_t buf) {
    const uint32_t port = rd32(sif, buf + 4), eeOut = rd32(sif, buf + 0x1C);
    uint8_t* out = sif.cpu.mem.hostPtr(eeOut, 0x94);
    const bool have = present(port);
    if (have) fs::create_directories(card());
    const uint32_t type = have ? kCardType : 0, free = have ? freeClusters() : 0, formatted = have ? 1 : 0;
    if (out) {
      std::memcpy(out, &type, 4);
      std::memcpy(out + 4, &free, 4);
      std::memcpy(out + 0x90, &formatted, 4);
    }
    int32_t result = kOk;
    if (!have) {
      result = kNoCard;
    } else if (!seen_) {
      seen_ = true;
      result = kNewCard;  // first look at a formatted card
    }
    sif.log(std::format("mcGetInfo port {} slot {} -> {} (type {}, free {}, formatted {}) block 0x{:08X}", port,
                        rd32(sif, buf + 8), result, type, free, formatted, eeOut));
    return result;
  }

  // {port, slot, mode, -, -, name[0x400]}
  int32_t open(Sif& sif, uint32_t buf) {
    if (!present(rd32(sif, buf))) return kNoCard;
    const uint32_t mode = rd32(sif, buf + 8);
    const std::string name = str(sif.iop(buf + 0x14, 0x400), 0x3FF);
    const fs::path path = hostPath(name);
    std::error_code ec;
    if (mode & kOpenDir) {  // sceMcMkdir
      if (fs::exists(path, ec)) return kDenied;
      if (!fs::exists(path.parent_path(), ec)) return kNoEntry;
      fs::create_directory(path, ec);
      sif.log(std::format("mcMkdir \"{}\"", name));
      return ec ? kFull : kOk;
    }
    if (fs::is_directory(path, ec)) return kDenied;
    if (!fs::exists(path, ec)) {
      if (!(mode & kOpenCreate)) return kNoEntry;
      if (!fs::exists(path.parent_path(), ec)) return kNoEntry;
      std::ofstream(path, std::ios::binary).close();
    }
    auto it = std::find_if(files_.begin(), files_.end(), [](const auto& f) { return !f.open; });
    if (it == files_.end()) {
      if (files_.size() >= 16) return kTooManyHandles;
      files_.push_back({});
      it = files_.end() - 1;
    }
    *it = {true, path, 0, (mode & kOpenWrite) != 0};
    sif.log(std::format("mcOpen \"{}\" mode 0x{:X} -> {}", name, mode, it - files_.begin()));
    return int32_t(it - files_.begin());
  }

  struct File {
    bool open = false;
    fs::path path;
    uint64_t pos = 0;
    bool writable = false;
  };
  File* file(uint32_t fd) { return fd < files_.size() && files_[fd].open ? &files_[fd] : nullptr; }

  int32_t close(uint32_t fd) {
    File* f = file(fd);
    if (!f) return kNoEntry;
    f->open = false;
    return kOk;
  }

  int32_t seek(uint32_t fd, int32_t offset, uint32_t origin) {
    File* f = file(fd);
    if (!f) return kNoEntry;
    std::error_code ec;
    const int64_t base = origin == 0 ? 0 : origin == 1 ? int64_t(f->pos) : int64_t(fs::file_size(f->path, ec));
    f->pos = uint64_t(std::max<int64_t>(0, base + offset));
    return int32_t(f->pos);
  }

  // {fd, -, -, size, -, -, EE buffer, EE partial-transfer block}
  int32_t read(Sif& sif, uint32_t buf) {
    File* f = file(rd32(sif, buf));
    if (!f) return kNoEntry;
    const uint32_t size = rd32(sif, buf + 0xC), dst = rd32(sif, buf + 0x18);
    if (uint8_t* partial = sif.cpu.mem.hostPtr(rd32(sif, buf + 0x1C), 16)) std::memset(partial, 0, 16);
    uint8_t* out = sif.cpu.mem.hostPtr(dst, size);
    if (!out) return kDenied;
    std::ifstream in(f->path, std::ios::binary);
    in.seekg(std::streamoff(f->pos));
    in.read(reinterpret_cast<char*>(out), size);
    const int32_t n = int32_t(in.gcount());
    f->pos += n;
    return n;
  }

  // {fd, -, -, size, -, headSize, EE buffer, -, head bytes[16]}: head bytes come first.
  int32_t write(Sif& sif, uint32_t buf) {
    File* f = file(rd32(sif, buf));
    if (!f || !f->writable) return kDenied;
    const uint32_t size = rd32(sif, buf + 0xC), headSize = std::min<uint32_t>(rd32(sif, buf + 0x14), 16);
    const uint8_t* head = sif.iop(buf + 0x20, 16);
    const uint8_t* body = size ? sif.cpu.mem.hostPtr(rd32(sif, buf + 0x18), size) : nullptr;
    if (size && !body) return kDenied;
    std::fstream out(f->path, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(std::streamoff(f->pos));
    out.write(reinterpret_cast<const char*>(head), headSize);
    if (size) out.write(reinterpret_cast<const char*>(body), size);
    if (!out) return kFull;
    f->pos += headSize + size;
    return int32_t(headSize + size);
  }

  // {port, slot, continue, maxEntries, EE table, name pattern}; 64-byte sceMcTblGetDir entries.
  int32_t getDir(Sif& sif, uint32_t buf) {
    if (!present(rd32(sif, buf))) return kNoCard;
    const uint32_t cont = rd32(sif, buf + 8), maxent = rd32(sif, buf + 0xC), table = rd32(sif, buf + 0x10);
    const std::string pattern = str(sif.iop(buf + 0x14, 0x400), 0x3FF);
    if (!cont) {
      listing_.clear();
      listPos_ = 0;
      const size_t slash = pattern.find_last_of('/');
      const std::string dir = slash == std::string::npos ? "" : pattern.substr(0, slash + 1);
      const std::string mask = slash == std::string::npos ? pattern : pattern.substr(slash + 1);
      const fs::path hostDir = hostPath(dir.empty() ? "." : dir);
      std::error_code ec;
      if (!fs::is_directory(hostDir, ec)) {  // a missing folder is an error, not an empty listing
        sif.log(std::format("mcGetDir \"{}\" -> no such folder", pattern));
        return kNoEntry;
      }
      if (hostDir != card() && fs::is_directory(hostDir, ec))
        for (const char* dots : {".", ".."})
          if (wildcard(mask.c_str(), dots)) listing_.push_back({dots, hostDir, true});
      for (auto& e : fs::directory_iterator(hostDir, ec)) {
        const std::string n = e.path().filename().string();
        if (wildcard(mask.c_str(), n.c_str())) listing_.push_back({n, e.path(), e.is_directory()});
      }
    }
    uint32_t count = 0;
    for (; listPos_ < listing_.size() && count < maxent; ++listPos_, ++count) {
      uint8_t* e = sif.cpu.mem.hostPtr(table + 64 * count, 64);
      if (!e) break;
      const Entry& en = listing_[listPos_];
      std::error_code ec;
      std::memset(e, 0, 64);
      const auto when = fs::last_write_time(en.path, ec);
      dateTime(e + 0, when);
      dateTime(e + 8, when);
      const uint32_t bytes = en.dir ? 0 : uint32_t(fs::file_size(en.path, ec));
      const uint16_t attr = en.dir ? kAttrDir : kAttrFile;
      std::memcpy(e + 16, &bytes, 4);
      std::memcpy(e + 20, &attr, 2);
      std::memcpy(e + 32, en.name.data(), std::min<size_t>(en.name.size(), 31));
    }
    sif.log(std::format("mcGetDir \"{}\" -> {}", pattern, count));
    return int32_t(count);
  }

  // {port, slot, flags, -, EE info table, name}: only renaming changes anything on the host.
  int32_t setInfo(Sif& sif, uint32_t buf) {
    if (!present(rd32(sif, buf))) return kNoCard;
    const uint32_t flags = rd32(sif, buf + 8);
    const fs::path path = hostPath(str(sif.iop(buf + 0x14, 0x400), 0x3FF));
    std::error_code ec;
    if (!fs::exists(path, ec)) return kNoEntry;
    if (flags & kRenameFlag) {
      const std::string to = str(sif.cpu.mem.hostPtr(rd32(sif, buf + 0x10) + 0x20, 32), 32);
      fs::rename(path, path.parent_path() / to, ec);
      if (ec) return kDenied;
    }
    return kOk;
  }

  int32_t remove(Sif& sif, uint32_t buf) {
    if (!present(rd32(sif, buf))) return kNoCard;
    const std::string name = str(sif.iop(buf + 0x14, 0x400), 0x3FF);
    const fs::path path = hostPath(name);
    std::error_code ec;
    if (!fs::exists(path, ec)) return kNoEntry;
    if (fs::is_directory(path, ec) && !fs::is_empty(path, ec)) return kNotEmpty;
    fs::remove(path, ec);
    sif.log(std::format("mcDelete \"{}\"", name));
    return ec ? kDenied : kOk;
  }

  // Free directory entries in a folder ({port, slot, -, -, -, path}); games check it before
  // creating a save. Folders on a real card grow by clusters, so this is generous.
  int32_t entrySpace(Sif& sif, uint32_t buf) {
    if (!present(rd32(sif, buf))) return kNoCard;
    const fs::path path = hostPath(str(sif.iop(buf + 0x14, 0x400), 0x3FF));
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return kNoEntry;
    int32_t used = 0;
    for (auto it = fs::directory_iterator(path, ec); it != fs::directory_iterator(); ++it) ++used;
    return std::max(0, 1024 - used);
  }

  int32_t format(Sif& sif, uint32_t buf) {
    if (!present(rd32(sif, buf + 4))) return kNoCard;
    // Formatting the card would erase saves the player made; the host folder is kept, and the
    // card simply reports as formatted.
    sif.log("mcFormat (kept existing saves)");
    return kOk;
  }

  struct Entry {
    std::string name;
    fs::path path;
    bool dir = false;
  };
  fs::path root_;
  std::string cwd_ = "";
  bool seen_ = false;
  std::vector<File> files_;
  std::vector<Entry> listing_;
  size_t listPos_ = 0;
};
}  // namespace

std::unique_ptr<Sif::RpcServer> makeMemcardServer(uint32_t sid, const fs::path& root) {
  if (sid == kMcServer) return std::make_unique<McServer>(root);
  return nullptr;
}
