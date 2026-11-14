#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <immintrin.h>
#include <string>
#include <vector>

#include "ee/cpu.h"

// The vector units. VU0 runs in macro mode (COP2 instructions from the EE, see Cpu::cop2Macro)
// or micro mode (VCALLMS); VU1 runs micro programs uploaded by VIF1. Both share the FMAC (upper)
// instruction set and most of the lower set, implemented once here.

// Views of a register file: 32 float vectors, 16 integer registers followed by the control
// registers (16 status, 17 MAC, 18 clip, 20 R, 21 I, 22 Q; floats kept as raw bits), and ACC.
struct VuRegs {
  u128* vf;
  uint32_t* vi;
  u128* acc;
};
enum : int { kVuStatus = 16, kVuMac = 17, kVuClip = 18, kVuR = 20, kVuI = 21, kVuQ = 22 };

// Lower instructions with the same encoding in macro and micro mode (MOVE ... RXOR, field
// numbers 0x30-0x43 of the second decode table). `data`/`dataQwords` is the unit's data memory.
// DIV/SQRT/RSQRT store their result in *q. False if `op` is not one of them.
CODPORT_API bool vuLowerCommon(VuRegs r, uint32_t op, uint8_t* data, uint32_t dataQwords, uint32_t* q);

// Micro-mode interpreter: runs from a start address until an E-bit (plus its delay slot).
// It works on a register file of its own (registers at fixed offsets from the unit, so compiled
// code reaches them without loading pointers); `regs`, if given, is an outside one (VU0's: the
// EE's COP2 registers) copied in and back out around each run, which the EE waits for.
class CODPORT_API VuMicro {
public:
  VuMicro(VuRegs regs, uint8_t* code, uint32_t codeBytes, uint8_t* data, uint32_t dataBytes)
      : outside_(regs), code_(code), codeBytes_(codeBytes), data_(data), dataBytes_(dataBytes) {}
  VuMicro(const VuMicro&) = delete;  // r_ points into the object
  VuMicro& operator=(const VuMicro&) = delete;

  void run(uint32_t startAddr);  // byte address in micro memory
  void setRegs(VuRegs regs) { outside_ = regs; }
  void codeChanged() { ++generation_; }  // micro memory was written (decoded pairs are stale)
  const uint64_t* codeTouches = nullptr;  // counts EE accesses to micro memory (Memory::vuCodeTouches)

  std::function<void(uint32_t qwordAddr)> xgkick;  // VU1 only
  std::function<uint32_t()> top, itop;             // VIF1 TOP / ITOP
  std::string fault;
  uint64_t programs = 0, instructions = 0, runNanos = 0;
  uint32_t tpc = 0;  // where the last program stopped

  // Internals below are public for the micro programs compiled ahead of time (recomp/vu).
  // An instruction pair decoded once: handlers for both halves and what the pair needs.
  struct Pair;
  using UpperFn = void (*)(VuMicro&, const Pair&);
  using LowerFn = void (*)(VuMicro&, const Pair&, uint32_t pc);
  struct Pair {
    UpperFn upper;
    LowerFn lower;
    uint32_t hi, lo;
    uint64_t generation;
    uint8_t fs, ft, fd, dest, bc;
    uint8_t setAside;  // vector register the upper half writes and the lower half may touch (0: none)
    bool flags;        // the upper half's MAC/status results can be read before being replaced
    bool iBit, eBit;
    bool waitQ, waitP;  // the lower half is WAITQ / WAITP: the whole pair waits for the result
    // Lower half fields.
    uint8_t is, it, lfs, lft, ldest;
    int32_t imm;
    const char* upperName;  // the handlers' names, for the compiled programs (recomp/vu)
    const char* lowerName;
  };
  const Pair& decode(uint32_t pc);
  bool flagsNeeded(uint32_t pc) const;
  template <int O, int B, bool ToAcc>
  static void upperArith(VuMicro& m, const Pair& p);
  static void upperGeneric(VuMicro& m, const Pair& p);
  // The same returning their MAC flags instead of queueing them (compiled blocks keep flags in
  // flight themselves); upperGenericMac only for instructions that set flags.
  template <int O, int B, bool ToAcc>
  static uint32_t upperArithMac(VuMicro& m, const Pair& p);
  static uint32_t upperGenericMac(VuMicro& m, const Pair& p);
  template <int O, int B, bool ToAcc>
  static __m128 upperArithStore(VuMicro& m, const Pair& p, __m128& mask);
  // Pairs until a DIV/SQRT/RSQRT result reaches Q, or an EFU result reaches P (0: not one).
  static int qLatency(uint32_t lo);
  static int pLatency(uint32_t lo);
  static void upperNop(VuMicro&, const Pair&) {}
  static void lowerGeneric(VuMicro& m, const Pair& p, uint32_t pc) { m.lower(p.lo, pc); }
  static void lowerNop(VuMicro&, const Pair&, uint32_t) {}
  static void lowerLq(VuMicro& m, const Pair& p, uint32_t);
  static void lowerSq(VuMicro& m, const Pair& p, uint32_t);
  static void lowerLqi(VuMicro& m, const Pair& p, uint32_t);
  static void lowerSqi(VuMicro& m, const Pair& p, uint32_t);
  static void lowerIaddiu(VuMicro& m, const Pair& p, uint32_t);
  static void lowerIsubiu(VuMicro& m, const Pair& p, uint32_t);
  static void lowerIadd(VuMicro& m, const Pair& p, uint32_t);
  static void lowerIaddi(VuMicro& m, const Pair& p, uint32_t);
  static void lowerMove(VuMicro& m, const Pair& p, uint32_t);
  static void lowerBranch(VuMicro& m, const Pair& p, uint32_t pc);
  static void lowerInteger(VuMicro& m, const Pair& p, uint32_t pc);
  static void lowerMfir(VuMicro& m, const Pair& p, uint32_t);
  static void lowerMtir(VuMicro& m, const Pair& p, uint32_t);
  static void lowerMr32(VuMicro& m, const Pair& p, uint32_t);
  static void lowerRnext(VuMicro& m, const Pair& p, uint32_t);  // and RGET
  static void lowerMfp(VuMicro& m, const Pair& p, uint32_t);
  static void lowerDiv(VuMicro& m, const Pair& p, uint32_t);
  static void lowerXgkick(VuMicro& m, const Pair& p, uint32_t);
  template <int Scale>
  static void upperFtoi(VuMicro& m, const Pair& p);
  template <int Scale>
  static void upperItof(VuMicro& m, const Pair& p);
  static void upperClip(VuMicro& m, const Pair& p);
  static void upperAbs(VuMicro& m, const Pair& p);
  std::vector<Pair> pairs_;

  // Compiled micro programs (recomp/vu): straight-line blocks from an entry point to the delay
  // slot of the first branch (or the end of the program), keyed by address and code.
  using BlockFn = uint32_t (*)(VuMicro&);  // runs the block, returns the next address
  static constexpr int kMaxBlock = 64;      // pairs
  struct BlockCache {
    uint64_t generation = 0;
    BlockFn fn = nullptr;
    std::vector<uint8_t> code;  // the words `fn` was found for: still them, still it (no new lookup)
  };
  std::vector<BlockCache> blocks_;
  bool blockEnded_ = false;  // the block ran the program's last pair
  int blockLength(uint32_t pc) const;
  BlockFn findBlock(uint32_t pc);
  uint64_t generation_ = 1, seenTouches_ = 0;
  uint32_t seenRegistry_ = 0;

  static constexpr int kFlagLatency = 4;
  struct PendingFlags {
    uint64_t at = 0;
    uint32_t mac = 0;
    bool valid = false;
  };
  PendingFlags pendingFlags_[4];
  uint64_t cycle_ = 0;  // pairs executed
  void delayFlags(uint32_t mac);
  void settleFlags(bool all);
  void settleDue();
  void settleAt(uint64_t cycle);    // settleDue at that cycle
  void settleUpTo(uint64_t cycle);  // settleFlags(false) at that cycle
  void pairStart();
  void pairStall(bool waitQ, bool waitP);

  void lower(uint32_t op, uint32_t pc);
  void branchTo(uint32_t target) { branchPending_ = true; branchTarget_ = target; }

  u128 vf_[32]{};
  uint32_t vi_[32]{};
  u128 acc_{};
  VuRegs r_{vf_, vi_, &acc_};
  VuRegs outside_;
  void execute(uint32_t startAddr);
  uint8_t* code_;
  uint32_t codeBytes_;
  uint8_t* data_;
  uint32_t dataBytes_;
  uint32_t pendingQ_ = 0, pendingP_ = 0, p_ = 0;
  int qDelay_ = 0, pDelay_ = 0;
  bool branchPending_ = false;
  uint32_t branchTarget_ = 0;
};

// Compiled micro-program blocks: generated by `codport --vu-recompile` (recomp/vu/table.cpp) or
// none (src/recomp/vunone.cpp).
struct VuBlockEntry {
  uint32_t pc, pairs;
  uint64_t hash;
  VuMicro::BlockFn fn;
};
void registerVuBlocks(std::vector<VuBlockEntry>& out);
// Compiled blocks that became available while the game runs (src/recomp/gamecode.cpp).
void addVuBlocks(const std::vector<VuBlockEntry>& entries);
CODPORT_API uint64_t vuBlockHash(uint32_t pc, const uint8_t* code, int pairs);
// With CODPORT_VU_COLLECT=FILE, blocks run without compiled code are added to FILE at exit.
void vuCollectSave();
// `executable`: the game's, for a blocks file that says where in it each block's words are (an index,
// as tools/recomp/vu-blocks.idx) rather than holding them.
bool generateVuCode(const std::vector<std::filesystem::path>& blocksFiles, const std::vector<uint8_t>& executable,
                    const std::filesystem::path& outDir, std::string& error);
