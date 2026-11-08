#pragma once
#include <cstdint>

struct PadInput;
struct _XINPUT_GAMEPAD;

// Sets the emulated controller (gPadInput) from the keyboard state merged with the first XInput
// controller, if one is connected, its buttons mapped as the settings say (sticks map directly).
void applyGamepad(const PadInput& keyboard);

// The controller inputs held, as Settings::pad names them: XInput button bits, with the triggers
// as Settings::kLeftTrigger / kRightTrigger.
uint16_t padInputs(const _XINPUT_GAMEPAD& g);
