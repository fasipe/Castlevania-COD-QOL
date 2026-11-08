// The game's recompiled code and compiled VU micro programs live in DLLs next to the program, built
// from the player's own disc by Setup.bat (or recompile.bat), in parts so the game can start
// before all of it is built: codport_game.dll (what the game runs from its start),
// codport_game_vu.dll (the VU programs), codport_game_hot.dll (the rest of what the game runs) and
// codport_game_cold.dll (everything else). The interpreter runs whatever is not there yet; a part
// built while the game runs is loaded then (newGameCode). Without any of them the interpreter runs
// everything, slowly.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "ee/vu.h"
#include "recomp/runtime.h"

namespace {
std::filesystem::path besideProgram(const wchar_t* name) {
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return std::filesystem::path(std::wstring(path, n)).parent_path() / name;
}

struct Part {
  const wchar_t* dll;
  const wchar_t* ready;  // written by tools/setup/build-game.bat once the DLL is built
  HMODULE module = nullptr;
};
Part gParts[] = {{L"codport_game.dll", nullptr},
                 {L"codport_game_vu.dll", L"codport_game_vu.ready"},
                 {L"codport_game_hot.dll", L"codport_game_hot.ready"},
                 {L"codport_game_cold.dll", L"codport_game_cold.ready"}};

void entriesOf(HMODULE m, std::vector<rc::Entry>& out) {
  using Fn = void (*)(std::vector<rc::Entry>&);
  if (auto f = reinterpret_cast<Fn>(GetProcAddress(m, "codportRecompiled"))) f(out);
}
void vuBlocksOf(HMODULE m, std::vector<VuBlockEntry>& out) {
  using Fn = void (*)(std::vector<VuBlockEntry>&);
  if (auto f = reinterpret_cast<Fn>(GetProcAddress(m, "codportVuBlocks"))) f(out);
}

// The parts there at start (a part still being written fails to load, and is loaded later).
void loadAtStart() {
  static bool done = false;
  if (done) return;
  done = true;
  for (Part& p : gParts) p.module = LoadLibraryW(besideProgram(p.dll).c_str());
}
}  // namespace

void registerRecompiled(std::vector<rc::Entry>& out) {
  loadAtStart();
  for (const Part& p : gParts)
    if (p.module) entriesOf(p.module, out);
}

void registerVuBlocks(std::vector<VuBlockEntry>& out) {
  loadAtStart();
  for (const Part& p : gParts)
    if (p.module) vuBlocksOf(p.module, out);
}

// The entry points of the parts built since the last call (the setup builds them while the game
// runs), for the caller to add where nothing is recompiled yet.
bool newGameCode(std::vector<rc::Entry>& out) {
  bool any = false;
  for (Part& p : gParts) {
    if (p.module || !p.ready) continue;
    std::error_code ec;
    if (!std::filesystem::exists(besideProgram(p.ready), ec)) continue;
    if (!(p.module = LoadLibraryW(besideProgram(p.dll).c_str()))) continue;
    entriesOf(p.module, out);
    std::vector<VuBlockEntry> blocks;
    vuBlocksOf(p.module, blocks);
    if (!blocks.empty()) addVuBlocks(blocks);
    any = true;
  }
  return any;
}
