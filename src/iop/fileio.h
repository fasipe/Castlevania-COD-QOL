#pragma once
#include <memory>

#include "iop/sif.h"

// FILEIO (rom0): stdio-style file access for the EE (open/read/lseek/close), here on the disc.
enum : uint32_t { kFileioServer = 0x80000001 };

std::unique_ptr<Sif::RpcServer> makeFileioServer(uint32_t sid);
