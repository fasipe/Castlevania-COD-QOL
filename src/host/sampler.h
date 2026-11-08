#pragma once
#include <cstdint>
#include <string>

// A sampling profiler for finding hot spots: with CODPORT_SAMPLE set, the threads that call
// sampleThisThread() are interrupted about every millisecond and their instruction pointers
// counted; samplerReport() lists the hottest source lines per thread name (needs the PDB).
// CODPORT_SAMPLE=N starts sampling at frame N (samplerFrame is told each frame).
void sampleThisThread(const std::string& name);
void samplerFrame(uint64_t frame);
void samplerReport();
