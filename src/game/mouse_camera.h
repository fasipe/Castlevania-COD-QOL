#pragma once

class Cpu;

// The mouse turns the camera directly: its motion is added to the camera's angles in the frame it
// happens, as much as it moved, with none of the stick's limits (the stick turns the camera at a
// rate, at most about 76 degrees a second). Done where the game turns the camera by the right
// stick (FUN_0044CF58, after FUN_0044BC80 has read the stick into its input): the stick's value is
// made to include the mouse's turn. Not available with --interp (no recompiled code to hook).
void installMouseCamera(Cpu& cpu);

// The mouse's turn this frame, in radians: yaw to the left positive (as the stick pushed left
// turns it), pitch up positive (as the stick pushed down). Kept until the camera takes it.
void queueCameraTurn(float yaw, float pitch);
