#pragma once
#include <filesystem>
#include <string>

class Iso9660;

// The program's icon: the 3D bust of Hector that the game shows in the PS2's memory card browser,
// read from the player's disc and drawn into a Windows .ico (256, 48, 32 and 16 pixels). Built on
// the player's computer, like the game's code, so none of the game's art is in this repository.
bool makeIcon(const Iso9660& iso, const std::filesystem::path& out, std::string& error);

// The icon next to the program (build\castlevania.ico).
std::filesystem::path iconFile();
