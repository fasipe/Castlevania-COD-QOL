#include "iop/sound.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "iop/spu2.h"

namespace {
// libsd function codes as sent by sceSdRemote (the low nibble carries flags).
enum : uint32_t {
  rSdInit = 0x8000, rSdSetParam = 0x8010, rSdGetParam = 0x8020, rSdSetSwitch = 0x8030,
  rSdGetSwitch = 0x8040, rSdSetAddr = 0x8050, rSdGetAddr = 0x8060, rSdSetCoreAttr = 0x8070,
  rSdGetCoreAttr = 0x8080, rSdNote2Pitch = 0x8090, rSdPitch2Note = 0x80A0, rSdProcBatch = 0x80B0,
  rSdProcBatchEx = 0x80C0, rSdVoiceTrans = 0x80D0, rSdBlockTrans = 0x80E0,
  rSdVoiceTransStatus = 0x80F0, rSdBlockTransStatus = 0x8100, rSdSetTransCallback = 0x8110,
  rSdSetIRQCallback = 0x8120, rSdSetEffectAttr = 0x8130, rSdGetEffectAttr = 0x8140,
  rSdClearEffectWorkArea = 0x8150, rSdSetTransIntrHandler = 0x8160,
  rSdSetSpu2IntrHandler = 0x8170, rSdStopTrans = 0x8180, rSdCleanEffectWorkArea = 0x8190,
  rSdSetEffectMode = 0x81A0, rSdSetEffectModeParams = 0x81B0, rSdProcBatch2 = 0x81C0,
  rSdProcBatchEx2 = 0x81D0,
};

uint32_t rd32(Sif& sif, uint32_t iopAddr) {
  uint32_t v = 0;
  if (const uint8_t* p = sif.iop(iopAddr, 4)) std::memcpy(&v, p, 4);
  return v;
}

class SdrServer : public Sif::RpcServer {
public:
  explicit SdrServer(Spu2& spu) : spu_(spu) {}
  const char* name() const override { return "sdrdrv"; }

  // Request: {self pointer, args[]...}; the reply's first word is the libsd return value.
  uint32_t call(Sif& sif, uint32_t fn, uint32_t buf, uint32_t size) override {
    const uint32_t cmd = fn & 0xFFF0;
    auto a = [&](int i) { return rd32(sif, buf + 4 + 4 * i); };
    uint32_t result = 0;
    switch (cmd) {
      case rSdInit: spu_.init(); break;
      case rSdSetParam: spu_.setParam(a(0), a(1)); break;
      case rSdGetParam: result = spu_.getParam(a(0)); break;
      case rSdSetSwitch: spu_.setSwitch(a(0), a(1)); break;
      case rSdGetSwitch: result = spu_.getSwitch(a(0)); break;
      case rSdSetAddr: spu_.setAddr(a(0), a(1)); break;
      case rSdGetAddr: result = spu_.getAddr(a(0)); break;
      case rSdSetCoreAttr: spu_.setCoreAttr(a(0), a(1)); break;
      case rSdGetCoreAttr: result = spu_.getCoreAttr(a(0)); break;
      case rSdVoiceTrans:  // (channel, mode, iopAddr, spuAddr, size): mode bit 0 set = read from SPU
        result = transfer(sif, a(1), a(2), a(3), a(4));
        break;
      case rSdBlockTrans:  // (channel, mode, iopAddr, size): sound data input, usually a looping ring
        if ((a(1) & 3) == 2) {
          spu_.stopAutoDma(int(a(0)));
        } else if (a(1) & 0x10) {
          spu_.startAutoDma(int(a(0)), a(2), a(3));
          sif.log(std::format("sdBlockTrans loop ch{} iop 0x{:06X} size {}", a(0) & 1, a(2), a(3)));
        }
        break;
      case rSdVoiceTransStatus: result = 1; break;  // transfers finish immediately
      case rSdBlockTransStatus: result = spu_.autoDmaStatus(int(a(0))); break;
      case rSdProcBatch2:
      case rSdProcBatchEx2: {  // batch array: header {u16 -, u16 count, ...}, then {u16 func, u16 entry, u32 value}
        uint16_t count = 0;
        if (const uint8_t* p = sif.iop(buf + 2, 2)) std::memcpy(&count, p, 2);
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t e = buf + 8 + 8 * i;
          const uint32_t head = rd32(sif, e), value = rd32(sif, e + 4), entry = head >> 16;
          switch (head & 0xFFFF) {
            case 1: spu_.setParam(entry, value); break;
            case 2: spu_.setSwitch(entry, value); break;
            case 3: spu_.setAddr(entry, value); break;
            case 4: spu_.setCoreAttr(entry, value); break;
          }
        }
        result = count;
        break;
      }
      case rSdSetEffectAttr: case rSdSetEffectMode: case rSdSetEffectModeParams:
      case rSdClearEffectWorkArea: case rSdCleanEffectWorkArea: case rSdGetEffectAttr:
      case rSdSetTransCallback: case rSdSetIRQCallback: case rSdSetTransIntrHandler:
      case rSdSetSpu2IntrHandler: case rSdStopTrans:
        break;
      default:
        sif.unimplemented(std::format("sdrdrv command 0x{:04X} ({} bytes)", fn, size));
        return buf;
    }
    std::memcpy(sif.iop(buf, 4), &result, 4);
    return buf;
  }

private:
  uint32_t transfer(Sif& sif, uint32_t mode, uint32_t iopAddr, uint32_t spuAddr, uint32_t size) {
    uint8_t* iop = sif.iop(iopAddr, size);
    if (!iop) return 0;
    sif.log(std::format("sdTrans {} bytes iop 0x{:06X} {} spu 0x{:06X}", size, iopAddr, mode & 1 ? "<-" : "->", spuAddr));
    return mode & 1 ? spu_.read(spuAddr, iop, size) : spu_.write(spuAddr, iop, size);
  }

  Spu2& spu_;
};

}  // namespace

std::unique_ptr<Sif::RpcServer> makeSoundServer(uint32_t sid, Spu2& spu) {
  if (sid == kSdrServer) return std::make_unique<SdrServer>(spu);
  return nullptr;
}
