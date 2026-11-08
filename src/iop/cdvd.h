#pragma once
#include <memory>

#include "iop/sif.h"

// The disc drive services the IOP module cdvdfsv provides to libcdvd on the EE. Sectors come
// straight from the original disc image; the drive is always ready and never errors.
// Protocol details were taken from the game's own libcdvd (around 0x0074E000 in SLES_537.55).
enum : uint32_t {
  kCdInitServer = 0x80000592,
  kCdSCmdServer = 0x80000593,
  kCdNCmdServer = 0x80000595,
  kCdSearchFileServer = 0x80000597,
  kCdDiskReadyServer = 0x8000059A,
  kCdDiskReady2Server = 0x8000059C,
};

std::unique_ptr<Sif::RpcServer> makeCdvdServer(uint32_t sid);
