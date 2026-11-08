#include "game/sprint.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

#include "ee/cpu.h"
#include "game/hooks.h"
#include "iop/pad.h"

namespace {
// The player's animation state (FUN_0046E980 runs it each frame): +0x24 the current state.
constexpr uint32_t kStateMachine = 0x00B066C0, kStateUpdate = 0x0046E980;
constexpr uint32_t kRunState = 15;
constexpr uint32_t kPlayer = 0x00B06880;
constexpr uint32_t kTimeScale = 0x138;  // of the player: all its timers, its animation and its moves
// FUN_00799FB8 takes an object's animated bones to its final pose: the arms' and legs' IK and the
// matrices. Called for every object, and for the player after the frame's animation.
constexpr uint32_t kPlayerPose = 0x00799FB8;

constexpr uint16_t kButtonL3 = 0x0002;
constexpr float kSprintScale = 1.55f;
// The steps go only this much faster than the run's: the animation clock (the model's +0xA0, 256 a
// frame at time scale 1) gets back the rest of what the time scale gave it, and the stride does
// the rest of the speed.
constexpr float kSprintCycle = 1.2f;
constexpr uint32_t kModel = 0x104, kAnimClock = 0xA0;
constexpr float kBlendIn = 1.0f / 14, kBlendOut = 1.0f / 8;  // per frame
constexpr int kGraceFrames = 20;
// The pose over the run, in degrees about the model's x axis and in its units.
constexpr float kLean = -24.0f, kHeadUp = 17.0f, kArmGain = 1.35f, kHandsUp = 6.0f;
constexpr float kCentreRate = 1.0f / 24;  // per animation frame: about a stride

// Hector's bones (33, of 0x80 bytes): +0 local rotation (x, y, z, w), +0x60 parent, +0x64 first
// child, +0x68 next sibling, +0x6E flags (8 and 0x10: world matrix up to date). The model's axes:
// x across, y down, z forward.
constexpr int kBones = 33;
// The hands reach for two bones under the chest, so moving those moves the arms.
constexpr int kPelvis = 1, kSpine = 2, kChest = 4, kNeck = 11, kHandTargetA = 31, kHandTargetB = 32;

using Quat = std::array<float, 4>;

Quat mul(const Quat& a, const Quat& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat conj(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
Quat aboutX(float degrees) {
  const float half = degrees * std::numbers::pi_v<float> / 360.0f;
  return {std::sin(half), 0, 0, std::cos(half)};
}

bool gEnabled = false;
bool gSprinting = false, gHeldLast = false, gScaleOurs = false;
int gGrace = 0;
float gBlend = 0;
std::array<Quat, kBones> gWritten{}, gOriginal{};
std::array<Quat, kBones> gWrittenAt{}, gOriginalAt{};  // positions (+0x10), the same way
std::array<bool, kBones> gTouched{}, gTouchedAt{};
std::array<Quat, kBones> gCentre{};  // where swing()'s bones usually are, in their parents' axes
std::array<bool, kBones> gCentreValid{};
Cpu::RecompFn gStateUpdate = nullptr, gPose = nullptr;
uint32_t gClockModel = 0;
int32_t gClockLast = 0, gAdvanceLast = 0;
float gScaleLast = 1.0f;

uint32_t stateUpdate(Cpu& c, uint32_t entry) {
  if (gEnabled && uint32_t(c.s.gpr[4].ud[0]) == kStateMachine) {
    const uint32_t player = c.mem.read<uint32_t>(kPlayer);
    const bool running = c.mem.read<uint32_t>(kStateMachine + 0x24) == kRunState;
    const bool held = (gPadInput.buttons & kButtonL3) != 0;
    // A press counts whenever it comes, as the run may be about to start; it lapses if the player
    // is not running for a moment. The same moment keeps it through a turn, which leaves the state.
    if (held && !gHeldLast) gSprinting = !gSprinting, gGrace = kGraceFrames;
    else if (running) gGrace = kGraceFrames;
    else if (gSprinting && --gGrace <= 0) gSprinting = false;
    gHeldLast = held;
    const bool sprinting = gSprinting && running;
    gBlend = sprinting ? std::min(1.0f, gBlend + kBlendIn) : std::max(0.0f, gBlend - kBlendOut);
    if (player) {
      // The clock ran at last frame's scale: back by what is beyond the cycle's share of it. Where the
      // animation looped back to its start, it went as far as the frame before.
      const uint32_t model = c.mem.read<uint32_t>(player + kModel);
      if (model != 0 && model == gClockModel && gScaleLast > 1.0f) {
        const int32_t clock = int32_t(c.mem.read<uint32_t>(model + kAnimClock));
        const int32_t advance = clock > gClockLast ? clock - gClockLast : gAdvanceLast;
        gAdvanceLast = advance;
        const float cycle = 1.0f + (kSprintCycle - 1.0f) * (gScaleLast - 1.0f) / (kSprintScale - 1.0f);
        const int32_t back = int32_t(float(advance) * (1.0f - cycle / gScaleLast));
        if (advance > 0 && advance < 4096 && back > 0 && back <= clock) c.mem.write<uint32_t>(model + kAnimClock, uint32_t(clock - back));
      }
      gClockModel = model;
      if (model) gClockLast = int32_t(c.mem.read<uint32_t>(model + kAnimClock));
      // Only over the game's own 1 (it stops time at 0 for hits and so on).
      const float now = std::bit_cast<float>(c.mem.read<uint32_t>(player + kTimeScale));
      if (sprinting && (now == 1.0f || gScaleOurs)) {
        const float scale = 1.0f + (kSprintScale - 1.0f) * gBlend;
        c.mem.write<uint32_t>(player + kTimeScale, std::bit_cast<uint32_t>(scale));
        gScaleOurs = true;
      } else if (gScaleOurs) {
        c.mem.write<uint32_t>(player + kTimeScale, std::bit_cast<uint32_t>(1.0f));
        gScaleOurs = false;
      }
      gScaleLast = gScaleOurs ? std::bit_cast<float>(c.mem.read<uint32_t>(player + kTimeScale)) : 1.0f;
    }
  }
  return gStateUpdate(c, entry);
}

class Bones {
public:
  explicit Bones(Cpu& c, uint32_t player) : c_(c) {
    const uint32_t model = c.mem.read<uint32_t>(player + 0x104);
    const uint32_t skeleton = c.mem.read<uint32_t>(model + 0x11C);
    base_ = c.mem.read<uint32_t>(skeleton + 0x10);
    ok_ = model != 0 && skeleton != 0 && base_ != 0 && c.mem.read<uint32_t>(skeleton + 0x14) == kBones;
  }
  bool ok() const { return ok_; }
  // The rotation the animation gave it this frame (it is not rewritten while time is stopped).
  Quat animated(int bone) {
    const Quat now = read(bone);
    if (!gTouched[size_t(bone)] || now != gWritten[size_t(bone)]) gOriginal[size_t(bone)] = now;
    return gOriginal[size_t(bone)];
  }
  // Turns a bone about an axis of the model, whatever its parents do. `parent` is the rotation of
  // the model's root down to the bone's parent.
  void turn(int bone, const Quat& parent, const Quat& inModel) {
    const Quat q = mul(mul(mul(conj(parent), inModel), parent), animated(bone));
    write(bone, q);
    gWritten[size_t(bone)] = q, gTouched[size_t(bone)] = true;
    invalidate(base_ + uint32_t(bone) * 0x80);
  }

  // Moves a bone `gain` times as far from where it usually is (the mean of its last positions), then
  // by a distance along the model's axes; `parent` as above.
  void swing(int bone, const Quat& parent, float gain, float x, float y, float z) {
    const Quat now = read(bone, 0x10);
    const size_t b = size_t(bone);
    if (!gTouchedAt[b] || now != gWrittenAt[b]) {  // the animation moved it: a new frame of it
      gOriginalAt[b] = now;
      if (!gCentreValid[b]) gCentre[b] = now, gCentreValid[b] = true;
      for (int i = 0; i < 3; ++i) gCentre[b][size_t(i)] += (now[size_t(i)] - gCentre[b][size_t(i)]) * kCentreRate;
    }
    const Quat d = mul(mul(conj(parent), Quat{x, y, z, 0}), parent);
    Quat at = gOriginalAt[b];
    for (int i = 0; i < 3; ++i) at[size_t(i)] = gCentre[b][size_t(i)] + (at[size_t(i)] - gCentre[b][size_t(i)]) * gain + d[size_t(i)];
    std::memcpy(c_.mem.hostPtr(base_ + uint32_t(bone) * 0x80 + 0x10, 16), at.data(), 16);
    gWrittenAt[size_t(bone)] = at, gTouchedAt[size_t(bone)] = true;
    invalidate(base_ + uint32_t(bone) * 0x80);
  }

private:
  Quat read(int bone, uint32_t offset = 0) {
    Quat q;
    std::memcpy(q.data(), c_.mem.hostPtr(base_ + uint32_t(bone) * 0x80 + offset, 16), 16);
    return q;
  }
  void write(int bone, const Quat& q) { std::memcpy(c_.mem.hostPtr(base_ + uint32_t(bone) * 0x80, 16), q.data(), 16); }
  void invalidate(uint32_t node) {
    uint8_t* flags = c_.mem.hostPtr(node + 0x6E, 2);
    uint16_t f;
    std::memcpy(&f, flags, 2);
    f &= uint16_t(~0x18u);
    std::memcpy(flags, &f, 2);
    for (uint32_t child = c_.mem.read<uint32_t>(node + 0x64); child != 0; child = c_.mem.read<uint32_t>(child + 0x68)) invalidate(child);
  }
  Cpu& c_;
  uint32_t base_ = 0;
  bool ok_ = false;
};

uint32_t playerPose(Cpu& c, uint32_t entry) {
  const uint32_t player = c.mem.read<uint32_t>(kPlayer);
  if (gEnabled && gBlend > 0 && player != 0 && uint32_t(c.s.gpr[4].ud[0]) == player) {
    Bones bones(c, player);
    if (bones.ok()) {
      const Quat pelvis = bones.animated(kPelvis);
      bones.turn(kSpine, pelvis, aboutX(kLean * gBlend));
      // The spine's own turn is in what its children hang from.
      const Quat leaned = mul(mul(mul(conj(pelvis), aboutX(kLean * gBlend)), pelvis), bones.animated(kSpine));
      const Quat chest = mul(mul(pelvis, leaned), bones.animated(kChest));
      bones.turn(kNeck, chest, aboutX(kHeadUp * gBlend));
      // The arms pump harder: the run's own swing, wider, the hands a little higher (elbows bent).
      const float gain = 1.0f + (kArmGain - 1.0f) * gBlend;
      bones.swing(kHandTargetA, chest, gain, 0, -kHandsUp * gBlend, 0);
      bones.swing(kHandTargetB, chest, gain, 0, -kHandsUp * gBlend, 0);
    }
  } else if (gBlend == 0) {
    gTouched.fill(false), gTouchedAt.fill(false), gCentreValid.fill(false);
  }
  return gPose(c, entry);
}
}  // namespace

void installSprint(Cpu& cpu) {
  Cpu::RecompFn& state = recompSlot(cpu, kStateUpdate);
  Cpu::RecompFn& pose = recompSlot(cpu, kPlayerPose);
  if (!state || !pose) return;
  gStateUpdate = state, state = stateUpdate;
  gPose = pose, pose = playerPose;
}

void setSprint(bool on) { gEnabled = on; }
