#pragma once

class Cpu;

// With an enemy locked on, attacks turn to it, whatever the stick says: the game's lock-on only
// aims the camera, and a hit goes where the character happens to face. Each hit of a combo follows
// the enemy for a few frames from its start. Changes the game's behaviour, so a replay has it only
// if its recording does. Not available with --interp (no recompiled code to hook).
void installAttackTurn(Cpu& cpu);
void setAttackTurn(bool on);
