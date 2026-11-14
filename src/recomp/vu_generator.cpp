// `codport --vu-recompile BLOCKS DIR`: turns the VU micro-program blocks collected while playing
// (CODPORT_VU_COLLECT) into C++ that calls the interpreter's own handlers with every operand
// known at compile time, so a block runs without decoding or dispatch.
//
// Each block has two versions. The general one keeps the interpreter's per-pair bookkeeping. The
// usual one, taken when no Q or P result is on its way at entry, works out at compile time when
// each pipelined result arrives: MAC/status results are held in locals and written at the fourth
// pair after their instruction (or left queued at the end of the block), Q and P results and
// WAITQ/WAITP stalls happen at known pairs, and only results queued by earlier blocks (due in
// the first four pairs) are checked at run time.
#include <cstring>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include "ee/vu.h"
#include "ee/vu_ops.h"

namespace fs = std::filesystem;

namespace {
struct Block {
  uint32_t pc;
  std::vector<uint32_t> words;  // lo, hi per pair
};

std::string pairConstant(const VuMicro::Pair& p, const std::string& name) {
  return std::format(
      "  static constexpr VuMicro::Pair {} = {{.hi = 0x{:08X}u, .lo = 0x{:08X}u, .fs = {}, .ft = {}, .fd = {}, .dest = {}, "
      ".bc = {}, .setAside = {}, .flags = {}, .iBit = {}, .eBit = {}, .waitQ = {}, .waitP = {}, .is = {}, .it = {}, "
      ".lfs = {}, .lft = {}, .ldest = {}, .imm = {}}};\n",
      name, p.hi, p.lo, p.fs, p.ft, p.fd, p.dest, p.bc, p.setAside, p.flags, p.iBit, p.eBit, p.waitQ, p.waitP, p.is, p.it,
      p.lfs, p.lft, p.ldest, p.imm);
}

// One pair's instructions, exactly as VuMicro::run executes them; with `flags`, the upper
// instruction's MAC result goes to that local instead of the queue.
std::string pairOps(const VuMicro::Pair& p, const std::string& name, uint32_t pc, const std::string& flags) {
  std::string out;
  std::string upper = std::format("{}(m, {});", p.upperName, name);
  if (!flags.empty()) {
    std::string fn = p.upperName;
    const size_t at = fn.find("upperArith<");
    fn = at != std::string::npos ? fn.replace(at, 10, "upperArithMac") : "VuMicro::upperGenericMac";
    out += std::format("  uint32_t {};\n", flags);
    upper = std::format("{} = {}(m, {});", flags, fn, name);
  }
  const std::string lower = p.iBit ? std::format("m.vi_[kVuI] = 0x{:08X}u;", p.lo)
                                   : std::format("{}(m, {}, 0x{:04X}u);", p.lowerName, name, pc);
  if (p.setAside) {
    out += std::format("  {{\n    u128& reg = m.vf_[{}];\n    const u128 before = reg;\n    {}\n    const u128 result = reg;\n"
                       "    reg = before;\n    {}\n    reg = result;\n  }}\n",
                       p.setAside, upper, lower);
  } else {
    out += "  " + upper + "\n  " + lower + "\n";
  }
  out += "  m.vi_[0] = 0;\n";
  return out;
}

// The general version: as VuMicro::run, pair by pair.
std::string pairCode(const VuMicro::Pair& p, const std::string& name, uint32_t pc) {
  std::string out;
  if (p.waitQ || p.waitP) out += std::format("  m.pairStall({}, {});\n", p.waitQ, p.waitP);
  return out + pairOps(p, name, pc, "");
}

bool pushesFlags(const VuMicro::Pair& p) {
  if (p.upper == &VuMicro::upperGeneric) return upperSetsFlags(p.hi);
  const std::string name = p.upperName;
  return p.flags && name.find("upperArith<") != std::string::npos && name.find("kMax") == std::string::npos &&
         name.find("kMin") == std::string::npos;
}

// Returns empty if the block cannot be compiled (a branch in a delay slot, a branch next to an E bit).
std::string blockFunction(const Block& b, const std::string& fname) {
  const int n = int(b.words.size() / 2);
  std::vector<uint8_t> code(16384, 0), data(16384, 0);
  if (b.pc + n * 8 + 8 > code.size()) return {};
  std::memcpy(code.data() + b.pc, b.words.data(), size_t(n) * 8);
  // After the block, a branch: flag-liveness scans stop there and assume the flags are needed.
  const uint32_t sentinel[2] = {0x40000000u, 0x000002FFu};  // B 0 / NOP
  std::memcpy(code.data() + b.pc + n * 8, sentinel, 8);
  u128 vf[32]{};
  uint32_t vi[32]{};
  u128 acc{};
  VuMicro vu(VuRegs{vf, vi, &acc}, code.data(), uint32_t(code.size()), data.data(), uint32_t(data.size()));

  std::vector<VuMicro::Pair> pairs;
  bool endsProgram = false, branched = false;
  for (int i = 0; i < n; ++i) {
    const VuMicro::Pair p = vu.decode(b.pc + uint32_t(i) * 8);
    const bool branch = !p.iBit && lowerBranches(p.lo);
    if (branch && (branched || endsProgram || i == n - 1)) return {};
    pairs.push_back(p);
    branched = branched || branch;
    if (p.eBit) endsProgram = true;
  }
  auto constants = [&] {
    std::string c;
    for (int i = 0; i < n; ++i) c += pairConstant(pairs[size_t(i)], std::format("p{}", i));
    return c;
  };
  auto ending = [&](bool branchedBlock) {
    std::string e;
    if (endsProgram) e += "  m.blockEnded_ = true;\n";
    const uint32_t next = b.pc + uint32_t(n) * 8;
    return e + (branchedBlock ? std::format("  return take ? target : 0x{:04X}u;\n", next) : std::format("  return 0x{:04X}u;\n", next));
  };
  const char* kTake = "  const bool take = m.branchPending_;\n  const uint32_t target = m.branchTarget_;\n  m.branchPending_ = false;\n";

  // General version.
  std::string out = std::format("uint32_t {}_s(VuMicro& m) {{\n", fname) + constants();
  branched = false;
  for (int i = 0; i < n; ++i) {
    const VuMicro::Pair& p = pairs[size_t(i)];
    out += "  m.pairStart();\n";
    if (branched) out += kTake;
    out += pairCode(p, std::format("p{}", i), b.pc + uint32_t(i) * 8);
    branched = branched || (!p.iBit && lowerBranches(p.lo));
  }
  out += ending(branched) + "}\n\n";

  // Usual version: the pipeline worked out here. cyc = pairs (and stall cycles) since entry.
  // A Q or P result still on its way from an earlier block counts down at run time (qIn, pIn)
  // until this block issues its own: caught up (as one pair at a time would have) only where
  // something may read Q or P, issues its own, or the block ends. A block that waits for Q or P
  // stalls by an amount known only then, so it takes the general version.
  bool waits = false;
  for (const VuMicro::Pair& p : pairs) waits = waits || p.waitQ || p.waitP;
  // Two of it: one for when nothing is on its way from earlier blocks (the usual case: no checks
  // at all), one that checks what is.
  auto usual = [&](bool clean) {
    std::string out;
    out += std::format("uint32_t {}{}(VuMicro& m) {{\n", fname, clean ? "_c" : "_u");
    if (!clean) {
      if (waits) out += std::format("  if (m.qDelay_ > 0 || m.pDelay_ > 0) return {}_s(m);\n", fname);
      else out += "  int qIn = m.qDelay_, pIn = m.pDelay_;\n";
    }
    out += constants() + "  const uint64_t base = m.cycle_;\n";
    struct Flags {
      int pushed;  // cycle of the instruction
      std::string var;
    };
    std::vector<Flags> inFlight;
    int cyc = 0, qd = 0, pd = 0;
    bool usesQ = false, usesP = false;
    int qAt = 0, pAt = 0;  // cycle the run-time countdowns stand at
    auto catchUp = [&](const char* left, const char* reg, int& at) {
      std::string c;
      if (cyc > at)
        c = std::format("  if ({0} > 0) {{\n    if ({0} <= {1}) {2} = m.pending{3}_, {0} = 0;\n    else {0} -= {1};\n  }}\n", left, cyc - at, reg,
                        left[0] == 'q' ? "Q" : "P");
      at = cyc;
      return c;
    };
    auto arrive = [&](int now) {  // flags due by cycle `now`, oldest first
      std::string a;
      while (!inFlight.empty() && inFlight.front().pushed + 4 <= now) {
        a += std::format("  vuops::writeMac(m.vi_, {});\n", inFlight.front().var);
        inFlight.erase(inFlight.begin());
      }
      return a;
    };
    bool branched = false;
    for (int i = 0; i < n; ++i) {
      const VuMicro::Pair& p = pairs[size_t(i)];
      const uint32_t pc = b.pc + uint32_t(i) * 8;
      ++cyc;
      if (cyc <= 4 && !clean) out += std::format("  m.settleAt(base + {});\n", cyc);  // queued by earlier blocks
      out += arrive(cyc);
      if (qd > 0 && --qd == 0) out += "  m.vi_[kVuQ] = m.pendingQ_;\n";
      if (pd > 0 && --pd == 0) out += "  m.p_ = m.pendingP_;\n";
      const std::string upperName = p.upperName;
      const bool readsQ = !p.iBit && (p.lower == &VuMicro::lowerGeneric || p.lower == &VuMicro::lowerDiv);
      const bool readsP = !p.iBit && (p.lower == &VuMicro::lowerGeneric || p.lower == &VuMicro::lowerMfp);
      if (!waits && !usesQ && !clean && (readsQ || p.upper == &VuMicro::upperGeneric || upperName.find("kQ") != std::string::npos))
        out += catchUp("qIn", "m.vi_[kVuQ]", qAt);
      if (!waits && !usesP && !clean && readsP) out += catchUp("pIn", "m.p_", pAt);
      if (branched) out += kTake;
      if ((p.waitQ && qd > 0) || (p.waitP && pd > 0)) {  // as VuMicro::pairStall
        const int stall = std::max(p.waitQ ? qd : 0, p.waitP ? pd : 0);
        if (qd > 0 && (p.waitQ || qd <= stall)) out += "  m.vi_[kVuQ] = m.pendingQ_;\n", qd = 0;
        else if (qd > 0) qd -= stall;
        if (pd > 0 && (p.waitP || pd <= stall)) out += "  m.p_ = m.pendingP_;\n", pd = 0;
        else if (pd > 0) pd -= stall;
        if (cyc < 4 && !clean) out += std::format("  m.settleUpTo(base + {});\n", cyc + stall);
        cyc += stall;
        out += arrive(cyc);
      }
      const bool pushes = pushesFlags(p);
      const std::string var = pushes ? std::format("f{}", i) : "";
      out += pairOps(p, std::format("p{}", i), pc, var);
      if (pushes) inFlight.push_back({cyc, var});
      if (!p.iBit) {
        if (const int q = VuMicro::qLatency(p.lo); q && p.lower == &VuMicro::lowerDiv) qd = q, usesQ = true;
        if (const int l = VuMicro::pLatency(p.lo); l && p.lower == &VuMicro::lowerGeneric) pd = l, usesP = true;
      }
      branched = branched || (!p.iBit && lowerBranches(p.lo));
    }
    if (!waits && !usesQ && !clean) out += catchUp("qIn", "m.vi_[kVuQ]", qAt);
    if (!waits && !usesP && !clean) out += catchUp("pIn", "m.p_", pAt);
    out += std::format("  m.instructions += {};\n  m.cycle_ = base + {};\n", n, cyc);
    for (const Flags& f : inFlight)
      out += std::format("  m.pendingFlags_[(base + {0}) & 3] = {{base + {1}, {2}, true}};\n", f.pushed, f.pushed + 4, f.var);
    if (usesQ) out += std::format("  m.qDelay_ = {};\n", qd);
    else if (!waits && !clean) out += "  m.qDelay_ = qIn;\n";
    if (usesP) out += std::format("  m.pDelay_ = {};\n", pd);
    else if (!waits && !clean) out += "  m.pDelay_ = pIn;\n";
    out += ending(branched) + "}\n\n";
    return out;
  };
  out += usual(true) + usual(false);
  out += std::format("uint32_t {0}(VuMicro& m) {{\n  if ((m.qDelay_ | m.pDelay_) == 0 && !(m.pendingFlags_[0].valid | m.pendingFlags_[1].valid | "
                     "m.pendingFlags_[2].valid | m.pendingFlags_[3].valid))\n    return {0}_c(m);\n  return {0}_u(m);\n}}\n\n", fname);
  return out;
}
}  // namespace

bool generateVuCode(const std::vector<fs::path>& blocksFiles, const std::vector<uint8_t>& executable, const fs::path& outDir,
                    std::string& error) {
  std::vector<Block> blocks;
  std::set<std::pair<uint32_t, std::vector<uint32_t>>> seen;  // a block in two files is compiled once
  for (const fs::path& blocksFile : blocksFiles) {
  std::ifstream in(blocksFile);
  if (!in) {
    error = "cannot read " + blocksFile.string();
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    Block b;
    uint32_t n = 0;
    ls >> std::hex >> b.pc >> n;
    if (line.find('@') == std::string::npos) {  // the words themselves (CODPORT_VU_COLLECT)
      b.words.resize(size_t(n) * 2);
      for (uint32_t& w : b.words) ls >> w;
      if (ls && n && seen.insert({b.pc, b.words}).second) blocks.push_back(std::move(b));
      continue;
    }
    // An index: "@offset:pairs" pieces of the executable, then "#hash" of the words (FNV-1a 64).
    std::string token;
    uint64_t hash = 0;
    while (ls >> token) {
      if (token[0] == '#') {
        hash = std::stoull(token.substr(1), nullptr, 16);
        continue;
      }
      const size_t colon = token.find(':');
      if (token[0] != '@' || colon == std::string::npos) continue;
      const size_t at = std::stoull(token.substr(1, colon - 1), nullptr, 16), pairs = std::stoull(token.substr(colon + 1), nullptr, 16);
      if (at + pairs * 8 > executable.size()) break;
      for (size_t i = 0; i < pairs * 2; ++i) {
        uint32_t w;
        std::memcpy(&w, executable.data() + at + i * 4, 4);
        b.words.push_back(w);
      }
    }
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t w : b.words)
      for (int k = 0; k < 4; ++k) h = (h ^ ((w >> (8 * k)) & 0xFF)) * 0x100000001B3ull;
    if (b.words.size() != size_t(n) * 2 || h != hash) {
      error = std::format("VU block {:x} in {} does not match this disc's executable (another version of the game?)", b.pc,
                          blocksFile.string());
      return false;
    }
    if (seen.insert({b.pc, b.words}).second) blocks.push_back(std::move(b));
  }
  }
  fs::create_directories(outDir);
  for (const auto& e : fs::directory_iterator(outDir))
    if (e.path().extension() == ".cpp") fs::remove(e.path());

  constexpr size_t kBytesPerFile = 400 * 1024;
  std::string body, entries;
  int fileIndex = 0, compiled = 0;
  size_t count = 0;
  auto flush = [&] {
    if (body.empty()) return;
    std::string text = "// Generated by codport --vu-recompile. Do not edit.\n#include \"ee/vu_ops.h\"\n\nnamespace {\n" + body +
                       "}  // namespace\n\n";
    text += std::format("extern const VuBlockEntry vu_entries_{}[] = {{\n{}}};\nextern const size_t vu_count_{} = {};\n",
                        fileIndex, entries, fileIndex, count);
    std::ofstream(outDir / std::format("vu_{:03}.cpp", fileIndex)) << text;
    body.clear(), entries.clear(), count = 0;
    ++fileIndex;
  };
  for (const Block& b : blocks) {
    const int n = int(b.words.size() / 2);
    const uint64_t hash = vuBlockHash(b.pc, reinterpret_cast<const uint8_t*>(b.words.data()), n);
    const std::string fname = std::format("vb_{:04X}_{:016X}", b.pc, hash);
    const std::string fn = blockFunction(b, fname);
    if (fn.empty()) continue;
    body += fn;
    entries += std::format("  {{0x{:04X}u, {}u, 0x{:016X}ull, {}}},\n", b.pc, n, hash, fname);
    ++count, ++compiled;
    if (body.size() >= kBytesPerFile) flush();
  }
  flush();
  std::string table = "// Generated by codport --vu-recompile. Do not edit.\n#include \"ee/vu.h\"\n\n";
  for (int i = 0; i < fileIndex; ++i)
    table += std::format("extern const VuBlockEntry vu_entries_{}[];\nextern const size_t vu_count_{};\n", i, i);
  table += "\nextern \"C\" __declspec(dllexport) void codportVuBlocks(std::vector<VuBlockEntry>& out) {\n";
  for (int i = 0; i < fileIndex; ++i) table += std::format("  out.insert(out.end(), vu_entries_{0}, vu_entries_{0} + vu_count_{0});\n", i);
  table += "}\n";
  std::ofstream(outDir / "table.cpp") << table;
  std::printf("compiled %d of %zu VU blocks into %d files in %s\n", compiled, blocks.size(), fileIndex, outDir.string().c_str());
  return true;
}
