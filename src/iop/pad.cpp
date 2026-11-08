#include "iop/pad.h"

#include <cstring>
#include <format>

#include "ee/cpu.h"

PadInput gPadInput;

namespace {
// Request/reply block (0x80 bytes): word 0 is the command, the rest depends on it.
enum : uint32_t {
  kOpen = 0x01, kSetMainMode = 0x06, kSetActAlign = 0x08, kSetPressMode = 0x0A, kSetVref = 0x0B,
  kPortMax = 0x0C, kSlotMax = 0x0D, kClose = 0x0E, kEnd = 0x0F, kInit = 0x10, kVersion = 0x12,
  kCmd14 = 0x14,
};

// The EE pad area, as libpad reads it (0x00755810 and friends).
enum : uint32_t {
  oData = 0x00, oActData = 0x30, oModeTable = 0x50, oFrame = 0x58, oLength = 0x60,
  oModeConfig = 0x64, oModeCurId = 0x65, oModel = 0x66, oButtonDataReady = 0x67,
  oNrOfModes = 0x68, oModeCurOffs = 0x69, oNrOfActuators = 0x6A, oState = 0x70, oReqState = 0x71,
  oOk = 0x72,
};
constexpr uint8_t kStateDisconnected = 0, kStateStable = 6, kReqComplete = 0;

uint32_t rd32(Sif& sif, uint32_t a) {
  uint32_t v = 0;
  std::memcpy(&v, sif.iop(a, 4), 4);
  return v;
}
void wr32(Sif& sif, uint32_t a, uint32_t v) { std::memcpy(sif.iop(a, 4), &v, 4); }

class PadServer : public Sif::RpcServer {
public:
  const char* name() const override { return "padman"; }

  uint32_t call(Sif& sif, uint32_t, uint32_t buf, uint32_t) override {
    const uint32_t cmd = rd32(sif, buf), port = rd32(sif, buf + 4), slot = rd32(sif, buf + 8);
    switch (cmd) {
      case kVersion: wr32(sif, buf + 0xC, 0x400); break;  // the EE checks version >> 8 == 4
      case kInit: wr32(sif, buf + 0xC, 1); break;
      case kEnd: wr32(sif, buf + 0xC, 1); break;
      case kPortMax: wr32(sif, buf + 0xC, 2); break;
      case kSlotMax: wr32(sif, buf + 0xC, 1); break;
      case kCmd14: wr32(sif, buf + 8, 1); break;
      case kOpen: {  // {cmd, port, slot, -, EE pad area}; reply: result, IOP address for actuator data
        if (port < 2 && slot == 0) {
          ports_[port] = {true, rd32(sif, buf + 0x10), 0};
          ports_[port].actuators = sif.iopAlloc(0x40);
          wr32(sif, buf + 0x14, ports_[port].actuators);
          update(sif, port);
        }
        wr32(sif, buf + 0xC, port < 2 && slot == 0);
        sif.log(std::format("padPortOpen {} -> area 0x{:08X}", port, port < 2 ? ports_[port].area : 0));
        break;
      }
      case kClose:
        if (port < 2) ports_[port].open = false;
        wr32(sif, buf + 0xC, 1);
        break;
      case kSetMainMode:  // {cmd, port, slot, mode, lock}
        if (port < 2) analog_[port] = rd32(sif, buf + 0xC) != 0;
        wr32(sif, buf + 0x14, 1);
        break;
      case kSetPressMode:  // {cmd, port, slot, mask}: 0xFFF enters pressure mode, 0 leaves it
        if (port < 2) pressure_[port] = rd32(sif, buf + 0xC) != 0;
        wr32(sif, buf + 0x10, 1);
        break;
      case kSetActAlign:
        wr32(sif, buf + 0x14, 1);
        break;
      case kSetVref:
        wr32(sif, buf + 0x1C, 1);
        break;
      default:
        sif.unimplemented(std::format("padman command 0x{:X}", cmd));
        return buf;
    }
    return buf;
  }

  void tick(Sif& sif) override {
    const uint64_t frame = sif.cpu.mem.hw.frames;
    if (frame == lastFrame_) return;
    lastFrame_ = frame;
    for (uint32_t p = 0; p < 2; ++p)
      if (ports_[p].open) update(sif, p);
  }

private:
  // Writes the next frame into the half of the double buffer the EE is not reading.
  void update(Sif& sif, uint32_t port) {
    Port& pt = ports_[port];
    ++pt.frame;
    uint8_t* a = sif.cpu.mem.hostPtr(pt.area + (pt.frame & 1) * 0x80, 0x80);
    if (!a) return;
    std::memset(a, 0, 0x80);
    const bool connected = port == 0;
    std::memcpy(a + oFrame, &pt.frame, 4);
    a[oState] = connected ? kStateStable : kStateDisconnected;
    a[oReqState] = kReqComplete;
    if (!connected) return;
    a[oOk] = 1;
    a[oModeConfig] = 2;
    a[oModel] = 2;  // DualShock 2 (pressure-sensitive buttons)
    a[oButtonDataReady] = 1;
    a[oNrOfModes] = 2;
    a[oModeCurOffs] = analog_[port] ? 1 : 0;
    a[oNrOfActuators] = 2;
    const uint16_t modes[2] = {4, 7};  // digital, DualShock
    std::memcpy(a + oModeTable, modes, sizeof(modes));
    const uint8_t act[8] = {0, 1, 0, 0, 1, 1, 1, 1};  // small and large motor
    std::memcpy(a + oActData, act, sizeof(act));

    const uint8_t id = !analog_[port] ? 0x41 : pressure_[port] ? 0x79 : 0x73;
    a[oModeCurId] = id;
    const PadInput& in = gPadInput;
    const uint16_t buttons = uint16_t(~in.buttons);  // active low on the wire
    uint8_t* d = a + oData;
    d[0] = 0;
    d[1] = id;
    d[2] = uint8_t(buttons);
    d[3] = uint8_t(buttons >> 8);
    d[4] = in.rx;
    d[5] = in.ry;
    d[6] = in.lx;
    d[7] = in.ly;
    std::memcpy(d + 8, in.pressure, 12);
    const uint32_t length = id == 0x41 ? 4 : id == 0x73 ? 8 : 20;
    std::memcpy(a + oLength, &length, 4);
  }

  struct Port {
    bool open = false;
    uint32_t area = 0;
    uint32_t frame = 0;
    uint32_t actuators = 0;
  };
  Port ports_[2];
  bool analog_[2] = {}, pressure_[2] = {};
  uint64_t lastFrame_ = 0;
};

// The second padman server only receives actuator data by SIF DMA; it has no calls.
class PadExtServer : public Sif::RpcServer {
public:
  const char* name() const override { return "padman-ext"; }
  uint32_t call(Sif& sif, uint32_t fn, uint32_t buf, uint32_t) override {
    sif.unimplemented(std::format("padman extension function {}", fn));
    return buf;
  }
};
}  // namespace

std::unique_ptr<Sif::RpcServer> makePadServer(uint32_t sid) {
  if (sid == kPadServer) return std::make_unique<PadServer>();
  if (sid == kPadExtServer) return std::make_unique<PadExtServer>();
  return nullptr;
}

void setPad(PadInput& pad, uint16_t buttons, uint8_t stick) {
  // Pressure slots in PadInput order: right, left, up, down, triangle, circle, cross, square,
  // L1, R1, L2, R2.
  static constexpr uint16_t kPressure[12] = {0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000,
                                             0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};
  pad.buttons = buttons;
  for (int i = 0; i < 12; ++i) pad.pressure[i] = (buttons & kPressure[i]) ? 255 : 0;
  pad.lx = (stick & 4) ? 0x00 : (stick & 8) ? 0xFF : 0x80;
  pad.ly = (stick & 1) ? 0x00 : (stick & 2) ? 0xFF : 0x80;
  pad.rx = (stick & 64) ? 0x00 : (stick & 128) ? 0xFF : 0x80;
  pad.ry = (stick & 16) ? 0x00 : (stick & 32) ? 0xFF : 0x80;
}
