#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kernel/memory.h"

namespace kernel::checked_gpu_transfer {

constexpr std::size_t kMaxStagedBytes = mem::kPhysicalSize;

enum class Status : uint8_t { kOk, kInvalidLayout, kRequestTooLarge, kGuestMemory, kNoMemory, kReadbackFailure };

struct Layout {
    std::size_t row_bytes = 0;
    std::size_t source_pitch = 0;
    std::size_t source_bytes = 0;
    uint32_t guest_pitch = 0;
    std::size_t guest_bytes = 0;
    std::size_t packed_bytes = 0;
};

struct Result {
    Status status = Status::kOk;
    mem::AccessStatus memory_status = mem::AccessStatus::kOk;
};

struct LayoutResult {
    Status status = Status::kOk;
    Layout layout;
};

using Readback = std::size_t (*)(void* context, uint8_t* destination, std::size_t capacity);

LayoutResult MakeLayout(uint32_t width, uint32_t height, std::size_t source_pitch,
                        std::size_t destination_capacity);
Result Transfer(uint32_t guest_buffer, std::size_t destination_capacity, uint32_t width, uint32_t height,
                std::size_t source_pitch, void* context, Readback readback, std::vector<uint8_t>& published_pixels);

}  // namespace kernel::checked_gpu_transfer
