#pragma once
#include <cstdint>
#include <vector>

// SPU2 (the PS2 sound processor) as seen through libsd: registers, 2 MB of sound RAM and its 48
// voices (2 cores x 24). Voices decode their ADPCM blocks at their pitch, run the ADSR envelope
// and are mixed with the cores' streamed PCM input (auto DMA) into 48 kHz stereo; sound drivers
// also poll the voices' end flags and envelope levels to know when a sound has finished.
class Spu2 {
public:
  static constexpr uint32_t kRamSize = 2 * 1024 * 1024;
  static constexpr int kSampleRate = 48000;

  Spu2();

  void init();
  // libsd entry encoding: bit 0 core, bits 1-5 voice, bits 8+ register number.
  void setParam(uint32_t entry, uint32_t value);
  uint32_t getParam(uint32_t entry) const;
  void setSwitch(uint32_t entry, uint32_t value);
  uint32_t getSwitch(uint32_t entry);
  void setAddr(uint32_t entry, uint32_t value);
  uint32_t getAddr(uint32_t entry) const;
  void setCoreAttr(uint32_t entry, uint32_t value);
  uint32_t getCoreAttr(uint32_t entry) const;
  // sceSdSetEffectAttr: reverb preset (libsd mode, bit 8 = clear the work area) and output volume.
  void setEffect(int core, uint32_t mode, int16_t depthL, int16_t depthR);

  // Sound RAM transfers (byte addresses); returns the size moved.
  uint32_t write(uint32_t spuAddr, const uint8_t* src, uint32_t size);
  uint32_t read(uint32_t spuAddr, uint8_t* dst, uint32_t size) const;

  // Plays everything forward; the samples produced are appended to `output` (interleaved
  // left/right) when it is set.
  void advance(uint64_t samples);
  std::vector<int16_t>* output = nullptr;

  // Auto DMA (libsd BlockTrans in loop mode): the core reads a ring buffer in IOP memory at
  // 48 kHz, half a buffer at a time; it holds blocks of 256 left then 256 right 16-bit samples.
  struct AutoDma {
    bool active = false;
    uint32_t iop = 0, size = 0;
    uint64_t samples = 0;  // played since the start
    uint64_t halves() const { return size >= 8 ? samples * 4 / (size / 2) : 0; }
  };
  void startAutoDma(int core, uint32_t iopAddr, uint32_t size);
  void stopAutoDma(int core) { adma[core & 1].active = false; }
  // Current read address in bits 0-23 and the buffer half being read in bit 24.
  uint32_t autoDmaStatus(int core) const;
  AutoDma adma[2];
  const std::vector<uint8_t>* iopRam = nullptr;  // where auto DMA reads from

  std::vector<uint8_t> ram;
  uint64_t keyOns = 0;
  uint32_t irqPending = 0;  // bit per core: a voice read the block at that core's IRQ address

private:
  enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
  struct Voice {
    uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    uint32_t ssa = 0, lsa = 0, nax = 0;
    bool lsaSet = false;
    bool on = false;
    Phase phase = Phase::Off;
    int32_t envx = 0;        // 0 .. 0x7FFF
    uint32_t envCounter = 0;  // samples until the next envelope step
    uint32_t pos = 0;        // 4.12 fixed point position within the decoded block
    int16_t block[28] = {};  // the current block, decoded
    int16_t prev[2] = {};    // ADPCM history (last two decoded samples)
    int16_t last = 0;        // the sample before block[0], for interpolation
    bool decoded = false;    // block[] holds the block at nax
  };
  // The reverb unit (PS1-style algorithm at half the sample rate) and its work area at the end of
  // the core's region of sound RAM.
  struct Reverb {
    const uint16_t* preset = nullptr;  // 32 registers; null = off
    uint32_t start = 0, end = 0, cur = 0;  // work area [start, end] and current position (bytes)
    int32_t volL = 0, volR = 0;
    int32_t inL = 0, inR = 0, outL = 0, outR = 0, prevL = 0, prevR = 0;
    bool odd = false;
  };
  struct Core {
    Voice voice[24];
    Reverb reverb;
    uint32_t endx = 0;
    uint32_t switches[16] = {};
    uint32_t params[32] = {};
    uint32_t addrs[16] = {};
    uint32_t attrs[8] = {};
  };
  void keyOn(Voice& v);
  void keyOff(Voice& v);
  void decodeBlock(Core& c, Voice& v);
  void stepEnvelope(Voice& v);
  int32_t nextSample(Core& c, int index);  // the voice's output (before volume), advancing it
  void readBlock(const Core& c, uint32_t addr);  // raises the core's IRQ if enabled there
  void reverb(Core& c, int32_t inL, int32_t inR, int32_t& outL, int32_t& outR);

  Core core_[2];
};
