#pragma once

// F1: a window with two tabs. Controls lists every action of the game with its keyboard key and
// Xbox controller button, where the player clicks an action and presses the new key or button;
// Options has the port's settings (full screen, 16:9, resolution, texture filter, minimap...).
// Modal: the game waits while it is open. Changes go to the settings file at once.
void showControlsDialog(void* owner);  // owner: the game window's HWND
