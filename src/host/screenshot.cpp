#include "host/screenshot.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <mutex>
#include <optional>
#include <vector>

namespace {
std::mutex gMutex;
std::optional<std::filesystem::path> gRequest;
}  // namespace

void requestScreenshot(const std::filesystem::path& file) {
  std::lock_guard lock(gMutex);
  gRequest = file;
}

bool takeScreenshotRequest(std::filesystem::path& file) {
  std::lock_guard lock(gMutex);
  if (!gRequest) return false;
  file = *gRequest;
  gRequest.reset();
  return true;
}

bool savePng(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint8_t* rgba, uint32_t pitch) {
  using Microsoft::WRL::ComPtr;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    // Opaque: the window's pixels, whatever their alpha.
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* s = rgba + size_t(y) * pitch + x * 4;
        uint8_t* d = pixels.data() + (size_t(y) * width + x) * 4;
        d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255;
      }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format));
    if (ok && format != GUID_WICPixelFormat32bppRGBA) {  // the encoder wants another layout: convert
      ComPtr<IWICBitmap> bitmap;
      ComPtr<IWICFormatConverter> converter;
      ok = SUCCEEDED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA, width * 4, UINT(pixels.size()),
                                                     pixels.data(), &bitmap)) &&
           SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
           SUCCEEDED(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
           SUCCEEDED(frame->WriteSource(converter.Get(), nullptr));
    } else if (ok) {
      ok = SUCCEEDED(frame->WritePixels(height, width * 4, UINT(pixels.size()), pixels.data()));
    }
    ok = ok && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}

bool loadPng(const std::filesystem::path& file, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) {
  using Microsoft::WRL::ComPtr;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
         SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
         SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) &&
         SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 16384 && h <= 16384;
    if (ok) {
      rgba.resize(size_t(w) * h * 4);
      ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
      width = w, height = h;
    }
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}
