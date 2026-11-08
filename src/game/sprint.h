#pragma once

class Cpu;

// A sprint: L3 makes the character run faster until it stops running or L3 is pressed again; the
// press counts just before the run starts as well. The game has no animation for it, so it is made
// here over the run: longer strides at only a slightly quicker cycle, the torso leaning forward and
// the arms pumping wider than the run swings them. Changes the game's behaviour, so a replay has it
// only if its recording does. Not available with --interp (no recompiled code to hook).
void installSprint(Cpu& cpu);
void setSprint(bool on);
