#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kernel/memory.h"
#include "xbe/xbe.h"

namespace kernel {

using ResolveThunk = bool (*)(uint32_t ordinal, uint32_t& address, bool& implemented, void* context);

struct ThunkMemory {
    mem::AccessStatus (*read)(uint32_t address, void* destination, std::size_t size) = nullptr;
    mem::AccessStatus (*write)(uint32_t address, const void* source, std::size_t size) = nullptr;
};

bool PatchKernelThunks(const xbe::Image& image, ResolveThunk resolve, void* context, std::vector<uint32_t>& missing,
                       std::string& error, ThunkMemory memory = {});

}  // namespace kernel
