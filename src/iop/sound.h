#pragma once
#include <memory>

#include "iop/sif.h"

class Spu2;

// SDRDRV.IRX: the RPC front end of the IOP sound library (libsd) that the EE drives with
// sceSdRemote, over the same SPU2 model the game's own sound driver (MODSD.IRX, run on the IOP)
// uses.
enum : uint32_t { kSdrServer = 0x80000701 };

std::unique_ptr<Sif::RpcServer> makeSoundServer(uint32_t sid, Spu2& spu);
