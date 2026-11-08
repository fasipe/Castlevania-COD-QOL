#pragma once
#include <cstdint>
#include <memory>

#include "iop/sif.h"

// PADMAN.IRX for the game's libpad (the older protocol: one RPC function, command in word 0).
// Port 0 has a DualShock 2; the IOP refreshes the EE's 2x0x80-byte pad area every VBlank.
enum : uint32_t { kPadServer = 0x80000100, kPadExtServer = 0x80000101 };

// Controller state in PS2 terms; filled by the host input layer (milestone 4).
struct PadInput {
  uint16_t buttons = 0;              // bit set = pressed (SELECT=0x0001 ... SQUARE=0x8000)
  uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
  uint8_t pressure[12] = {};         // right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
  // Not the PS2's: the mouse's camera turn this frame, in radians (game/mouse_camera.h).
  float turnYaw = 0, turnPitch = 0;
};
extern PadInput gPadInput;
// Sets the buttons (pressure-sensitive ones fully pressed) and the sticks (left: bits 1 up, 2 down,
// 4 left, 8 right; right: 16, 32, 64, 128; centred when none).
void setPad(PadInput& pad, uint16_t buttons, uint8_t stick);

std::unique_ptr<Sif::RpcServer> makePadServer(uint32_t sid);
