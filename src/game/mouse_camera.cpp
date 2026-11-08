#include "game/mouse_camera.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "ee/cpu.h"
#include "game/hooks.h"

namespace {
// In FUN_0044CF58, the camera's update: where FUN_0044BC80 returns, having put the right stick
// (-1..1 past the dead zone, the game's own vertical inversion applied) at sp+0x30 (x) and sp+0x34
// (y). s1 holds the camera; the update then adds -x * speed to its yaw (+0x1E4) and y * speed to
// its pitch (+0x1E0, kept between +0x248 and +0x244), speed being +0x24C, radians a frame at the
// end of the stick. Those angles say where the camera should be: the point it looks at, +0x200,
// plus (cos pitch sin yaw, sin pitch, cos pitch cos yaw) times the distance (y points down). But
// its place, +0x210, only eases toward there, a part of the way each frame, and the camera itself,
// +0x1F0, eases toward its place: that is the stick's slow start and finish.
constexpr uint32_t kAfterInput = 0x0044CFC0;
constexpr uint32_t kStickX = 0x30, kStickY = 0x34, kStickPush = 0x38;
constexpr uint32_t kPitch = 0x1E0, kPosition = 0x1F0, kLookAt = 0x200, kGoal = 0x210, kPitchMax = 0x244, kPitchMin = 0x248,
                   kSpeed = 0x24C;

Cpu::RecompFn gAfterInput = nullptr;
float gYaw = 0, gPitch = 0;
int gUntaken = 0;  // frames queued since the camera last took the turn (menus, movies: none)

float readFloat(Cpu& c, uint32_t address) { return std::bit_cast<float>(c.mem.read<uint32_t>(address)); }
void writeFloat(Cpu& c, uint32_t address, float v) { c.mem.write<uint32_t>(address, std::bit_cast<uint32_t>(v)); }

uint32_t afterInput(Cpu& c, uint32_t entry) {
  gUntaken = 0;
  if (gYaw != 0 || gPitch != 0) {
    const uint32_t camera = uint32_t(c.s.gpr[17].ud[0]), sp = uint32_t(c.s.gpr[29].ud[0]);
    const float speed = camera ? readFloat(c, camera + kSpeed) : 0;
    if (speed > 1e-6f) {
      // The target turns by the mouse's motion, through the stick's value (the pitch as far as
      // its limits let it).
      const float pitch = readFloat(c, camera + kPitch);
      const float pitchTurn =
          std::clamp(pitch + gPitch, readFloat(c, camera + kPitchMin), readFloat(c, camera + kPitchMax)) - pitch;
      writeFloat(c, sp + kStickX, readFloat(c, sp + kStickX) - gYaw / speed);
      writeFloat(c, sp + kStickY, readFloat(c, sp + kStickY) + pitchTurn / speed);
      // The pitch only moves with the stick's push (sp+0x38) past 0.1.
      if (pitchTurn != 0) writeFloat(c, sp + kStickPush, std::max(readFloat(c, sp + kStickPush), 0.11f));
      // And the camera and its place turn with it, around the point it looks at, so that neither
      // eases into the turn: each still as far behind as before, but in the turned direction.
      float at[3];
      for (int i = 0; i < 3; ++i) at[i] = readFloat(c, camera + kLookAt + 4 * i);
      for (uint32_t point : {camera + kPosition, camera + kGoal}) {
        float e[3];
        for (int i = 0; i < 3; ++i) e[i] = readFloat(c, point + 4 * i) - at[i];
        const float r = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (r < 1e-3f) continue;
        const float yaw = std::atan2(e[0], e[2]) + gYaw;
        const float elevation = std::asin(std::clamp(e[1] / r, -1.0f, 1.0f)) + pitchTurn;
        const float flat = r * std::cos(elevation);
        writeFloat(c, point, at[0] + flat * std::sin(yaw));
        writeFloat(c, point + 4, at[1] + r * std::sin(elevation));
        writeFloat(c, point + 8, at[2] + flat * std::cos(yaw));
      }
    }
    gYaw = gPitch = 0;
  }
  return gAfterInput(c, entry);
}
}  // namespace

void installMouseCamera(Cpu& cpu) {
  Cpu::RecompFn& slot = recompSlot(cpu, kAfterInput);
  if (!slot) return;
  gAfterInput = slot, slot = afterInput;
}

void queueCameraTurn(float yaw, float pitch) {
  // While nothing takes it (a menu, a movie), the mouse's motion is dropped rather than saved up
  // for a jump when the camera is back.
  if (++gUntaken > 2) gYaw = gPitch = 0;
  gYaw += yaw, gPitch += pitch;
}
