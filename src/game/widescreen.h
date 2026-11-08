#pragma once

#include <cstdint>

class Cpu;

// True 16:9 (F10 / the settings' 16:9): the game's 3D camera shows a wider view instead of the
// 4:3 picture being stretched, full-screen movies keep their 4:3 shape in the middle, and the 2D
// interface keeps its shape while playing (the GS narrows what is drawn in the game's 2D layer). Based on
// the PCSX2 widescreen patch for SLES-53755 (by nemesis2000), applied as hooks on the recompiled
// functions it changes, so it can be switched on and off while the game runs. Not available with
// --interp (no recompiled code to hook).
void installWidescreen(Cpu& cpu);

uint64_t statusDisplayUpdates();  // counts the updates of the game's status display (HP and so on)
