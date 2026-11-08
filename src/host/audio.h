#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

// Sound output: 16-bit stereo samples pushed by the emulation are played through the default
// device (waveOut). A queue absorbs the jitter between emulated and real time: playback starts
// (and, after running dry, starts again) only once kStartQueueMs are waiting, so frames that
// arrive late or in bursts do not cut the sound into pieces. Samples that would make the queue
// longer than kMaxQueueMs are dropped.
class AudioOut {
public:
  static constexpr int kStartQueueMs = 60, kMaxQueueMs = 150;

  AudioOut();
  ~AudioOut();
  bool open(int rate);
  void push(const int16_t* samples, size_t count);  // interleaved left/right

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
