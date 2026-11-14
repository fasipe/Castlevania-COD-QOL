#include "gs/texture_pack.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

void TexturePack::open(const std::filesystem::path& folder, bool use, bool dump) {
  folder_ = folder, dump_ = dump;
  std::error_code ec;
  if (dump) std::filesystem::create_directories(folder / "dump", ec);
  for (const auto& e : std::filesystem::directory_iterator(folder / "dump", ec))  // dumped before: not again
    if (e.path().extension() == ".png") dumped_.insert(std::strtoull(e.path().stem().string().c_str(), nullptr, 16));
  if (!use) return;
  // In order of their paths, a later one taking a texture another has too: the packs Texture Pack
  // Mod installs are folders numbered in the order they were installed, the newest winning.
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::recursive_directory_iterator(folder / "replace", ec)) {
    const std::string stem = e.path().stem().string();
    if (e.path().extension() != ".png" || stem.size() != 16 || stem.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
      continue;
    files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) replacements_[std::strtoull(f.stem().string().c_str(), nullptr, 16)] = f;
  if (!replacements_.empty()) std::printf("texture pack: %zu textures in %s\n", replacements_.size(), (folder / "replace").string().c_str());
}

bool TexturePack::shouldDump(uint64_t hash) { return dump_ && dumped_.insert(hash).second; }

std::filesystem::path TexturePack::dumpFile(uint64_t hash) const { return folder_ / "dump" / (name(hash) + ".png"); }

const std::filesystem::path* TexturePack::replacement(uint64_t hash) const {
  const auto it = replacements_.find(hash);
  return it == replacements_.end() ? nullptr : &it->second;
}

std::string TexturePack::name(uint64_t hash) { return std::format("{:016x}", hash); }
