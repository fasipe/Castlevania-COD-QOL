#pragma once
// What the game's recompiled code (codport_game.dll, built on the player's computer from their disc)
// uses of the program: exported by codport.exe, imported by the DLL.
#ifdef CODPORT_GAME_DLL
#define CODPORT_API __declspec(dllimport)
#else
#define CODPORT_API __declspec(dllexport)
#endif
