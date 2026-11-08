#pragma once

#include <cstdint>
#include <vector>

class Cpu;
class Iso9660;

// A map in the corner of the screen, only while the game is being played (no story scenes, menus,
// messages, doors or loading): the rooms of the current area as the Select screen's map draws
// them, around the player. The rooms' shapes are
// read from the disc, what has been seen from the game's memory; nothing in the game changes, so
// replays are not affected.
struct MinimapVertex {
  float x, z;  // on the map, x already mirrored as the map screen shows it
  uint32_t rgba;
};

struct MinimapView {
  bool visible = false;
  float x = 0, z = 0;        // the player on the map
  float dirX = 0, dirZ = 1;  // and where the character faces
  float alpha = 1;           // it fades in and out
  uint64_t version = 0;      // of the triangles
};

void installMinimap(const Iso9660& iso);
void updateMinimap(Cpu& cpu);  // once per frame, on the emulation thread

// For the renderer, on its own thread: the newest view, and the triangles if they are not `have`.
MinimapView minimapView(std::vector<MinimapVertex>& triangles, uint64_t have);
