#include "gs/shader_cache.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gs/frame_gen.h"
#include "gs/gpu.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char kMagic[8] = {'C', 'P', 'S', 'H', 'A', 'D', 'E', '1'};

uint64_t keyOf(const std::string& source, const char* target) {
  uint64_t h = 0xCBF29CE484222325ull;
  auto add = [&](const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(p[i])) * 0x100000001B3ull;
  };
  add(target, std::strlen(target) + 1);
  add(source.data(), source.size());
  return h;
}

// A blob of bytecode from the file (ID3DBlob without the compiler's D3DCreateBlob).
class Blob final : public ID3DBlob {
public:
  explicit Blob(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (id == __uuidof(IUnknown) || id == __uuidof(ID3D10Blob)) {
      AddRef();
      *out = static_cast<ID3DBlob*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes_.data(); }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes_.size(); }

private:
  std::vector<uint8_t> bytes_;
  std::atomic<ULONG> refs_{1};
};

std::mutex gMutex;
std::unordered_map<uint64_t, std::vector<uint8_t>>* gCache = nullptr;  // loaded on first use
std::unordered_map<uint64_t, std::vector<uint8_t>>* gRecording = nullptr;  // --shader-cache

std::filesystem::path cacheFile() {
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return std::filesystem::path(std::wstring(path, n)).parent_path() / L"shaders.bin";
}

const std::unordered_map<uint64_t, std::vector<uint8_t>>& cache() {
  if (!gCache) {
    gCache = new std::unordered_map<uint64_t, std::vector<uint8_t>>;
    std::ifstream in(cacheFile(), std::ios::binary);
    char magic[8]{};
    uint32_t count = 0;
    if (in.read(magic, 8) && std::memcmp(magic, kMagic, 8) == 0 && in.read(reinterpret_cast<char*>(&count), 4)) {
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t key = 0;
        uint32_t size = 0;
        if (!in.read(reinterpret_cast<char*>(&key), 8) || !in.read(reinterpret_cast<char*>(&size), 4) || size > (64u << 20)) break;
        std::vector<uint8_t> bytes(size);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) break;
        (*gCache)[key] = std::move(bytes);
      }
    }
  }
  return *gCache;
}
}  // namespace

ComPtr<ID3DBlob> compileShader(const std::string& source, const char* target, std::string& error) {
  const uint64_t key = keyOf(source, target);
  {
    std::lock_guard lock(gMutex);
    if (!gRecording) {
      const auto& c = cache();
      if (auto it = c.find(key); it != c.end()) {
        ComPtr<ID3DBlob> blob;
        blob.Attach(new Blob(it->second));
        return blob;
      }
    }
  }
  // CODPORT_NO_HLSL: as where there is no compiler (to check that shaders.bin has everything).
  static const bool noCompiler = std::getenv("CODPORT_NO_HLSL") != nullptr;
  if (noCompiler && !gRecording) return error = "shader not in shaders.bin (CODPORT_NO_HLSL)", nullptr;
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &code, &errors))) {
    error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "shader compilation failed";
    return nullptr;
  }
  std::lock_guard lock(gMutex);
  if (gRecording) {
    const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    (*gRecording)[key] = std::vector<uint8_t>(p, p + code->GetBufferSize());
  }
  return code;
}

bool writeShaderCache(const std::filesystem::path& file, std::string& error) {
  std::unordered_map<uint64_t, std::vector<uint8_t>> recorded;
  {
    std::lock_guard lock(gMutex);
    gRecording = &recorded;
  }
  bool ok = true;
  for (uint32_t scale = 1; scale <= 4 && ok; ++scale) ok = GsGpu::create(nullptr, scale, error) != nullptr;
  if (ok) {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ok = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    if (!ok) error = "no Direct3D 11 device";
    FrameGen gen;
    if (ok) ok = gen.init(dev.Get(), ctx.Get(), error);
  }
  {
    std::lock_guard lock(gMutex);
    gRecording = nullptr;
  }
  if (!ok) return false;
  std::ofstream out(file, std::ios::binary);
  const uint32_t count = uint32_t(recorded.size());
  out.write(kMagic, 8);
  out.write(reinterpret_cast<const char*>(&count), 4);
  for (const auto& [key, bytes] : recorded) {
    const uint32_t size = uint32_t(bytes.size());
    out.write(reinterpret_cast<const char*>(&key), 8);
    out.write(reinterpret_cast<const char*>(&size), 4);
    out.write(reinterpret_cast<const char*>(bytes.data()), size);
  }
  if (!out) return error = "could not write " + file.string(), false;
  std::printf("%u shaders written to %s\n", count, file.string().c_str());
  return true;
}
