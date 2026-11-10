#pragma once

#ifdef MOSTWANTED_GUEST_CALLBACK_TESTS

#include <array>
#include <cstdint>
#include <mutex>

#include "cpu/dispatch_table.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace kernel {

enum class GuestExceptionChainStatus : uint8_t {
    kValid,
    kInvalidOwner,
    kMemoryUnavailable,
    kInvalidFrame,
    kUnmappedHandler,
    kCycle,
    kTooDeep,
};

struct GuestExceptionChainValidation {
    GuestExceptionChainStatus status;
    uint32_t frame_count;
};

inline GuestExceptionChainValidation ValidateGuestExceptionChain(const X86Context& ctx, Thread& owner) {
    constexpr uint32_t kKpcrSize = 0x400;
    constexpr uint32_t kEthreadSize = 0x200;
    constexpr uint32_t kMaximumFrames = 64;  // shortcut: 64 records, raise only after retail stack limits are verified.
    constexpr uint32_t kEmptyChain = 0xFFFFFFFF;

    mem::ScopedVirtualRangeLock memory_range_lock;
    std::lock_guard lifecycle_lock(owner.lifecycle_lock);
    const ThreadState state = owner.state.load(std::memory_order_acquire);
    const uint64_t stack_bottom = uint64_t(owner.stack_top) - owner.stack_size;
    if (owner.ctx != &ctx || (state != ThreadState::kPrepared && state != ThreadState::kActive) ||
        !owner.memory_owner || owner.memory_generation != mem::Generation() || !owner.kpcr || !owner.ethread ||
        !owner.stack_top || !owner.stack_size || stack_bottom == 0 || stack_bottom >= owner.stack_top ||
        owner.kpcr > UINT32_MAX - kKpcrSize || owner.ethread > UINT32_MAX - kEthreadSize ||
        ctx.fs_base != owner.kpcr)
        return {GuestExceptionChainStatus::kInvalidOwner, 0};

    mem::ScopedPageLease kpcr_lease;
    mem::ScopedPageLease ethread_lease;
    mem::ScopedPageLease stack_lease;
    if (kpcr_lease.Acquire(owner.kpcr, kKpcrSize, mem::kRead) != mem::AccessStatus::kOk ||
        ethread_lease.Acquire(owner.ethread, kEthreadSize, mem::kRead) != mem::AccessStatus::kOk ||
        stack_lease.Acquire(uint32_t(stack_bottom), owner.stack_size, mem::kRead) != mem::AccessStatus::kOk)
        return {GuestExceptionChainStatus::kMemoryUnavailable, 0};

    std::array<uint32_t, 11> kpcr{};
    std::array<uint32_t, 9> ethread{};
    if (mem::Read(owner.kpcr, kpcr.data(), sizeof(kpcr)) != mem::AccessStatus::kOk ||
        mem::Read(owner.ethread + 0x1C, ethread.data(), sizeof(ethread)) != mem::AccessStatus::kOk)
        return {GuestExceptionChainStatus::kMemoryUnavailable, 0};
    if (kpcr[1] != owner.stack_top || kpcr[2] != stack_bottom || kpcr[6] != owner.kpcr ||
        kpcr[7] != owner.kpcr || kpcr[8] != owner.kpcr + 0x28 ||
        kpcr[10] != owner.ethread || ethread[0] != owner.stack_top || ethread[1] != stack_bottom)
        return {GuestExceptionChainStatus::kInvalidOwner, 0};

    dispatch::CallLease dispatch_lease;
    const uint32_t code_base = dispatch_lease.CodeBase();
    const uint32_t code_end = dispatch_lease.CodeEnd();
    mem::ScopedPageLease code_lease;
    if (code_end <= code_base ||
        code_lease.Acquire(code_base, code_end - code_base, mem::kExecute) != mem::AccessStatus::kOk)
        return {GuestExceptionChainStatus::kMemoryUnavailable, 0};

    std::array<uint32_t, kMaximumFrames> visited{};
    uint32_t frame = kpcr[0];
    uint32_t count = 0;
    while (frame != kEmptyChain) {
        if ((frame & 3u) || frame < stack_bottom || uint64_t(frame) + 8 > owner.stack_top)
            return {GuestExceptionChainStatus::kInvalidFrame, count};
        for (uint32_t i = 0; i < count; ++i)
            if (visited[i] == frame) return {GuestExceptionChainStatus::kCycle, count};
        if (count == kMaximumFrames) return {GuestExceptionChainStatus::kTooDeep, count};
        visited[count++] = frame;

        std::array<uint32_t, 2> record{};
        if (mem::Read(frame, record.data(), sizeof(record)) != mem::AccessStatus::kOk)
            return {GuestExceptionChainStatus::kMemoryUnavailable, count};
        const dispatch::ResolvedTarget handler = dispatch_lease.Resolve(record[1]);
        if (handler.kind != dispatch::TargetKind::kGeneratedFunction || !handler.function)
            return {GuestExceptionChainStatus::kUnmappedHandler, count};
        frame = record[0];
    }
    return {GuestExceptionChainStatus::kValid, count};
}

}  // namespace kernel

#endif
