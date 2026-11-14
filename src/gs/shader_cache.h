#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3dcommon.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>

// Shader bytecode for the graphics card: taken from shaders.bin next to codport.exe when that
// holds this source (written by `codport --shader-cache`, so that no HLSL compiler is needed where
// the system's is missing or incomplete, as under Wine/Proton on Linux and the Steam Deck),
// compiled with D3DCompile otherwise.
Microsoft::WRL::ComPtr<ID3DBlob> compileShader(const std::string& source, const char* target, std::string& error);

// `codport --shader-cache FILE`: compiles every shader of the GPU renderer (at every resolution
// scale) and of the interpolation, and writes them to FILE.
bool writeShaderCache(const std::filesystem::path& file, std::string& error);
