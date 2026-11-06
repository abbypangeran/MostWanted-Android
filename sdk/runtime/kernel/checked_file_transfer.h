#pragma once

#include <cstddef>
#include <cstdint>

#include "kernel/memory.h"

namespace kernel::checked_file_transfer {

// Compatibility bound inferred from the guest's modeled 64 MiB Xbox RAM; no
// in-repository NtReadFile/NtWriteFile call sites expose a stronger size limit.
constexpr uint32_t kMaxStagedBytes = mem::kPhysicalSize;

enum class Status : uint8_t { kOk, kGuestMemory, kRequestTooLarge, kNoMemory, kHostContract };

struct Result {
    Status status = Status::kOk;
    mem::AccessStatus memory_status = mem::AccessStatus::kOk;
    uint32_t transferred = 0;
};

using HostRead = std::size_t (*)(void* context, uint8_t* destination, std::size_t size);
using HostWrite = std::size_t (*)(void* context, const uint8_t* source, std::size_t size);

Result ReadFromHost(uint32_t guest_buffer, uint32_t length, void* context, HostRead read);
Result WriteToHost(uint32_t guest_buffer, uint32_t length, void* context, HostWrite write);

}  // namespace kernel::checked_file_transfer
