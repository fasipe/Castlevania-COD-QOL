#pragma once
#include <filesystem>
#include <string>

class Memory;

// Static recompiler: translates the game's MIPS functions (listed by the Ghidra export
// tools/recomp/functions.txt) into C++ source files in `outDir`, one C++ function per game
// function. `onlyList` (optional) restricts it to the functions named there (hex addresses, one
// per line), e.g. the ones a run actually called. Returns false and fills `error` on failure.
bool generateRecompiledCode(Memory& mem, const std::filesystem::path& functionsFile,
                            const std::filesystem::path& onlyList, const std::filesystem::path& outDir,
                            std::string& error);
