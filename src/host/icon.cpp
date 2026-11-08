#include "host/icon.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "iso9660.h"

namespace {
// The memory card icon (a PS2 .ico: one shape, a compressed 128x128 texture) is stored as is in
// FILEDATA.BIN, at this offset on SLES-53755.
constexpr uint32_t kIconOffset = 0xD9900;
constexpr uint32_t kIconSize = 77058;

struct Vertex {
  float x, y, z, nx, ny, nz, u, v;
};

uint16_t u16(const std::vector<uint8_t>& d, size_t at) { return uint16_t(d[at] | d[at + 1] << 8); }
uint32_t u32(const std::vector<uint8_t>& d, size_t at) { return u16(d, at) | uint32_t(u16(d, at + 2)) << 16; }
float s16(const std::vector<uint8_t>& d, size_t at) { return int16_t(u16(d, at)) / 4096.0f; }

bool parse(const std::vector<uint8_t>& d, std::vector<Vertex>& verts, std::vector<uint16_t>& texture) {
  if (d.size() < 20 || u32(d, 0) != 0x10000 || u32(d, 4) != 1 || (u32(d, 8) & 8) == 0) return false;
  const uint32_t count = u32(d, 16);
  size_t at = 20;
  if (count % 3 || at + size_t(count) * 24 + 20 > d.size()) return false;
  for (uint32_t i = 0; i < count; ++i, at += 24)
    verts.push_back({s16(d, at), s16(d, at + 2), s16(d, at + 4), s16(d, at + 8), s16(d, at + 10), s16(d, at + 12),
                     s16(d, at + 16), s16(d, at + 18)});
  const uint32_t frames = u32(d, at + 16);
  at += 20;
  for (uint32_t f = 0; f < frames; ++f) {
    if (at + 8 > d.size()) return false;
    at += 8 + size_t(u32(d, at + 4)) * 8;
  }
  if (at + 4 > d.size()) return false;
  const size_t end = at + 4 + u32(d, at);
  if (end > d.size()) return false;
  // Run-length coded 16-bit texels: a count below 0x8000 repeats the next texel, otherwise
  // 0x10000 - code texels follow as they are.
  for (at += 4; at + 2 <= end && texture.size() < 128 * 128;) {
    const uint16_t code = u16(d, at);
    at += 2;
    if (code & 0x8000) {
      for (uint32_t n = 0x10000 - code; n-- && at + 2 <= end; at += 2) texture.push_back(u16(d, at));
    } else if (at + 2 <= end) {
      texture.insert(texture.end(), code, u16(d, at));
      at += 2;
    }
  }
  if (texture.size() < 128 * 128) return false;
  texture.resize(128 * 128);
  return true;
}

using Rgba = std::array<float, 4>;  // premultiplied, 0..1

Rgba texel(const std::vector<uint16_t>& texture, float u, float v) {
  const float x = std::clamp(u * 128 - 0.5f, 0.0f, 127.0f), y = std::clamp(v * 128 - 0.5f, 0.0f, 127.0f);
  const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, 127), y1 = std::min(y0 + 1, 127);
  const float fx = x - x0, fy = y - y0;
  Rgba out{0, 0, 0, 1};
  const std::pair<int, float> taps[4] = {{y0 * 128 + x0, (1 - fx) * (1 - fy)}, {y0 * 128 + x1, fx * (1 - fy)},
                                          {y1 * 128 + x0, (1 - fx) * fy}, {y1 * 128 + x1, fx * fy}};
  for (auto [i, w] : taps)
    for (int c = 0; c < 3; ++c) out[c] += w * float(texture[i] >> (5 * c) & 31) / 31;
  return out;
}

// The bust seen from the front, size x size, with light from the upper left: all of it, or (close)
// the head and shoulders, which small icons need to be recognisable.
std::vector<Rgba> render(const std::vector<Vertex>& verts, const std::vector<uint16_t>& texture, int size, bool close) {
  float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
  for (const Vertex& v : verts) {
    const float p[3] = {v.x, v.y, v.z};
    for (int i = 0; i < 3; ++i) lo[i] = std::min(lo[i], p[i]), hi[i] = std::max(hi[i], p[i]);
  }
  const float height = hi[1] - lo[1];
  const float cx = (lo[0] + hi[0]) / 2, cy = close ? lo[1] + height * 0.33f : (lo[1] + hi[1]) / 2;
  const float extent = close ? height * 0.35f : std::max({hi[0] - lo[0], height, hi[2] - lo[2]}) * 0.52f;
  float light[3] = {0.3f, -0.5f, -0.8f};
  const float len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
  for (float& c : light) c /= len;

  std::vector<Rgba> image(size_t(size) * size, Rgba{0, 0, 0, 0});
  std::vector<float> depth(size_t(size) * size, 1e9f);
  auto sx = [&](const Vertex& v) { return ((v.x - cx) / extent * 0.5f + 0.5f) * size; };
  auto sy = [&](const Vertex& v) { return ((v.y - cy) / extent * 0.5f + 0.5f) * size; };  // y points down
  for (size_t t = 0; t + 2 < verts.size(); t += 3) {
    const Vertex& a = verts[t];
    const Vertex& b = verts[t + 1];
    const Vertex& c = verts[t + 2];
    const float ax = sx(a), ay = sy(a), bx = sx(b), by = sy(b), qx = sx(c), qy = sy(c);
    const float den = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy);
    if (std::abs(den) < 1e-9f) continue;
    const int x0 = std::max(0, int(std::min({ax, bx, qx}))), x1 = std::min(size - 1, int(std::max({ax, bx, qx})) + 1);
    const int y0 = std::max(0, int(std::min({ay, by, qy}))), y1 = std::min(size - 1, int(std::max({ay, by, qy})) + 1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float fx = x + 0.5f, fy = y + 0.5f;
        const float w0 = ((by - qy) * (fx - qx) + (qx - bx) * (fy - qy)) / den;
        const float w1 = ((qy - ay) * (fx - qx) + (ax - qx) * (fy - qy)) / den;
        const float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const float z = w0 * a.z + w1 * b.z + w2 * c.z;
        float& d = depth[size_t(y) * size + x];
        if (z >= d) continue;
        d = z;
        const float n[3] = {w0 * a.nx + w1 * b.nx + w2 * c.nx, w0 * a.ny + w1 * b.ny + w2 * c.ny,
                            w0 * a.nz + w1 * b.nz + w2 * c.nz};
        const float nl = std::max(1e-6f, std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
        const float shade = 0.55f + 0.6f * std::max(0.0f, -(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / nl);
        Rgba p = texel(texture, w0 * a.u + w1 * b.u + w2 * c.u, w0 * a.v + w1 * b.v + w2 * c.v);
        for (int i = 0; i < 3; ++i) p[i] = std::min(1.0f, p[i] * shade);
        image[size_t(y) * size + x] = p;
      }
  }
  return image;
}

// Averages blocks of pixels: from size to size / factor.
std::vector<Rgba> shrink(const std::vector<Rgba>& image, int size, int factor) {
  const int out = size / factor;
  std::vector<Rgba> small(size_t(out) * out, Rgba{});
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      for (int c = 0; c < 4; ++c) small[size_t(y / factor) * out + x / factor][c] += image[size_t(y) * size + x][c];
  for (Rgba& p : small)
    for (float& c : p) c /= float(factor * factor);
  return small;
}

// An icon image: a 32-bit bottom-up DIB (BGRA, straight alpha) and its AND mask.
std::vector<uint8_t> dib(const std::vector<Rgba>& image, int size) {
  const size_t maskRow = ((size + 31) / 32) * 4;
  std::vector<uint8_t> out(sizeof(BITMAPINFOHEADER) + size_t(size) * size * 4 + maskRow * size, 0);
  BITMAPINFOHEADER h{};
  h.biSize = sizeof h;
  h.biWidth = size;
  h.biHeight = size * 2;
  h.biPlanes = 1;
  h.biBitCount = 32;
  std::memcpy(out.data(), &h, sizeof h);
  uint8_t* px = out.data() + sizeof h;
  uint8_t* mask = px + size_t(size) * size * 4;
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const Rgba& p = image[size_t(size - 1 - y) * size + x];
      uint8_t* o = px + (size_t(y) * size + x) * 4;
      const float a = std::clamp(p[3], 0.0f, 1.0f);
      for (int c = 0; c < 3; ++c) o[2 - c] = uint8_t(a > 0 ? std::clamp(p[c] / a, 0.0f, 1.0f) * 255 + 0.5f : 0);
      o[3] = uint8_t(a * 255 + 0.5f);
      if (o[3] == 0) mask[y * maskRow + x / 8] |= uint8_t(0x80 >> (x % 8));
    }
  return out;
}
}  // namespace

bool makeIcon(const Iso9660& iso, const std::filesystem::path& out, std::string& error) {
  const auto pack = iso.find("FILEDATA.BIN");
  if (!pack || pack->size < kIconOffset + kIconSize) {
    error = "FILEDATA.BIN not found on the disc";
    return false;
  }
  const uint32_t first = kIconOffset / Iso9660::kSectorSize;
  const uint32_t sectors = (kIconOffset + kIconSize + Iso9660::kSectorSize - 1) / Iso9660::kSectorSize - first;
  std::vector<uint8_t> data(size_t(sectors) * Iso9660::kSectorSize);
  if (!iso.readSectors(pack->lba + first, sectors, data.data())) {
    error = "could not read the disc";
    return false;
  }
  data.erase(data.begin(), data.begin() + (kIconOffset % Iso9660::kSectorSize));
  data.resize(kIconSize);
  std::vector<Vertex> verts;
  std::vector<uint16_t> texture;
  if (!parse(data, verts, texture)) {
    error = "the memory card icon was not found on the disc";
    return false;
  }
  // Drawn at 1024 pixels and averaged down, which smooths its edges.
  const std::vector<Rgba> whole = render(verts, texture, 1024, false), close = render(verts, texture, 1024, true);
  std::vector<std::vector<uint8_t>> images;
  const std::vector<int> sizes = {256, 48, 32, 16};
  for (int size : sizes) images.push_back(dib(shrink(size >= 48 ? whole : close, 1024, 1024 / size), size));

  std::ofstream f(out, std::ios::binary);
  if (!f) {
    error = "could not write " + out.string();
    return false;
  }
  const uint16_t header[3] = {0, 1, uint16_t(images.size())};
  f.write(reinterpret_cast<const char*>(header), sizeof header);
  uint32_t offset = uint32_t(6 + 16 * images.size());
  for (size_t i = 0; i < images.size(); ++i) {
    const uint8_t dims[4] = {uint8_t(sizes[i] & 0xFF), uint8_t(sizes[i] & 0xFF), 0, 0};
    const uint16_t planes[2] = {1, 32};
    const uint32_t bytes = uint32_t(images[i].size());
    f.write(reinterpret_cast<const char*>(dims), 4);
    f.write(reinterpret_cast<const char*>(planes), 4);
    f.write(reinterpret_cast<const char*>(&bytes), 4);
    f.write(reinterpret_cast<const char*>(&offset), 4);
    offset += bytes;
  }
  for (const auto& image : images) f.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
  if (!f) {
    error = "could not write " + out.string();
    return false;
  }
  return true;
}

std::filesystem::path iconFile() {
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return std::filesystem::path(std::wstring(path, n)).parent_path() / L"castlevania.ico";
}
