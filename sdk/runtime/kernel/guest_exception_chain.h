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
#include "kernel/worker_lifecycle.h"

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
    kInvalidContinuation,
};

struct SyntheticExceptionResult {
    SyntheticExceptionStatus status;
    uint32_t disposition = 0;
    uint32_t handlers_called = 0;
};

// Synthetic continuation context layout, kept in guest memory:
//   +00 saved ESP before the bridge CALL, +04 saved EBP
//   +08 target EIP, +0C target ESP, +10 target EBP
//   +14 EAX, +18 EBX, +1C ECX, +20 EDX, +24 ESI, +28 EDI, +2C EFLAGS
// EFLAGS carries only the bits X86Context represents; unsupported bits reject.
inline constexpr uint32_t kSyntheticContinuationContextBytes = 0x40;
inline constexpr uint32_t kSyntheticContinuationSentinel = 0xFFFFFFF0u;
inline constexpr uint32_t kSyntheticEflagsAllowed = 0x00240ED7u;

struct SyntheticContinuationRequest {
    X86Context context{};
    uint32_t target = 0;
    uint64_t dispatch_generation = 0;
    uint64_t memory_generation = 0;
    Thread* owner = nullptr;
};

struct SyntheticGuestContinuation {
    const SyntheticContinuationRequest* request = nullptr;
};

inline SyntheticContinuationRequest& SyntheticContinuationBufferForTest() {
    static thread_local SyntheticContinuationRequest request;
    return request;
}

[[noreturn]] inline void ThrowSyntheticGuestContinuation(const SyntheticContinuationRequest& request) {
    SyntheticContinuationBufferForTest() = request;
    throw SyntheticGuestContinuation{&SyntheticContinuationBufferForTest()};
}

inline bool& SyntheticExceptionActiveForTest() {
    static thread_local bool active = false;
    return active;
}

inline SyntheticContinuationStatus BuildSyntheticContinuationRequestForTest(
    X86Context& ctx, Thread& owner, uint32_t context_va, uint64_t dispatch_generation,
    SyntheticContinuationRequest& request) {
    if (Current() != &owner || !SyntheticWorkerClaimHeld() || owner.state.load() != ThreadState::kActive ||
        owner.ctx != &ctx)
        return SyntheticContinuationStatus::kInvalidOwner;
    if (owner.memory_generation != mem::Generation() || !owner.kpcr || !owner.ethread || !owner.stack_top ||
        !owner.stack_size || ctx.fs_base != owner.kpcr)
        return SyntheticContinuationStatus::kStaleMemory;
    const uint32_t stack_bottom = owner.stack_top - owner.stack_size;
    if (!stack_bottom || stack_bottom >= owner.stack_top) return SyntheticContinuationStatus::kInvalidStack;

    mem::ScopedPageLease context_lease;
    if ((context_va & 3u) || context_lease.Acquire(context_va, kSyntheticContinuationContextBytes, mem::kRead) !=
                                  mem::AccessStatus::kOk)
        return SyntheticContinuationStatus::kInvalidContext;
    std::array<uint32_t, kSyntheticContinuationContextBytes / sizeof(uint32_t)> block{};
    if (mem::Read(context_va, block.data(), sizeof(block)) != mem::AccessStatus::kOk)
        return SyntheticContinuationStatus::kInvalidContext;
    if (uint64_t(ctx.esp) + 4 != block[0] || block[1] != ctx.ebp)
        return SyntheticContinuationStatus::kInvalidContext;

    const SyntheticContinuationFault fault = SyntheticContinuationFaultForTest();
    if (fault == SyntheticContinuationFault::kUnauthorizedTarget)
        return SyntheticContinuationStatus::kUnauthorizedTarget;
    if (fault == SyntheticContinuationFault::kInvalidStack) return SyntheticContinuationStatus::kInvalidStack;
    if (fault == SyntheticContinuationFault::kInvalidFlags) return SyntheticContinuationStatus::kInvalidContext;
    if (fault == SyntheticContinuationFault::kStaleDispatch) return SyntheticContinuationStatus::kStaleDispatch;
    if (fault == SyntheticContinuationFault::kStaleMemory) return SyntheticContinuationStatus::kStaleMemory;

    const uint32_t target = block[2];
    const uint32_t target_esp = block[3];
    const uint32_t target_ebp = block[4];
    const uint32_t eflags = block[11];
    if (!target || !SyntheticWorkerContinuationAuthorized(target, dispatch_generation))
        return SyntheticContinuationStatus::kUnauthorizedTarget;
    dispatch::CallLease lease;
    if (lease.Generation() != dispatch_generation) return SyntheticContinuationStatus::kStaleDispatch;
    const uint32_t code_base = lease.CodeBase(), code_end = lease.CodeEnd();
    if (code_end <= code_base || target < code_base || target >= code_end)
        return SyntheticContinuationStatus::kUnauthorizedTarget;
    const dispatch::ResolvedTarget resolved = lease.Resolve(target);
    if (resolved.kind != dispatch::TargetKind::kContinuation || !resolved.function)
        return SyntheticContinuationStatus::kInvalidTarget;
    if ((target_esp & 3u) || target_esp < uint64_t(stack_bottom) + 8 || target_esp > owner.stack_top - 4)
        return SyntheticContinuationStatus::kInvalidStack;
    if (target_ebp && ((target_ebp & 3u) || target_ebp < stack_bottom || target_ebp >= owner.stack_top))
        return SyntheticContinuationStatus::kInvalidStack;
    if (eflags & ~kSyntheticEflagsAllowed) return SyntheticContinuationStatus::kInvalidContext;

    mem::ScopedPageLease stack_lease;
    if (stack_lease.Acquire(stack_bottom, owner.stack_size, mem::kReadWrite) != mem::AccessStatus::kOk)
        return SyntheticContinuationStatus::kLeaseDenied;
    uint32_t sentinel = 0;
    if (mem::Read(target_esp, &sentinel, sizeof(sentinel)) != mem::AccessStatus::kOk ||
        sentinel != kSyntheticContinuationSentinel)
        return SyntheticContinuationStatus::kInvalidStack;

    request.context = ctx;
    request.context.eax = block[5];
    request.context.ebx = block[6];
    request.context.ecx = block[7];
    request.context.edx = block[8];
    request.context.esi = block[9];
    request.context.edi = block[10];
    request.context.esp = target_esp;
    request.context.ebp = target_ebp;
    request.context.cf = (eflags >> 0) & 1;
    request.context.pf = (eflags >> 2) & 1;
    request.context.af = (eflags >> 4) & 1;
    request.context.zf = (eflags >> 6) & 1;
    request.context.sf = (eflags >> 7) & 1;
    request.context.df = (eflags >> 10) & 1;
    request.context.of = (eflags >> 11) & 1;
    request.context.xflags = eflags & 0x00240000u;
    request.context.checked_call = nullptr;
    request.context.host = nullptr;
    request.context.last_icall = 0;
    request.target = target;
    request.dispatch_generation = dispatch_generation;
    request.memory_generation = mem::Generation();
    request.owner = &owner;
    return SyntheticContinuationStatus::kResumed;
}

inline SyntheticContinuationStatus ResumeSyntheticGuestContinuationForTest(
    X86Context& ctx, Thread& owner, const SyntheticGuestContinuation& continuation, uint32_t& result) {
    result = 0;
    auto fail = [](SyntheticContinuationStatus status) {
        SetLastSyntheticContinuationStatusForTest(status);
        return status;
    };
    if (!continuation.request) return fail(SyntheticContinuationStatus::kInvalidContext);
    const SyntheticContinuationRequest& request = *continuation.request;
    if (Current() != &owner || !SyntheticWorkerClaimHeld() || owner.ctx != &ctx ||
        owner.state.load() != ThreadState::kActive || request.owner != &owner)
        return fail(SyntheticContinuationStatus::kInvalidOwner);
    if (owner.stop_requested.load()) return fail(SyntheticContinuationStatus::kShutdown);
    CallSyntheticContinuationAdmissionHookForTest();
    if (Current() != &owner || !SyntheticWorkerClaimHeld() || owner.ctx != &ctx ||
        owner.state.load() != ThreadState::kActive)
        return fail(SyntheticContinuationStatus::kInvalidOwner);
    if (owner.stop_requested.load() || !KernelWorkersAccepting())
        return fail(SyntheticContinuationStatus::kShutdown);
    if (!SyntheticWorkerContinuationAuthorized(request.target, request.dispatch_generation))
        return fail(SyntheticContinuationStatus::kNotAuthorized);
    const uint64_t memory_generation = mem::Generation();
    if (!memory_generation || request.memory_generation != memory_generation ||
        owner.memory_generation != memory_generation)
        return fail(SyntheticContinuationStatus::kStaleMemory);

    dispatch::CallLease lease;
    if (lease.Generation() != request.dispatch_generation) return fail(SyntheticContinuationStatus::kStaleDispatch);
    const uint32_t code_base = lease.CodeBase(), code_end = lease.CodeEnd();
    if (code_end <= code_base || request.target < code_base || request.target >= code_end)
        return fail(SyntheticContinuationStatus::kInvalidTarget);
    const dispatch::ResolvedTarget resolved = lease.Resolve(request.target);
    if (resolved.kind != dispatch::TargetKind::kContinuation || !resolved.function)
        return fail(SyntheticContinuationStatus::kInvalidTarget);

    const uint32_t stack_bottom = owner.stack_top - owner.stack_size;
    const X86Context& snapshot = request.context;
    if (!stack_bottom || stack_bottom >= owner.stack_top || snapshot.fs_base != owner.kpcr ||
        snapshot.checked_call || snapshot.host || snapshot.top > 7)
        return fail(SyntheticContinuationStatus::kInvalidContext);
    if ((snapshot.esp & 3u) || snapshot.esp < uint64_t(stack_bottom) + 8 || snapshot.esp > owner.stack_top - 4)
        return fail(SyntheticContinuationStatus::kInvalidStack);
    if (snapshot.ebp &&
        ((snapshot.ebp & 3u) || snapshot.ebp < stack_bottom || snapshot.ebp >= owner.stack_top))
        return fail(SyntheticContinuationStatus::kInvalidStack);

    mem::ScopedPageLease code_lease, stack_lease;
    if (code_lease.Acquire(code_base, code_end - code_base, mem::kExecute) != mem::AccessStatus::kOk ||
        stack_lease.Acquire(stack_bottom, owner.stack_size, mem::kReadWrite) != mem::AccessStatus::kOk)
        return fail(SyntheticContinuationStatus::kLeaseDenied);
    uint32_t sentinel = 0;
    if (mem::Read(snapshot.esp, &sentinel, sizeof(sentinel)) != mem::AccessStatus::kOk ||
        sentinel != kSyntheticContinuationSentinel)
        return fail(SyntheticContinuationStatus::kInvalidStack);

    ctx = snapshot;
    ctx.checked_call = nullptr;
    ctx.host = nullptr;
    ctx.last_icall = request.target;
    resolved.function(ctx, mem::Base());
    if (ctx.esp != uint64_t(snapshot.esp) + 4 || ctx.checked_call != nullptr)
        return fail(SyntheticContinuationStatus::kInvalidReturn);
    SetLastSyntheticContinuationStatusForTest(SyntheticContinuationStatus::kResumed);
    result = ctx.eax;
    return SyntheticContinuationStatus::kResumed;
}

enum class SyntheticUnwindStatus : uint8_t {
    kContinued,
    kInvalidOwner,
    kInvalidChain,
    kInvalidTarget,
    kInvalidTicket,
    kInvalidReturn,
    kUnsupportedDisposition,
    kInvalidContext,
    kReentrant,
};

struct SyntheticUnwindResult {
    SyntheticUnwindStatus status;
    uint32_t handlers_called = 0;
};

inline SyntheticUnwindResult SyntheticRtlUnwindForTest(
    X86Context& ctx, Thread& owner, uint32_t record_va, uint32_t target_frame, uint32_t target_ip,
    uint32_t context_va, std::span<const GuestCallbackTicket> tickets) {
    if (Current() != &owner || !SyntheticWorkerClaimHeld() || owner.state.load() != ThreadState::kActive ||
        owner.ctx != &ctx)
        return {SyntheticUnwindStatus::kInvalidOwner, 0};
    if (SyntheticExceptionActiveForTest()) return {SyntheticUnwindStatus::kReentrant, 0};
    const auto chain = ValidateGuestExceptionChain(ctx, owner);
    if (chain.status != GuestExceptionChainStatus::kValid || chain.frame_count != tickets.size())
        return {SyntheticUnwindStatus::kInvalidChain, 0};
    mem::ScopedPageLease record_lease, context_lease;
    if ((record_va & 3u) || record_lease.Acquire(record_va, 8, mem::kRead) != mem::AccessStatus::kOk)
        return {SyntheticUnwindStatus::kInvalidChain, 0};
    if ((context_va & 3u) ||
        context_lease.Acquire(context_va, kSyntheticContinuationContextBytes, mem::kRead) !=
            mem::AccessStatus::kOk)
        return {SyntheticUnwindStatus::kInvalidContext, 0};
    std::array<uint32_t, 2> synthetic_record{};
    if (mem::Read(record_va, synthetic_record.data(), sizeof(synthetic_record)) != mem::AccessStatus::kOk ||
        synthetic_record[0] != 0xE042D900 || synthetic_record[1] != 0)
        return {SyntheticUnwindStatus::kInvalidChain, 0};

    dispatch::CallLease dispatch_lease;
    mem::ScopedPageLease code_lease, stack_lease, kpcr_lease, ethread_lease;
    const uint32_t stack_base = owner.stack_top - owner.stack_size;
    if (dispatch_lease.CodeEnd() <= dispatch_lease.CodeBase() ||
        code_lease.Acquire(dispatch_lease.CodeBase(), dispatch_lease.CodeEnd() - dispatch_lease.CodeBase(),
                           mem::kExecute) != mem::AccessStatus::kOk ||
        stack_lease.Acquire(stack_base, owner.stack_size, mem::kReadWrite) != mem::AccessStatus::kOk ||
        kpcr_lease.Acquire(owner.kpcr, 0x400, mem::kRead) != mem::AccessStatus::kOk ||
        ethread_lease.Acquire(owner.ethread, 0x200, mem::kRead) != mem::AccessStatus::kOk)
        return {SyntheticUnwindStatus::kInvalidChain, 0};

    std::array<uint32_t, 64> frames{}, handlers{};
    uint32_t frame = 0;
    if (mem::Read(owner.kpcr, &frame, sizeof(frame)) != mem::AccessStatus::kOk)
        return {SyntheticUnwindStatus::kInvalidChain, 0};
    for (uint32_t i = 0; i < chain.frame_count; ++i) {
        std::array<uint32_t, 2> words{};
        if ((frame & 3u) || frame < ctx.esp || uint64_t(frame) + 8 > owner.stack_top ||
            mem::Read(frame, words.data(), sizeof(words)) != mem::AccessStatus::kOk)
            return {SyntheticUnwindStatus::kInvalidChain, 0};
        for (uint32_t j = 0; j < i; ++j)
            if (frames[j] == frame) return {SyntheticUnwindStatus::kInvalidChain, 0};
        if (tickets[i].target != words[1] || tickets[i].callback_class != GuestCallbackClass::kFixture ||
            tickets[i].dispatch_generation != dispatch_lease.Generation() ||
            dispatch_lease.Resolve(words[1]).kind != dispatch::TargetKind::kGeneratedFunction)
            return {SyntheticUnwindStatus::kInvalidTicket, 0};
        frames[i] = frame;
        handlers[i] = words[1];
        frame = words[0];
    }
    if (frame != 0xFFFFFFFF) return {SyntheticUnwindStatus::kInvalidChain, 0};

    uint32_t target_index = UINT32_MAX;
    for (uint32_t i = 0; i < chain.frame_count; ++i)
        if (frames[i] == target_frame) target_index = i;
    if (target_index == UINT32_MAX) return {SyntheticUnwindStatus::kInvalidTarget, 0};

    SyntheticContinuationRequest request{};
    const SyntheticContinuationStatus build = BuildSyntheticContinuationRequestForTest(
        ctx, owner, context_va, dispatch_lease.Generation(), request);
    if (build != SyntheticContinuationStatus::kResumed)
        return {SyntheticUnwindStatus::kInvalidContext, 0};
    if (request.target != target_ip) return {SyntheticUnwindStatus::kInvalidTarget, 0};

    std::array<uint32_t, kSyntheticContinuationContextBytes / sizeof(uint32_t)> context_before{};
    if (mem::Read(context_va, context_before.data(), sizeof(context_before)) != mem::AccessStatus::kOk)
        return {SyntheticUnwindStatus::kInvalidContext, 0};

    bool& active = SyntheticExceptionActiveForTest();
    active = true;
    struct ClearActive { bool& value; ~ClearActive() { value = false; } } clear{active};
    SyntheticUnwindResult outcome{SyntheticUnwindStatus::kContinued, 0};
    for (uint32_t i = 0; i < target_index; ++i) {
        const std::array<uint32_t, 4> arguments{record_va, frames[i], context_va, 2};
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
        if (callback != GuestCallbackStatus::kInvoked) return {SyntheticUnwindStatus::kInvalidTicket, outcome.handlers_called};
        ++outcome.handlers_called;
        if (!call_frame.returned || final_esp != entry_esp)
            return {SyntheticUnwindStatus::kInvalidReturn, outcome.handlers_called};
        if (disposition != 0) return {SyntheticUnwindStatus::kUnsupportedDisposition, outcome.handlers_called};

        std::array<uint32_t, kSyntheticContinuationContextBytes / sizeof(uint32_t)> context_after{};
        std::array<uint32_t, 2> record{};
        uint32_t current_head = 0;
        if (mem::Read(context_va, context_after.data(), sizeof(context_after)) != mem::AccessStatus::kOk ||
            context_after != context_before || mem::Read(frames[i], record.data(), sizeof(record)) != mem::AccessStatus::kOk ||
            record[0] != (i + 1 < chain.frame_count ? frames[i + 1] : 0xFFFFFFFF) || record[1] != handlers[i] ||
            mem::Read(owner.kpcr, &current_head, sizeof(current_head)) != mem::AccessStatus::kOk ||
            current_head != frames[0])
            return {SyntheticUnwindStatus::kInvalidChain, outcome.handlers_called};
    }

    uint32_t head_before_commit = 0;
    std::array<uint32_t, 2> target_record{};
    if (mem::Read(owner.kpcr, &head_before_commit, sizeof(head_before_commit)) != mem::AccessStatus::kOk ||
        head_before_commit != frames[0] ||
        mem::Read(target_frame, target_record.data(), sizeof(target_record)) != mem::AccessStatus::kOk ||
        target_record[0] != (target_index + 1 < chain.frame_count ? frames[target_index + 1] : 0xFFFFFFFF) ||
        target_record[1] != handlers[target_index])
        return {SyntheticUnwindStatus::kInvalidChain, outcome.handlers_called};
    if (mem::Write(owner.kpcr, &target_frame, sizeof(target_frame)) != mem::AccessStatus::kOk)
        return {SyntheticUnwindStatus::kInvalidChain, outcome.handlers_called};
    uint32_t committed = 0;
    if (mem::Read(owner.kpcr, &committed, sizeof(committed)) != mem::AccessStatus::kOk ||
        committed != target_frame)
        return {SyntheticUnwindStatus::kInvalidChain, outcome.handlers_called};
    ThrowSyntheticGuestContinuation(request);
}

inline SyntheticExceptionResult DispatchSyntheticGuestExceptionForTest(
    X86Context& ctx, Thread& owner, uint32_t record_va, uint32_t context_va,
    std::span<const GuestCallbackTicket> tickets) {
    if (Current() != &owner || !SyntheticWorkerClaimHeld() || owner.state.load() != ThreadState::kActive ||
        owner.ctx != &ctx)
        return {SyntheticExceptionStatus::kInvalidOwner};
    if (SyntheticExceptionActiveForTest()) return {SyntheticExceptionStatus::kReentrant};
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

    bool& active = SyntheticExceptionActiveForTest();
    active = true;
    struct ClearDispatch { bool& value; ~ClearDispatch() { value = false; } } clear{active};
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
        if (disposition == 2) {
            SyntheticContinuationRequest request{};
            if (BuildSyntheticContinuationRequestForTest(ctx, owner, context_va, dispatch_lease.Generation(),
                                                         request) != SyntheticContinuationStatus::kResumed) {
                outcome.status = SyntheticExceptionStatus::kInvalidContinuation;
                return outcome;
            }
            ThrowSyntheticGuestContinuation(request);
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
