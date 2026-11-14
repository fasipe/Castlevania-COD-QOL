#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

// Texture packs: the game's textures saved as PNG files named by a hash of their contents, and
// PNG files with those names used in their place, at any size (an upscaled texture is sampled the
// same way). In textures/ next to the memory card folder: textures/dump gets the game's textures
// when dumping is on (settings: [texturas]), textures/replace is the pack. Only textures the game
// uploads whole (not pictures it draws) have a hash; their alpha is saved doubled (the PS2's 128 is
// opaque), so a PNG's 255 is opaque.
class TexturePack {
public:
  void open(const std::filesystem::path& folder, bool use, bool dump);
  bool dumping() const { return dump_; }
  // The first time a hash is seen while dumping: true, and its file is then to be written.
  bool shouldDump(uint64_t hash);
  std::filesystem::path dumpFile(uint64_t hash) const;
  const std::filesystem::path* replacement(uint64_t hash) const;
  static std::string name(uint64_t hash);  // 16 hex digits

private:
  std::filesystem::path folder_;
  bool dump_ = false;
  std::unordered_set<uint64_t> dumped_;
  std::unordered_map<uint64_t, std::filesystem::path> replacements_;
};
