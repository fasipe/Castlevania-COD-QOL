#include "host/audio.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AudioOut::Impl {
  static constexpr int kBuffers = 4, kFramesPerBuffer = 960;  // 20 ms each at 48 kHz

  HWAVEOUT device = nullptr;
  HANDLE event = nullptr;
  WAVEHDR headers[kBuffers] = {};
  std::vector<int16_t> buffers[kBuffers];
  std::mutex mutex;
  std::deque<int16_t> queue;  // interleaved samples waiting to be played
  size_t startQueue = 0, maxQueue = 0;
  bool waiting = true;  // playing silence until startQueue samples are queued
  std::atomic<bool> stop = false;
  std::thread thread;

  // Hands every finished buffer back to the device, refilled from the queue.
  void run() {
    while (!stop) {
      WaitForSingleObject(event, 50);
      for (int i = 0; i < kBuffers; ++i) {
        WAVEHDR& h = headers[i];
        if (!(h.dwFlags & WHDR_DONE)) continue;
        std::vector<int16_t>& b = buffers[i];
        {
          std::lock_guard lock(mutex);
          if (waiting && queue.size() >= startQueue) waiting = false;
          const size_t n = waiting ? 0 : std::min(queue.size(), b.size());
          if (n < b.size()) waiting = true;  // ran dry: wait for a cushion again
          std::copy(queue.begin(), queue.begin() + ptrdiff_t(n), b.begin());
          std::fill(b.begin() + ptrdiff_t(n), b.end(), int16_t(0));
          queue.erase(queue.begin(), queue.begin() + ptrdiff_t(n));
        }
        h.dwFlags &= ~WHDR_DONE;
        waveOutWrite(device, &h, sizeof(h));
      }
    }
  }
};

AudioOut::AudioOut() : impl_(std::make_unique<Impl>()) {}

AudioOut::~AudioOut() {
  Impl& m = *impl_;
  if (!m.device) return;
  m.stop = true;
  SetEvent(m.event);
  if (m.thread.joinable()) m.thread.join();
  waveOutReset(m.device);
  for (WAVEHDR& h : m.headers) waveOutUnprepareHeader(m.device, &h, sizeof(h));
  waveOutClose(m.device);
  CloseHandle(m.event);
}

bool AudioOut::open(int rate) {
  Impl& m = *impl_;
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 2;
  format.nSamplesPerSec = DWORD(rate);
  format.wBitsPerSample = 16;
  format.nBlockAlign = 4;
  format.nAvgBytesPerSec = DWORD(rate) * 4;
  m.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (waveOutOpen(&m.device, WAVE_MAPPER, &format, DWORD_PTR(m.event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
    m.device = nullptr;
    return false;
  }
  m.startQueue = size_t(rate) * 2 * kStartQueueMs / 1000;
  m.maxQueue = size_t(rate) * 2 * kMaxQueueMs / 1000;
  for (int i = 0; i < Impl::kBuffers; ++i) {
    m.buffers[i].assign(Impl::kFramesPerBuffer * 2, 0);
    WAVEHDR& h = m.headers[i];
    h.lpData = reinterpret_cast<LPSTR>(m.buffers[i].data());
    h.dwBufferLength = DWORD(m.buffers[i].size() * 2);
    waveOutPrepareHeader(m.device, &h, sizeof(h));
    h.dwFlags |= WHDR_DONE;  // filled and queued by the thread
  }
  m.thread = std::thread([&m] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    m.run();
  });
  SetEvent(m.event);
  return true;
}

void AudioOut::push(const int16_t* samples, size_t count) {
  Impl& m = *impl_;
  if (!m.device) return;
  std::lock_guard lock(m.mutex);
  m.queue.insert(m.queue.end(), samples, samples + count);
  if (m.queue.size() > m.maxQueue) m.queue.erase(m.queue.begin(), m.queue.begin() + ptrdiff_t((m.queue.size() - m.maxQueue) & ~size_t(1)));
}
