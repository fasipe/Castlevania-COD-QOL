// The game's recompiled code and compiled VU micro programs live in codport_game.dll, next to the
// program, built from the player's own disc by Setup.bat (or recompile.bat). Without it the
// interpreter runs everything, slowly.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>

#include "ee/vu.h"
#include "recomp/runtime.h"

namespace {
// A DLL next to the program, or null.
HMODULE load(const wchar_t* name) {
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  std::wstring dll(path, n);
  dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + name;
  return LoadLibraryW(dll.c_str());
}
// The functions the game runs and the VU programs, and the rest of the functions (built after).
HMODULE gameCode() {
  static HMODULE module = load(L"codport_game.dll");
  return module;
}
HMODULE gameCodeCold() {
  static HMODULE module = load(L"codport_game_cold.dll");
  return module;
}
}  // namespace

void registerRecompiled(std::vector<rc::Entry>& out) {
  using Fn = void (*)(std::vector<rc::Entry>&);
  for (HMODULE m : {gameCode(), gameCodeCold()})
    if (m)
      if (auto f = reinterpret_cast<Fn>(GetProcAddress(m, "codportRecompiled"))) f(out);
}

void registerVuBlocks(std::vector<VuBlockEntry>& out) {
  using Fn = void (*)(std::vector<VuBlockEntry>&);
  if (HMODULE m = gameCode())
    if (auto f = reinterpret_cast<Fn>(GetProcAddress(m, "codportVuBlocks"))) f(out);
}
