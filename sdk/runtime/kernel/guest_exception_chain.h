#pragma once

#ifdef MOSTWANTED_GUEST_CALLBACK_TESTS

#include <array>
#include <cstdint>
#include <mutex>
#include <span>

#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/synthetic_worker.h"

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

#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
// Synthetic ABI: record=[0xE042D900,0], context=[ESP before bridge CALL,EBP].
// Handler arguments are (record, frame, context, 0); EAX=0 handles, EAX=1 searches next.
enum class SyntheticExceptionStatus : uint8_t {
    kHandled,
    kUnhandled,
    kInvalidOwner,
    kInvalidChain,
    kInvalidRecord,
    kInvalidContext,
    kInvalidTicket,
    kInvalidReturn,
    kUnsupportedDisposition,
    kReentrant,
};

struct SyntheticExceptionResult {
    SyntheticExceptionStatus status;
    uint32_t disposition = 0;
    uint32_t handlers_called = 0;
};

inline SyntheticExceptionResult DispatchSyntheticGuestExceptionForTest(
    X86Context& ctx, Thread& owner, uint32_t record_va, uint32_t context_va,
    std::span<const GuestCallbackTicket> tickets) {
    if (Current() != &owner || !SyntheticWorkerClaimHeld() || owner.state.load() != ThreadState::kActive ||
        owner.ctx != &ctx)
        return {SyntheticExceptionStatus::kInvalidOwner};
    static thread_local bool dispatching = false;
    if (dispatching) return {SyntheticExceptionStatus::kReentrant};
    const auto chain = ValidateGuestExceptionChain(ctx, owner);
    if (chain.status != GuestExceptionChainStatus::kValid || chain.frame_count != tickets.size())
        return {SyntheticExceptionStatus::kInvalidChain};

    dispatch::CallLease dispatch_lease;
    mem::ScopedPageLease code_lease, stack_lease, kpcr_lease, ethread_lease, record_lease, context_lease;
    const uint32_t stack_base = owner.stack_top - owner.stack_size;
    if (dispatch_lease.CodeEnd() <= dispatch_lease.CodeBase() ||
        code_lease.Acquire(dispatch_lease.CodeBase(), dispatch_lease.CodeEnd() - dispatch_lease.CodeBase(),
                           mem::kExecute) != mem::AccessStatus::kOk ||
        stack_lease.Acquire(stack_base, owner.stack_size, mem::kReadWrite) != mem::AccessStatus::kOk ||
        kpcr_lease.Acquire(owner.kpcr, 0x400, mem::kRead) != mem::AccessStatus::kOk ||
        ethread_lease.Acquire(owner.ethread, 0x200, mem::kRead) != mem::AccessStatus::kOk)
        return {SyntheticExceptionStatus::kInvalidChain};
    if ((record_va & 3u) || record_lease.Acquire(record_va, 8, mem::kRead) != mem::AccessStatus::kOk)
        return {SyntheticExceptionStatus::kInvalidRecord};
    if ((context_va & 3u) || context_lease.Acquire(context_va, 8, mem::kRead) != mem::AccessStatus::kOk)
        return {SyntheticExceptionStatus::kInvalidContext};

    std::array<uint32_t, 2> record{}, saved_context{};
    if (mem::Read(record_va, record.data(), sizeof(record)) != mem::AccessStatus::kOk ||
        record[0] != 0xE042D900 || record[1] != 0)
        return {SyntheticExceptionStatus::kInvalidRecord};
    if (mem::Read(context_va, saved_context.data(), sizeof(saved_context)) != mem::AccessStatus::kOk ||
        saved_context[0] != ctx.esp + 4 || saved_context[1] != ctx.ebp ||
        ctx.esp < stack_base || ctx.esp >= owner.stack_top)
        return {SyntheticExceptionStatus::kInvalidContext};
    const uint64_t callback_frame = uint64_t(ctx.esp) - 5 * sizeof(uint32_t);
    auto overlaps_callback = [&](uint32_t address) {
        return uint64_t(address) < ctx.esp && uint64_t(address) + 8 > callback_frame;
    };
    if (overlaps_callback(record_va) ||
        (uint64_t(record_va) < uint64_t(context_va) + 8 &&
         uint64_t(context_va) < uint64_t(record_va) + 8))
        return {SyntheticExceptionStatus::kInvalidRecord};
    if (overlaps_callback(context_va)) return {SyntheticExceptionStatus::kInvalidContext};

    std::array<uint32_t, 64> frames{}, handlers{};
    uint32_t frame = 0;
    if (mem::Read(owner.kpcr, &frame, sizeof(frame)) != mem::AccessStatus::kOk)
        return {SyntheticExceptionStatus::kInvalidChain};
    for (uint32_t i = 0; i < chain.frame_count; ++i) {
        std::array<uint32_t, 2> words{};
        if ((frame & 3u) || frame < ctx.esp || uint64_t(frame) + 8 > owner.stack_top ||
            mem::Read(frame, words.data(), sizeof(words)) != mem::AccessStatus::kOk)
            return {SyntheticExceptionStatus::kInvalidChain};
        for (uint32_t j = 0; j < i; ++j)
            if (frames[j] == frame) return {SyntheticExceptionStatus::kInvalidChain};
        if (tickets[i].target != words[1] || tickets[i].callback_class != GuestCallbackClass::kFixture ||
            tickets[i].dispatch_generation != dispatch_lease.Generation() ||
            dispatch_lease.Resolve(words[1]).kind != dispatch::TargetKind::kGeneratedFunction)
            return {SyntheticExceptionStatus::kInvalidTicket};
        frames[i] = frame;
        handlers[i] = words[1];
        frame = words[0];
    }
    if (frame != 0xFFFFFFFF) return {SyntheticExceptionStatus::kInvalidChain};

    dispatching = true;
    struct ClearDispatch { bool& value; ~ClearDispatch() { value = false; } } clear{dispatching};
    SyntheticExceptionResult outcome{SyntheticExceptionStatus::kUnhandled};
    for (uint32_t i = 0; i < chain.frame_count; ++i) {
        const std::array<uint32_t, 4> arguments{record_va, frames[i], context_va, 0};
        constexpr uint32_t host_return = 0xFFFFFFF0;
        X86CheckedCallFrame call_frame{ctx.checked_call, host_return, false};
        ctx.checked_call = &call_frame;
        struct RestoreCallFrame {
            X86Context& ctx;
            X86CheckedCallFrame& frame;
            ~RestoreCallFrame() { ctx.checked_call = frame.previous; }
        } restore{ctx, call_frame};
        uint32_t disposition = 0, final_esp = 0;
        const uint32_t entry_esp = ctx.esp;
        const GuestCallbackStatus callback = CallGuestCallbackForTest(ctx, tickets[i], handlers[i], arguments,
                                                                       stack_base, owner.stack_size, disposition,
                                                                       &final_esp);
        if (callback != GuestCallbackStatus::kInvoked) {
            outcome.status = SyntheticExceptionStatus::kInvalidTicket;
            return outcome;
        }
        ++outcome.handlers_called;
        if (!call_frame.returned || final_esp != entry_esp) {
            outcome.status = SyntheticExceptionStatus::kInvalidReturn;
            return outcome;
        }
        std::array<uint32_t, 2> current_record{}, current_context{};
        uint32_t current_head = 0;
        if (mem::Read(record_va, current_record.data(), sizeof(current_record)) != mem::AccessStatus::kOk ||
            mem::Read(context_va, current_context.data(), sizeof(current_context)) != mem::AccessStatus::kOk ||
            mem::Read(owner.kpcr, &current_head, sizeof(current_head)) != mem::AccessStatus::kOk ||
            current_record != record || current_context != saved_context || current_head != frames[0]) {
            outcome.status = SyntheticExceptionStatus::kInvalidChain;
            return outcome;
        }
        for (uint32_t j = 0; j < chain.frame_count; ++j) {
            std::array<uint32_t, 2> words{};
            if (mem::Read(frames[j], words.data(), sizeof(words)) != mem::AccessStatus::kOk ||
                words[0] != (j + 1 < chain.frame_count ? frames[j + 1] : 0xFFFFFFFF) ||
                words[1] != handlers[j]) {
                outcome.status = SyntheticExceptionStatus::kInvalidChain;
                return outcome;
            }
        }
        outcome.disposition = disposition;
        if (disposition == 0) {
            outcome.status = SyntheticExceptionStatus::kHandled;
            return outcome;
        }
        if (disposition != 1) {
            outcome.status = SyntheticExceptionStatus::kUnsupportedDisposition;
            return outcome;
        }
    }
    return outcome;
}
#endif

}  // namespace kernel

#endif
