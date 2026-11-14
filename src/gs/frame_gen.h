#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

// Pictures between the game's, for screens faster than it (interpolation, an option): the motion
// from one picture to the next is estimated on the GPU at the PS2's resolution, block by block
// from coarse to fine, then per pixel; each picture between is the two moved part of the way
// and mixed. It knows nothing of the game: the pictures are all it has.
class FrameGen {
 public:
  bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string& error);
  // The game's newest picture (w x h, `scale` of its pixels to one of the PS2's): the one before
  // it is kept as the start.
  // `view`: where the game's camera looked for it, if the game said (gameplay: not menus, movies).
  struct View {
    bool known = false;
    float yaw = 0, pitch = 0;  // radians
    bool wide = false;          // 16:9
  };
  // `depth`: the game's depth buffer for it, if known (depthScale samples to a PS2 pixel; larger is nearer).
  // `sceneDepth`: the depth before the characters were drawn over the scene (Hector), if known.
  void push(ID3D11Texture2D* picture, uint32_t w, uint32_t h, uint32_t scale, const View& view = {}, ID3D11ShaderResourceView* depth = nullptr,
            uint32_t depthScale = 1, ID3D11ShaderResourceView* sceneDepth = nullptr);
  bool ready() const { return pictures_ >= 2; }  // two pictures of the same size
  void forget() { pictures_ = 0; }  // the next picture pushed starts again
  // The picture at t (0-1) from the one before the newest to the newest (moving along the motion
  // found halfway, for all t).
  // outW x outH: its size, as it is shown (no larger than the pictures).
  ID3D11ShaderResourceView* make(float t, uint32_t outW, uint32_t outH);

 private:
  template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
  static constexpr int kLevels = 4;  // the PS2's resolution, then halves
  struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
  };
  bool make(Target& t, uint32_t w, uint32_t h, DXGI_FORMAT format);
  void constants(const void* data, size_t bytes);
  void motion(int a, int b);  // from picture a to b, halfway, into pixels_
  void run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11UnorderedAccessView* uav,
           uint32_t w, uint32_t h, bool groups = false);  // groups: a thread group per item (else per 8 x 8)

  ID3D11Device* dev_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  ComPtr<ID3D11ComputeShader> lumaCs_, downCs_, searchCs_, topSearchCs_, selectCs_, depthCs_, smoothCs_, mixCs_;
  ComPtr<ID3D11Buffer> constants_;
  ComPtr<ID3D11SamplerState> sampler_;
  uint32_t w_ = 0, h_ = 0, scale_ = 1;
  int pictures_ = 0, newest_ = 0;  // pictures pushed at this size; which of the two is newest
  bool moved_ = false;  // the motion between the two found (once for all the pictures made between)
  Target picture_[2], luma_[2][kLevels], blocks_[kLevels], chosen_, pixels_, out_;
  uint32_t levelW_[kLevels]{}, levelH_[kLevels]{};
  uint32_t outW_ = 0, outH_ = 0;
  View views_[2];
  Target depth_[2];  // the game's depth of each picture at the PS2's resolution
  bool deep_[2] = {};
  float turnX_ = 0, turnY_ = 0;  // the camera's turn from the start to the newest, in PS2 pixels
  uint32_t turned_ = 0;
};
