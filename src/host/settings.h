#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

// The player's settings, kept in codport.ini next to the memory card folder: which keyboard key
// and which Xbox controller input give each control of the PS2 controller, and how the window
// shows the picture. Missing or unreadable entries keep their defaults; the file is rewritten
// whenever a setting changes.
struct Settings {
  // The controls that can be bound, in this order: the 16 buttons (bit i of the PS2 button
  // word), then the left and right sticks pushed up, down, left and right.
  enum : int { kButtons = 16, kLeftUp = 16, kLeftDown, kLeftLeft, kLeftRight, kRightUp, kRightDown, kRightLeft,
               kRightRight, kControls };
  static const char* id(int control);    // name in the file
  static const char* label(int control); // what it does in the game, shown to the player
  static const char* ps2Name(int control);  // its button on the PS2 controller

  // Keyboard: a Windows virtual-key code per control (0 = none). The same key may drive several
  // controls (the arrows give the D-pad and the left stick).
  std::array<uint8_t, kControls> keys{};
  // Controller: per button, an XInput button bit, or kLeftTrigger / kRightTrigger, or 0. The
  // sticks are always the controller's sticks.
  enum : uint16_t { kLeftTrigger = 0x0400, kRightTrigger = 0x0800 };
  std::array<uint16_t, kButtons> pad{};

  std::string iso;  // the game's disc image, UTF-8 (set by the setup, or when the game asks for it)
  bool fullscreen = false;
  int windowWidth = 960, windowHeight = 720;  // the window's inside, out of full screen
  bool wide = false;  // the picture shown 16:9 instead of 4:3
  int scale = 1;      // drawing resolution, 1-4 times the PS2's (read at start)
  bool showFps = true;  // frames per second in the top right corner (F9)
  bool minimap = true;  // the map in the corner of the screen (F8)
  bool textureFilter = true;  // as the PS2 filters (read at start)
  bool fastLoading = true;  // between places, no waiting for real time (nor its sound)
  bool sprint = true;  // L3 sprints (read at start)
  bool attackTurn = true;  // with an enemy locked on, attacks turn to it (read at start)

  static Settings defaults();
  void load(const std::filesystem::path& file);
  void save() const;
  std::filesystem::path file;
};

Settings& settings();  // the ones in use

std::string keyName(int vk);           // as written in the file and shown ("Z", "Enter", "F1")
int keyFromName(const std::string& name);
std::string padName(uint16_t input);   // "A", "LB", "LT"...
uint16_t padFromName(const std::string& name);
