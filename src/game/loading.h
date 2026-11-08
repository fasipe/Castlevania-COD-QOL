#pragma once

#include <cstdint>

#include "ee/memory.h"

// Whether the game is between places: its event flag for doors and loading is up (bit 15 of the
// flags at 0xAED030, the same FUN_0043EC98(0x1000000F) the camera asks) and the player is gone.
// Disc reads are instant in this port, so all it does then is count frames, at a tenth of its usual
// work, and a frame takes 1 to 3 ms: nothing needs to wait for real time. The picture does not come
// back until the player does, and frames are the same as ever, so replays are not affected.
inline bool gameIsLoading(Memory& mem) {
  return (mem.read<uint32_t>(0x00AED030) & (1u << 15)) != 0 && mem.read<uint32_t>(0x00B06880) == 0;
}
