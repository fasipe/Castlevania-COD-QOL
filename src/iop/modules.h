#pragma once
#include <memory>

#include "iop/sif.h"

// Services of the IOP's built-in (rom0) modules.
enum : uint32_t {
  kIopHeapServer = 0x80000003,
  kLoadfileServer = 0x80000006,
};

std::unique_ptr<Sif::RpcServer> makeSystemServer(uint32_t sid);
