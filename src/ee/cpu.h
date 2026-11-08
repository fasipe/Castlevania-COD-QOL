#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codport_api.h"
#include "ee/memory.h"

union u128 {
  uint64_t ud[2];
  int64_t sd[2];
  uint32_t uw[4];
  int32_t sw[4];
  uint16_t uh[8];
  int16_t sh[8];
  uint8_t ub[16];
  int8_t sb[16];
  float f[4];
};

// Everything a thread switch has to save. The call stack is a shadow stack of
// (callee, return address) pairs used only for diagnostics.
struct CpuState {
  u128 gpr[32]{};
  u128 hi{}, lo{};  // [0] = pipeline 0, [1] = pipeline 1 (MULT1/DIV1/...)
  uint32_t pc = 0, npc = 0;
  uint32_t sa = 0;  // funnel shift amount in bits (QFSRV)
  float fpr[32]{};
  float acc = 0;
  uint32_t fcr31 = 0;
  uint32_t cop0[32]{};
  u128 vf[32]{};      // VU0 float registers (COP2)
  uint32_t vi[32]{};  // VU0 integer and control registers (I, Q, R kept as float bits)
  u128 vacc{};        // VU0 accumulator
  std::vector<std::pair<uint32_t, uint32_t>> callstack;

  void reset(uint32_t entry);
};

enum class StopReason { None, Unimplemented, Break, Fault, Exit, Idle, Polling, Limit };
const char* toString(StopReason r);

class Kernel;

// Reference interpreter for the R5900 (Emotion Engine) core. It runs the original code
// so every later step of the port (the static recompiler, hardware HLE) can be checked
// against it. It stops with a precise reason the first time it meets something the port
// does not handle yet.
class CODPORT_API Cpu {
public:
  explicit Cpu(Memory& memory) : mem(memory) {}

  void run(uint64_t maxInstructions);
  void halt(StopReason reason, std::string detail);
  void execute(uint32_t op, uint32_t pc);  // one instruction (also used by recompiled code)
  void idleCheck(uint32_t pc, int32_t length);  // a backward branch: skip time if the loop just waits

  // Statically recompiled code (src/recomp): entry points indexed by (pc - kRecompBase) / 4.
  using RecompFn = uint32_t (*)(Cpu&, uint32_t entry);
  static constexpr uint32_t kRecompBase = 0x00400000;
  std::vector<RecompFn> recomp;
  uint64_t pollAt = 64;      // instruction count at which timers/interrupts are next checked
  uint64_t recompCalls = 0;  // times control entered recompiled code
  uint64_t interpreted = 0;  // instructions the interpreter loop ran
  uint64_t idleSkipped = 0;  // EE cycles skipped in idle loops
  std::unordered_map<uint32_t, uint64_t>* profile = nullptr;  // samples per 64-byte code block

  CpuState s;
  Memory& mem;
  Kernel* kernel = nullptr;
  StopReason stop = StopReason::None;
  std::string stopDetail;
  uint64_t executed = 0;
  uint32_t currentPc = 0;
  std::unordered_map<uint32_t, uint64_t> calls;  // function entry -> times called
  std::vector<uint32_t> callOrder;               // function entries in first-call order

private:
  uint32_t spinPc_ = 0;
  uint64_t spinWrites_ = 0, spinState_ = 0;
  int spinCount_ = 0;

public:

private:
  void special(uint32_t op, uint32_t pc);
  void regimm(uint32_t op, uint32_t pc);
  void mmi(uint32_t op, uint32_t pc);
  void mmi0(uint32_t op, uint32_t pc);
  void mmi1(uint32_t op, uint32_t pc);
  void mmi2(uint32_t op, uint32_t pc);
  void mmi3(uint32_t op, uint32_t pc);
  void cop0(uint32_t op, uint32_t pc);
  void cop1(uint32_t op, uint32_t pc);
  void cop2(uint32_t op, uint32_t pc);
  void cop2Macro(uint32_t op, uint32_t pc);  // VU0 macro mode, in vu0.cpp

  void branch(bool taken, uint32_t pc, int32_t imm, bool likely);
  void noteCall(uint32_t target, uint32_t ret);
  void noteReturn(uint32_t target);
  void unimplemented(uint32_t op, uint32_t pc, const char* what);
  void mult(int pipe, uint32_t rs, uint32_t rt, uint32_t rd, bool isSigned, bool accumulate);
  void div(int pipe, uint32_t rs, uint32_t rt, bool isSigned);
};
