#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

// Screenshots of the window as shown (the picture, the minimap; not the frame counter), saved as
// PNG: F12 in the window (screenshots/ next to the memory card folder), --shot N in a replay.
void requestScreenshot(const std::filesystem::path& file);
// The request, if there is one (taken: the next call returns false).
bool takeScreenshotRequest(std::filesystem::path& file);
// Writes width x height RGBA pixels (rows `pitch` bytes apart) as a PNG.
bool savePng(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint8_t* rgba, uint32_t pitch);
// Reads a PNG (or any image Windows decodes) as RGBA pixels, rows width * 4 bytes apart.
bool loadPng(const std::filesystem::path& file, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);
