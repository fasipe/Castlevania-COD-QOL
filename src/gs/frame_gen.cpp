#include "gs/frame_gen.h"

#include "gs/shader_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// Shared by the shaders: the constants of each step.
constexpr const char* kCommon = R"(
cbuffer Step : register(b0) {
  uint2 size;      // of the level worked on (the picture, for mixCs)
  uint2 blocks;    // 8 x 8 blocks of the level
  float t;         // where the picture made lies between the two (0-1)
  uint range;      // searched around 0 at the coarsest level, in its pixels
  uint parented;   // a coarser level's motion to start from
  uint scale;      // picture pixels to a PS2 pixel
  float2 texel;    // 1 / size
  float2 picTexel; // 1 / the picture's size
  uint fine;       // half pixels tried too
  float2 turn;     // how the camera's turn moved the picture, in this level's pixels (where far things go)
  uint turned;     // the game said how its camera turned
  uint deep;       // the game's depth of both pictures is known (depthA, depthB: larger is nearer)
};
SamplerState smooth : register(s0);
)";

// The picture's brightness at the PS2's resolution: each pixel the mean of the picture's there.
constexpr const char* kLumaCs = R"(
Texture2D<float4> picture : register(t0);
RWTexture2D<float> luma : register(u0);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  float s = 0;
  for (uint y = 0; y < scale; ++y)
    for (uint x = 0; x < scale; ++x) s += dot(picture.Load(int3(id.xy * scale + uint2(x, y), 0)).rgb, float3(0.299, 0.587, 0.114));
  luma[id.xy] = s / float(scale * scale);
}
)";

// The game's depth at the PS2's resolution: the nearest of each pixel's samples (scale of them
// across); negative where something was drawn over the scene after it was finished (the game draws
// Hector, and the other characters, last: `scene` is the depth before them, if `range` says so).
constexpr const char* kDepthCs = R"(
Texture2D<float> source : register(t0);
Texture2D<float> scene : register(t1);
RWTexture2D<float> depth : register(u0);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  uint2 have;
  source.GetDimensions(have.x, have.y);
  float z = 0;
  uint over = 0;
  for (uint y = 0; y < scale; ++y)
    for (uint x = 0; x < scale; ++x) {
      const uint2 at = id.xy * scale + uint2(x, y);
      if (any(at >= have)) continue;
      const float s = source.Load(int3(at, 0));
      z = max(z, s);
      if (range && s > scene.Load(int3(at, 0))) ++over;  // (exactly: what was not drawn over is the same number)
    }
  depth[id.xy] = over * 2 > scale * scale ? -max(z, 1e-6) : z;
}
)";

// A level from the one twice as large.
constexpr const char* kDownCs = R"(
Texture2D<float> finer : register(t0);
RWTexture2D<float> luma : register(u0);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  luma[id.xy] = finer.SampleLevel(smooth, (float2(id.xy * 2) + 1.0) * texel, 0);
}
)";

// The motion of each block, from the start A to the end B, as it is at t: the block's pixels p
// match A at p - t v and B at p + (1 - t) v (so the picture made has no holes). The coarsest level
// is searched around 0; each finer one tries the coarser motion of its block and of the blocks
// beside it, each give or take a pixel; the finest then half pixels. A group of threads per block,
// each trying some of the candidates.
constexpr const char* kSearchCs = R"(
Texture2D<float> lumaA : register(t0);
Texture2D<float> lumaB : register(t1);
Texture2D<float2> coarser : register(t2);
RWTexture2D<float2> motion : register(u0);
groupshared float gCost[GROUP];
groupshared float2 gMotion[GROUP];
// The block's mismatch moved by v, over rows [j0, j1) of its 12 x 12 neighbourhood (the first rows
// adding what favours short motions: the shorter of two that match as well; still things stay still).
float cost(int2 origin, float2 v, int j0 = -2, int j1 = 10) {
  float s = 0;
  [loop] for (int j = j0; j < j1; ++j)
    [unroll] for (int i = -2; i < 10; ++i) {
      const float2 p = float2(origin + int2(i, j)) + 0.5;
      s += abs(lumaA.SampleLevel(smooth, (p - t * v) * texel, 0) - lumaB.SampleLevel(smooth, (p + (1.0 - t) * v) * texel, 0));
    }
  return s / 144.0 + (j0 == -2 ? 0.0005 * length(v) - (all(v == 0) ? 0.002 : 0.0) : 0.0);
}
// The group's best of each thread's best.
float2 bestOfGroup(uint ti, float c, float2 v) {
  gCost[ti] = c, gMotion[ti] = v;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint n = GROUP / 2; n > 0; n >>= 1) {
    if (ti < n && gCost[ti + n] < gCost[ti]) gCost[ti] = gCost[ti + n], gMotion[ti] = gMotion[ti + n];
    GroupMemoryBarrierWithGroupSync();
  }
  const float2 best = gMotion[0];
  GroupMemoryBarrierWithGroupSync();
  return best;
}
// Candidates tried by 4 threads each, a quarter of the rows apiece: the group's best.
float2 bestOfParts(uint ti, float part, float2 v, bool tried) {
  gCost[ti] = part;
  GroupMemoryBarrierWithGroupSync();
  const float whole = tried && (ti & 3) == 0 ? gCost[ti] + gCost[ti + 1] + gCost[ti + 2] + gCost[ti + 3] : 1e30;
  GroupMemoryBarrierWithGroupSync();
  return bestOfGroup(ti, whole, v);
}
// GROUP: 128 threads for the coarsest level's search, 64 (each candidate in 4 parts) for the others.
[numthreads(GROUP, 1, 1)] void main(uint3 group : SV_GroupID, uint ti : SV_GroupIndex) {
  const int2 origin = int2(group.xy) * 8;
  float bestCost = 1e30;
  float2 best = 0;
  if (!parented) {
    // Around no motion, and (however far it took things) around the camera's turn.
    const int r = int(range), side = 2 * r + 1;
    const float2 around = turned ? round(turn) : float2(0, 0);
    for (int k = int(ti); k < side * side * (turned ? 2 : 1); k += GROUP) {
      const int i = k % (side * side);
      const float2 v = float2(i % side - r, i / side - r) + (k >= side * side ? around : float2(0, 0));
      const float c = cost(origin, v);
      if (c < bestCost) bestCost = c, best = v;
    }
    best = bestOfGroup(ti, bestCost, best);
  } else {  // the block's coarser motion give or take a pixel, its 4 neighbours', none, the camera's turn
    uint2 cs;
    coarser.GetDimensions(cs.x, cs.y);
    const int2 near[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    const int2 at = int2(group.xy / 2);
    const uint k = ti / 4;
    const int row = int(ti & 3) * 3 - 2;
    float2 v = 0;
    if (k < 9) v = coarser.Load(int3(min(at, int2(cs) - 1), 0)) * 2.0 + float2(k % 3, k / 3) - 1.0;
    else if (k < 13) v = coarser.Load(int3(clamp(at + near[k - 9], int2(0, 0), int2(cs) - 1), 0)) * 2.0;
    else if (k == 14) v = turned ? turn : 0;  // the camera's turn
    best = bestOfParts(ti, cost(origin, v, row, row + 3), v, k < 15);
  }
  if (fine) {
    const uint k = ti / 4;
    const int row = int(ti & 3) * 3 - 2;
    const float2 v = best + (float2(k % 3, k / 3) - 1.0) * 0.5;
    best = bestOfParts(ti, k < 9 ? cost(origin, v, row, row + 3) : 0.0, v, k < 9);
  }
  if (ti == 0) motion[group.xy] = best;
}
)";

// Each PS2 pixel's motion: of its block and the 4 beside it (and none), the one that matches its own
// neighbourhood best, so edges between moving things stay sharp.
constexpr const char* kSelectCs = R"(
Texture2D<float> lumaA : register(t0);
Texture2D<float> lumaB : register(t1);
Texture2D<float2> motion : register(t2);
Texture2D<float> depthA : register(t3);
Texture2D<float> depthB : register(t4);
RWTexture2D<float2> pixels : register(u0);
// How far apart in depth the two things a motion joins are (0: the same surface), relative.
float apart(int2 p0, float2 v) {
  if (!deep) return 0;
  const float2 q = float2(p0) + 0.5;
  const float za = depthA.Load(int3(clamp(int2(floor(q - t * v)), int2(0, 0), int2(size) - 1), 0));
  const float zb = depthB.Load(int3(clamp(int2(floor(q + (1.0 - t) * v)), int2(0, 0), int2(size) - 1), 0));
  if (za == 0 || zb == 0) return 0;  // (beyond what the depth buffer covers)
  return abs(abs(za) - abs(zb)) / max(max(abs(za), abs(zb)), 1e-6);
}
float cost(int2 p0, float2 v) {
  float s = 0;
  [unroll] for (int j = -1; j <= 1; ++j)
    [unroll] for (int i = -1; i <= 1; ++i) {
      const float2 p = float2(p0 + int2(i, j)) + 0.5;
      s += abs(lumaA.SampleLevel(smooth, (p - t * v) * texel, 0) - lumaB.SampleLevel(smooth, (p + (1.0 - t) * v) * texel, 0));
    }
  return s;
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  const int2 p = int2(id.xy);
  const int2 b = int2(id.xy / 8);
  const int2 near[5] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
  float2 v[6];
  float c[6], least = 1e30;
  v[5] = 0;
  [unroll] for (int k = 0; k < 6; ++k) {
    if (k < 5) v[k] = motion.Load(int3(clamp(b + near[k], int2(0, 0), int2(blocks) - 1), 0));
    c[k] = cost(p, v[k]) + 2.0 * apart(p, v[k]);  // (a motion joining Hector to the wall behind him is wrong)
    least = min(least, c[k]);
  }
  // The best match, still things staying still (none, slightly favoured: the interface).
  float2 best = v[5];
  float bestCost = c[5] - 0.01;
  [unroll] for (int k = 0; k < 5; ++k)
    if (c[k] < bestCost) bestCost = c[k], best = v[k];
  // But where blocks around it move differently (the edge of something near, as Hector while the
  // camera circles him, against the scene behind), of their motions that fit it well enough, the
  // one of what is in front: both may fit there, and the near thing hides the far one. In front is
  // what moves least like the camera's turn (Hector stays put while the scene turns), or with no
  // turn, what moves least.
  float frontBest = turned ? length(best - turn) : -length(best);
  [unroll] for (int k = 0; k < 5; ++k) {
    if (c[k] > least + 0.3 || (c[k] > least + 0.05 && c[k] > 0.5)) continue;  // (a poorer match only where it still fits well)
    const float front = turned ? length(v[k] - turn) : -length(v[k]);
    if (front > frontBest + 1.0) frontBest = front, best = v[k];
  }
  // Then a pixel either way, if it fits clearly better: limbs and the like move apart from their block.
  float here = cost(p, best) + 2.0 * apart(p, best);
  const float2 steps[4] = {float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1)};
  const float2 from = best;
  [unroll] for (int s = 0; s < 4; ++s) {
    const float cs = cost(p, from + steps[s]) + 2.0 * apart(p, from + steps[s]);
    if (cs < here - 0.05) here = cs, best = from + steps[s];
  }
  pixels[id.xy] = best;
}
)";

// Lone pixels whose choice differs from all around them (where the picture is too plain to tell)
// take the median motion of their 3 x 3 neighbourhood.
constexpr const char* kSmoothCs = R"(
Texture2D<float2> chosen : register(t0);
RWTexture2D<float2> pixels : register(u0);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  float2 v[9];
  [unroll] for (int k = 0; k < 9; ++k) v[k] = chosen.Load(int3(clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(size) - 1), 0));
  float2 best = v[4];
  float bestSum = 1e30;
  [unroll] for (int i = 0; i < 9; ++i) {
    float s = 0;
    [unroll] for (int j = 0; j < 9; ++j) s += abs(v[i].x - v[j].x) + abs(v[i].y - v[j].y);
    if (s < bestSum) bestSum = s, best = v[i];
  }
  pixels[id.xy] = best;
}
)";

// The picture at t, as large as it is shown (no larger than the pictures): A moved forward and B
// back to it, mixed by how near each is (each read with a Catmull-Rom filter, which keeps them as
// sharp as they are between pixels; from 5 reads, the corners' weights being small).
constexpr const char* kMixCs = R"(
Texture2D<float4> pictureA : register(t0);
Texture2D<float4> pictureB : register(t1);
Texture2D<float2> pixels : register(t2);
Texture2D<float> depthA : register(t3);
Texture2D<float> depthB : register(t4);
RWTexture2D<unorm float4> outPicture : register(u0);
float3 sharp(Texture2D<float4> tex, float2 p) {  // p in pixels, their centres at .5
  const float2 p1 = floor(p - 0.5) + 0.5, f = p - p1;
  const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
  const float2 w12 = w1 + w2;
  const float2 q0 = (p1 - 1.0) * picTexel, q12 = (p1 + w2 / w12) * picTexel, q3 = (p1 + 2.0) * picTexel;
  const float wc = w12.x * w12.y, wl = w0.x * w12.y, wr = w3.x * w12.y, wt = w12.x * w0.y, wb = w12.x * w3.y;
  const float3 c = tex.SampleLevel(smooth, q12, 0).rgb * wc + tex.SampleLevel(smooth, float2(q0.x, q12.y), 0).rgb * wl +
                   tex.SampleLevel(smooth, float2(q3.x, q12.y), 0).rgb * wr + tex.SampleLevel(smooth, float2(q12.x, q0.y), 0).rgb * wt +
                   tex.SampleLevel(smooth, float2(q12.x, q3.y), 0).rgb * wb;
  return saturate(c / (wc + wl + wr + wt + wb));
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  const float2 c = (float2(id.xy) + 0.5) * texel;  // texel: picture pixels to one of the picture made
  const float2 v = pixels.Load(int3(uint2(c) / scale, 0)) * float(scale);
  const float3 lum = float3(0.299, 0.587, 0.114);
  const float2 pa = c - t * v, pb = c + (1.0 - t) * v, picture = 1.0 / picTexel;
  const float3 a = sharp(pictureA, pa), b = sharp(pictureB, pb);
  // Where the two disagree, the motion found is wrong there (something appears, the scene changes,
  // a flash): the nearer of the two pictures as it is, rather than a mix of both. And where one of
  // them is off its picture (the side the camera turns to, or from), the other as it is.
  float differ = smoothstep(0.04, 0.1, abs(dot(a - b, lum)));  // (low: a mix of two things that differ is a ghost)
  const bool inA = all(pa >= 0.0 && pa <= picture), inB = all(pb >= 0.0 && pb <= picture);
  float3 near = t < 0.5 ? a : b;
  if (deep) {
    // Hector (what the game draws over the finished scene) as the nearer picture shows him, not
    // made up: he barely moves on the screen while the camera circles him, and his limbs change
    // shape from one picture to the next, which no motion describes.
    const int2 at = clamp(int2(c / float(scale)), int2(0, 0), int2(blocks) - 1);
    const bool nearIsA = t < 0.5;
    if ((nearIsA ? depthA.Load(int3(at, 0)) : depthB.Load(int3(at, 0))) < 0) {
      outPicture[id.xy] = float4(nearIsA ? pictureA.Load(int3(c, 0)).rgb : pictureB.Load(int3(c, 0)).rgb, 1);
      return;
    }
    // Elsewhere, a sample that lands on him (or on anything nearer than the other sample) is not
    // the scene this pixel shows: the other one, solid.
    const float za = depthA.Load(int3(clamp(int2(pa / float(scale)), int2(0, 0), int2(blocks) - 1), 0));
    const float zb = depthB.Load(int3(clamp(int2(pb / float(scale)), int2(0, 0), int2(blocks) - 1), 0));
    if (za < 0 && zb >= 0) near = b, differ = 1;
    else if (zb < 0 && za >= 0) near = a, differ = 1;
    else if (za < 0 && zb < 0) near = nearIsA ? pictureA.Load(int3(c, 0)).rgb : pictureB.Load(int3(c, 0)).rgb, differ = 1;
    else if (za > 0 && zb > 0 && abs(za - zb) > 0.15 * max(za, zb)) near = za > zb ? a : b, differ = 1;  // (0: no depth there)
  }
  if (inA != inB) near = inA ? a : b, differ = 1;
  outPicture[id.xy] = float4(lerp(lerp(a, b, t), near, differ), 1);
}
)";

struct Step {
  uint32_t w, h, bw, bh;
  float t;
  uint32_t range, parented, scale;
  float texelX, texelY, picTexelX, picTexelY;
  uint32_t fine;
  float turnX, turnY;
  uint32_t turned;
  uint32_t deep;
};

}  // namespace

bool FrameGen::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string& error) {
  dev_ = dev, ctx_ = ctx;
  struct Shader {
    const char* body;
    ComPtr<ID3D11ComputeShader>* out;
  };
  const std::string search64 = std::string("#define GROUP 64\n") + kSearchCs, search128 = std::string("#define GROUP 128\n") + kSearchCs;
  for (const Shader& s : {Shader{kLumaCs, &lumaCs_}, Shader{kDownCs, &downCs_}, Shader{search64.c_str(), &searchCs_},
                          Shader{search128.c_str(), &topSearchCs_}, Shader{kSelectCs, &selectCs_}, Shader{kDepthCs, &depthCs_},
                          Shader{kSmoothCs, &smoothCs_}, Shader{kMixCs, &mixCs_}}) {
    const std::string source = std::string(kCommon) + s.body;
    const ComPtr<ID3DBlob> code = compileShader(source, "cs_5_0", error);
    if (!code) return false;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, s.out->GetAddressOf())))
      return error = "interpolation shader", false;
  }
  D3D11_BUFFER_DESC cb{256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  if (FAILED(dev->CreateBuffer(&cb, nullptr, &constants_)) || FAILED(dev->CreateSamplerState(&sd, &sampler_)))
    return error = "interpolation resources", false;
  return true;
}

bool FrameGen::make(Target& t, uint32_t w, uint32_t h, DXGI_FORMAT format) {
  t = {};
  D3D11_TEXTURE2D_DESC td{w, h, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
  return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &t.tex)) && SUCCEEDED(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)) &&
         SUCCEEDED(dev_->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav));
}

void FrameGen::constants(const void* data, size_t bytes) {
  D3D11_MAPPED_SUBRESOURCE map;
  if (FAILED(ctx_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
  std::memcpy(map.pData, data, bytes);
  ctx_->Unmap(constants_.Get(), 0);
}

void FrameGen::run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11UnorderedAccessView* uav,
                   uint32_t w, uint32_t h, bool groups) {
  ctx_->CSSetShader(cs, nullptr, 0);
  ctx_->CSSetShaderResources(0, UINT(srvs.size()), srvs.begin());
  ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx_->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
  ctx_->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  if (groups) ctx_->Dispatch(w, h, 1);
  else ctx_->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
  ID3D11ShaderResourceView* none[5] = {};
  ID3D11UnorderedAccessView* noUav = nullptr;
  ctx_->CSSetShaderResources(0, 5, none);
  ctx_->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
}

void FrameGen::push(ID3D11Texture2D* picture, uint32_t w, uint32_t h, uint32_t scale, const View& view, ID3D11ShaderResourceView* depth,
                    uint32_t depthScale, ID3D11ShaderResourceView* sceneDepth) {
  if (w != w_ || h != h_ || scale != scale_ || !picture_[0].tex) {
    pictures_ = 0, w_ = w, h_ = h, scale_ = scale;
    bool ok = make(picture_[0], w, h, DXGI_FORMAT_R8G8B8A8_UNORM) && make(picture_[1], w, h, DXGI_FORMAT_R8G8B8A8_UNORM) &&
              true;
    outW_ = outH_ = 0;
    for (int l = 0; l < kLevels && ok; ++l) {
      levelW_[l] = (std::max)(1u, (w / scale) >> l), levelH_[l] = (std::max)(1u, (h / scale) >> l);
      ok = make(luma_[0][l], levelW_[l], levelH_[l], DXGI_FORMAT_R16_FLOAT) && make(luma_[1][l], levelW_[l], levelH_[l], DXGI_FORMAT_R16_FLOAT) &&
           make(blocks_[l], (levelW_[l] + 7) / 8, (levelH_[l] + 7) / 8, DXGI_FORMAT_R16G16_FLOAT);
    }
    if (!ok || !make(chosen_, levelW_[0], levelH_[0], DXGI_FORMAT_R16G16_FLOAT) || !make(pixels_, levelW_[0], levelH_[0], DXGI_FORMAT_R16G16_FLOAT) ||
        !make(depth_[0], levelW_[0], levelH_[0], DXGI_FORMAT_R32_FLOAT) || !make(depth_[1], levelW_[0], levelH_[0], DXGI_FORMAT_R32_FLOAT)) {
      picture_[0] = {}, w_ = 0;
      return;
    }
  }
  newest_ ^= 1;
  ctx_->CopyResource(picture_[newest_].tex.Get(), picture);
  for (int l = 0; l < kLevels; ++l) {
    const Step s{levelW_[l], levelH_[l], 0, 0, 0, 0, 0, scale_, l ? 1.0f / float(levelW_[l - 1]) : 0, l ? 1.0f / float(levelH_[l - 1]) : 0};
    constants(&s, sizeof(s));
    if (l == 0) run(lumaCs_.Get(), {picture_[newest_].srv.Get()}, luma_[newest_][0].uav.Get(), levelW_[0], levelH_[0]);
    else run(downCs_.Get(), {luma_[newest_][l - 1].srv.Get()}, luma_[newest_][l].uav.Get(), levelW_[l], levelH_[l]);
  }
  deep_[newest_] = depth != nullptr;
  if (depth) {
    const Step s{levelW_[0], levelH_[0], 0, 0, 0, sceneDepth ? 1u : 0u, 0, depthScale};
    constants(&s, sizeof(s));
    run(depthCs_.Get(), {depth, sceneDepth}, depth_[newest_].uav.Get(), levelW_[0], levelH_[0]);
  }
  pictures_ = (std::min)(pictures_ + 1, 2);
  moved_ = false;
  // How far the picture moved with the camera's turn, where the search starts besides no motion:
  // measured in the game, things move about 450 PS2 pixels (of 512 across, 4:3) a radian of yaw
  // and 560 a radian of pitch, the far ones more and the near ones less (the camera circles the
  // point it looks at), which the search then finds. 16:9 shows a quarter more across in the same
  // pixels.
  const View& before = views_[newest_ ^ 1];
  views_[newest_] = view;
  turned_ = 0;
  if (view.known && before.known) {
    const float yaw = std::remainder(view.yaw - before.yaw, 6.2831853f), pitch = view.pitch - before.pitch;
    turnX_ = -450.0f * yaw * float(levelW_[0]) / 512.0f * (view.wide ? 0.75f : 1.0f);
    turnY_ = -560.0f * pitch * float(levelH_[0]) / 448.0f;
    turned_ = std::abs(turnX_) < 400 && std::abs(turnY_) < 400;  // (not a cut to another view)
  }
}

ID3D11ShaderResourceView* FrameGen::make(float t, uint32_t outW, uint32_t outH) {
  if (!ready()) return nullptr;
  const int a = newest_ ^ 1, b = newest_;
  if (!moved_) {
    moved_ = true;
    motion(a, b);
  }
  outW = std::clamp(outW, 1u, w_), outH = std::clamp(outH, 1u, h_);
  if (outW != outW_ || outH != outH_) {
    if (!make(out_, outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM)) return nullptr;
    outW_ = outW, outH_ = outH;
  }
  const Step mix{outW, outH, levelW_[0], levelH_[0], t, 0, 0, scale_, float(w_) / float(outW), float(h_) / float(outH), 1.0f / float(w_),
                 1.0f / float(h_), 0, turnX_, turnY_, turned_, uint32_t(deep_[a] && deep_[b])};
  constants(&mix, sizeof(mix));
  run(mixCs_.Get(), {picture_[a].srv.Get(), picture_[b].srv.Get(), pixels_.srv.Get(), depth_[a].srv.Get(), depth_[b].srv.Get()}, out_.uav.Get(),
      outW, outH);
  return out_.srv.Get();
}

void FrameGen::motion(int a, int b) {
  const float t = 0.5f;
  for (int l = kLevels - 1; l >= 0; --l) {
    const uint32_t bw = (levelW_[l] + 7) / 8, bh = (levelH_[l] + 7) / 8;
    const float k = 1.0f / float(1 << l);
    const Step s{levelW_[l], levelH_[l], bw, bh, t, 8, l < kLevels - 1, scale_, 1.0f / float(levelW_[l]), 1.0f / float(levelH_[l]),
                 0, 0, l == 0, turnX_ * k, turnY_ * k, turned_};
    constants(&s, sizeof(s));
    run((l < kLevels - 1 ? searchCs_ : topSearchCs_).Get(), {luma_[a][l].srv.Get(), luma_[b][l].srv.Get(), l < kLevels - 1 ? blocks_[l + 1].srv.Get() : nullptr},
        blocks_[l].uav.Get(), bw, bh, true);
  }
  const Step sel{levelW_[0], levelH_[0], (levelW_[0] + 7) / 8, (levelH_[0] + 7) / 8, t, 0, 0, scale_, 1.0f / float(levelW_[0]),
                 1.0f / float(levelH_[0]), 0, 0, 0, turnX_, turnY_, turned_, uint32_t(deep_[a] && deep_[b])};
  constants(&sel, sizeof(sel));
  run(selectCs_.Get(), {luma_[a][0].srv.Get(), luma_[b][0].srv.Get(), blocks_[0].srv.Get(), depth_[a].srv.Get(), depth_[b].srv.Get()},
      chosen_.uav.Get(), levelW_[0], levelH_[0]);
  run(smoothCs_.Get(), {chosen_.srv.Get()}, pixels_.uav.Get(), levelW_[0], levelH_[0]);
}
