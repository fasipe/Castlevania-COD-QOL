#pragma once
#include <filesystem>
#include <memory>

#include "iop/sif.h"

// MCSERV.IRX: memory cards for libmc. Port 0 holds a formatted 8 MB card whose filesystem is
// a host folder (one subfolder per save, like on the card); port 1 is empty.
enum : uint32_t { kMcServer = 0x80000400 };

std::unique_ptr<Sif::RpcServer> makeMemcardServer(uint32_t sid, const std::filesystem::path& root);
