#include "kernel/checked_gpu_transfer.h"

#include <cstring>
#include <limits>
#include <new>

namespace kernel::checked_gpu_transfer {

namespace {

constexpr uint64_t kGuestAddressSpaceSize = uint64_t(UINT32_MAX) + 1;

Result MemoryFailure(mem::AccessStatus status) { return {Status::kGuestMemory, status}; }

}  // namespace

LayoutResult MakeLayout(uint32_t width, uint32_t height, std::size_t source_pitch,
                        std::size_t destination_capacity) {
    if (!width || !height) return {Status::kInvalidLayout, {}};

    const uint64_t row_bytes = uint64_t(width) * 4;
    if (source_pitch < row_bytes || row_bytes > UINT64_MAX - 63) return {Status::kInvalidLayout, {}};
    const uint64_t guest_pitch = (row_bytes + 63) & ~uint64_t(63);
    if (source_pitch > UINT64_MAX / height || guest_pitch > UINT64_MAX / height || row_bytes > UINT64_MAX / height)
        return {Status::kInvalidLayout, {}};

    const uint64_t source_bytes = uint64_t(source_pitch) * height;
    const uint64_t guest_bytes = guest_pitch * height;
    const uint64_t packed_bytes = row_bytes * height;
    if (source_bytes > kMaxStagedBytes || guest_bytes > kMaxStagedBytes || packed_bytes > kMaxStagedBytes)
        return {Status::kRequestTooLarge, {}};
    if (guest_bytes > destination_capacity || guest_bytes > kGuestAddressSpaceSize ||
        row_bytes > std::numeric_limits<std::size_t>::max() || source_bytes > std::numeric_limits<std::size_t>::max() ||
        guest_bytes > std::numeric_limits<std::size_t>::max() || packed_bytes > std::numeric_limits<std::size_t>::max() ||
        guest_pitch > std::numeric_limits<uint32_t>::max())
        return {Status::kInvalidLayout, {}};

    return {Status::kOk,
            {std::size_t(row_bytes), source_pitch, std::size_t(source_bytes), uint32_t(guest_pitch),
             std::size_t(guest_bytes), std::size_t(packed_bytes)}};
}

Result Transfer(uint32_t guest_buffer, std::size_t destination_capacity, uint32_t width, uint32_t height,
                std::size_t source_pitch, void* context, Readback readback, std::vector<uint8_t>& published_pixels) {
    const LayoutResult checked = MakeLayout(width, height, source_pitch, destination_capacity);
    if (checked.status != Status::kOk) return {checked.status, mem::AccessStatus::kOk};
    if (checked.layout.guest_bytes > kGuestAddressSpaceSize - guest_buffer)
        return {Status::kInvalidLayout, mem::AccessStatus::kOk};
    if (!readback) return {Status::kReadbackFailure, mem::AccessStatus::kOk};

    mem::AccessStatus access = mem::CheckAccess(guest_buffer, checked.layout.guest_bytes, mem::kWrite);
    if (access != mem::AccessStatus::kOk) return MemoryFailure(access);

    std::vector<uint8_t> staging;
    try {
        staging.resize(checked.layout.source_bytes);
    } catch (const std::bad_alloc&) {
        return {Status::kNoMemory, mem::AccessStatus::kOk};
    }

    std::size_t transferred = 0;
    try {
        transferred = readback(context, staging.data(), staging.size());
    } catch (const std::bad_alloc&) {
        return {Status::kNoMemory, mem::AccessStatus::kOk};
    } catch (...) {
        return {Status::kReadbackFailure, mem::AccessStatus::kOk};
    }
    if (transferred != staging.size()) return {Status::kReadbackFailure, mem::AccessStatus::kOk};

    std::vector<uint8_t> packed;
    if (checked.layout.source_pitch == checked.layout.row_bytes) {
        packed = std::move(staging);
    } else {
        try {
            packed.resize(checked.layout.packed_bytes);
        } catch (const std::bad_alloc&) {
            return {Status::kNoMemory, mem::AccessStatus::kOk};
        }
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(packed.data() + std::size_t(y) * checked.layout.row_bytes,
                        staging.data() + std::size_t(y) * checked.layout.source_pitch, checked.layout.row_bytes);
    }

    mem::ScopedLock lock;
    access = mem::CheckAccess(guest_buffer, checked.layout.guest_bytes, mem::kWrite);
    if (access != mem::AccessStatus::kOk) return MemoryFailure(access);
    for (uint32_t y = 0; y < height; ++y) {
        access = mem::Write(guest_buffer + uint32_t(std::size_t(y) * checked.layout.guest_pitch),
                            packed.data() + std::size_t(y) * checked.layout.row_bytes, checked.layout.row_bytes);
        if (access != mem::AccessStatus::kOk) return MemoryFailure(access);
    }
    published_pixels = std::move(packed);
    return {};
}

}  // namespace kernel::checked_gpu_transfer
