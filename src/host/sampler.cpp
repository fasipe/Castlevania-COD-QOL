#include "host/sampler.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct Sampled {
  HANDLE thread;
  std::string name;
  std::vector<DWORD64> rips;  // preallocated: the sampler must not allocate while a thread is stopped
  size_t count = 0;
};

std::mutex gMutex;
std::vector<std::unique_ptr<Sampled>> gThreads;
std::thread gSampler;
std::atomic<bool> gStop = false, gActive = false;

bool enabled() {
  static const bool on = std::getenv("CODPORT_SAMPLE") != nullptr;
  return on;
}

void loop() {
  timeBeginPeriod(1);
  while (!gStop) {
    Sleep(1);
    if (!gActive) continue;
    std::lock_guard lock(gMutex);
    for (auto& t : gThreads) {
      if (t->count == t->rips.size() || SuspendThread(t->thread) == DWORD(-1)) continue;
      CONTEXT context{};
      context.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(t->thread, &context)) t->rips[t->count++] = context.Rip;
      ResumeThread(t->thread);
    }
  }
  timeEndPeriod(1);
}
}  // namespace

void sampleThisThread(const std::string& name) {
  if (!enabled()) return;
  HANDLE handle;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
  std::lock_guard lock(gMutex);
  gThreads.push_back(std::make_unique<Sampled>(Sampled{handle, name, std::vector<DWORD64>(1 << 22)}));
  if (!gSampler.joinable()) gSampler = std::thread(loop);
}

void samplerFrame(uint64_t frame) {
  static const uint64_t from = enabled() ? std::strtoull(std::getenv("CODPORT_SAMPLE"), nullptr, 10) : ~0ull;
  if (frame >= from) gActive = true;
}

void samplerReport() {
  if (!enabled() || !gSampler.joinable()) return;
  gStop = true;
  gSampler.join();
  const HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  std::map<std::string, std::map<DWORD64, uint64_t>> byName;
  for (auto& t : gThreads)
    for (size_t i = 0; i < t->count; ++i) ++byName[t->name][t->rips[i]];
  for (auto& [name, hits] : byName) {
    uint64_t total = 0;
    std::map<std::string, uint64_t> functions, lines;
    for (auto& [rip, n] : hits) {
      total += n;
      alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
      auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = 255;
      DWORD64 offset = 0;
      std::string function = SymFromAddr(process, rip, &offset, symbol) ? symbol->Name : std::format("{:#x}", rip);
      functions[function] += n;
      IMAGEHLP_LINE64 line{};
      line.SizeOfStruct = sizeof(line);
      DWORD displacement = 0;
      // Code inlined into the function counts at its own line, in the innermost inlined function.
      DWORD context = 0, frame = 0;
      const bool inlined = SymAddrIncludeInlineTrace(process, rip) > 0 &&
                           SymQueryInlineTrace(process, rip, 0, rip, rip, &context, &frame) &&
                           SymGetLineFromInlineContext(process, rip, context, 0, &displacement, &line);
      if (inlined && SymFromInlineContext(process, rip, context, &offset, symbol)) function = symbol->Name;
      if (inlined || SymGetLineFromAddr64(process, rip, &displacement, &line)) {
        std::string file = line.FileName;
        file = file.substr(file.find_last_of("\\/") + 1);
        lines[std::format("{}:{} ({})", file, line.LineNumber, function)] += n;
      }
    }
    auto top = [&](const std::map<std::string, uint64_t>& m, size_t count) {
      std::vector<std::pair<uint64_t, std::string>> v;
      for (auto& [k, n] : m) v.push_back({n, k});
      std::sort(v.rbegin(), v.rend());
      for (size_t i = 0; i < v.size() && i < count; ++i)
        std::printf("   %5.1f%%  %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
    };
    std::printf("\n== samples: %s (%llu)\n  functions:\n", name.c_str(), (unsigned long long)total);
    top(functions, 20);
    std::printf("  lines:\n");
    top(lines, 40);
  }
  SymCleanup(process);
}
