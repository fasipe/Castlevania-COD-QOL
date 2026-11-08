#include "game/attack_turn.h"

#include <bit>
#include <cmath>
#include <numbers>

#include "ee/cpu.h"
#include "game/hooks.h"

namespace {
// The player's state, all in the block for player 0 (FUN_0045a940 updates it each frame).
constexpr uint32_t kActionFlags = 0x00B06698;  // 0x280: in an attack; 0xF000: which hit of a combo
constexpr uint32_t kMovement = 0x00B06780;     // what FUN_0046C240 gets for the player
constexpr uint32_t kPosition = 0x00B06790;     // x, y, z
constexpr uint32_t kYaw = 0x00B067AC;          // facing (sin yaw, cos yaw) on (x, z), set directly by the game too
constexpr uint32_t kLockPosition = 0x00B06810;  // kept up to date by FUN_0046B550, the lock-on
constexpr uint32_t kLockActive = 0x00B0683C;    // 16 bits
constexpr uint32_t kLockTarget = 0x00B06840;    // -1: none

// Runs after this frame's lock-on and animation scripts, so it has the last word over a turn the
// stick asked for, and before the movement that uses the facing.
constexpr uint32_t kMovementUpdate = 0x0046C240;

// How long an attack keeps following the enemy, and how fast: the game's own turn with the stick
// between hits takes about a radian per frame.
constexpr int kFollowFrames = 8;
constexpr float kMaxTurn = 1.0f;

bool gEnabled = false;
Cpu::RecompFn gMovementUpdate = nullptr;

float readFloat(Cpu& c, uint32_t address) { return std::bit_cast<float>(c.mem.read<uint32_t>(address)); }

uint32_t movementUpdate(Cpu& c, uint32_t entry) {
  static uint16_t lastFlags = 0;
  static int follow = 0;
  if (gEnabled && uint32_t(c.s.gpr[4].ud[0]) == kMovement) {
    const uint16_t flags = c.mem.read<uint16_t>(kActionFlags);
    const uint16_t hit = flags & 0xF000;
    if (((flags & 0x280) && !(lastFlags & 0x280)) || (hit && hit != (lastFlags & 0xF000))) follow = kFollowFrames;
    lastFlags = flags;
    if (follow > 0) {
      --follow;
      if (c.mem.read<uint16_t>(kLockActive) != 0 && c.mem.read<uint32_t>(kLockTarget) != 0xFFFFFFFFu) {
        const float dx = readFloat(c, kLockPosition) - readFloat(c, kPosition);
        const float dz = readFloat(c, kLockPosition + 8) - readFloat(c, kPosition + 8);
        const float yaw = readFloat(c, kYaw);
        const float pi = std::numbers::pi_v<float>;
        float turn = std::atan2(dx, dz) - yaw;
        turn -= std::round(turn / (2 * pi)) * (2 * pi);
        turn = std::fmod(yaw + std::fmin(kMaxTurn, std::fmax(-kMaxTurn, turn)) + 3 * pi, 2 * pi) - pi;
        c.mem.write<uint32_t>(kYaw, std::bit_cast<uint32_t>(turn));
      }
    }
  }
  return gMovementUpdate(c, entry);
}
}  // namespace

void installAttackTurn(Cpu& cpu) {
  Cpu::RecompFn& update = recompSlot(cpu, kMovementUpdate);
  if (!update) return;
  gMovementUpdate = update, update = movementUpdate;
}

void setAttackTurn(bool on) { gEnabled = on; }
