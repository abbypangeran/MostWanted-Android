#pragma once

#include <cstdint>

#include "cpu/dispatch_table.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/thread_memory.h"

namespace kernel::seh {

// The reviewed frame contract of the authentic SEH prologue at 00319804.
//
// Let E be the caller's ESP at the `call` into the prologue, i.e. the address of
// the last argument it pushed. The helper pushes the handler and the previous
// fs:[0] below that, then reuses the caller's two argument slots for the scope
// flag and the scope table:
//
//   E+0x04  caller EBP (was the size argument)
//   E+0x00  scope flag, -1 while the frame is live (was the table argument)
//   E-0x04  scope table after the helper returns (its own return address before)
//   E-0x08  handler
//   E-0x0C  previous fs:[0], and the registration address the helper installs
//   E-0x14  saved ESP after the prologue (E-0x30 for the reviewed 0x18 frame)
//   E-0x30  ESP after the prologue; EBP is E+4
//
// The helper consumes only its own return address; the caller's own return
// address and arguments stay intact. 002B5C0F therefore could return, but it
// terminates the thread instead (002B5CA0). 002B5BD3 is a different routine
// reached from the exception path and is not an SEH epilogue.
inline constexpr uint32_t kRegistrationBelowCallEsp = 0x0C;
inline constexpr uint32_t kHandlerBelowCallEsp = 0x08;
inline constexpr uint32_t kScopeTableBelowCallEsp = 0x04;
inline constexpr uint32_t kSavedEspBelowCallEsp = 0x14;
inline constexpr uint32_t kFrameAboveCallEsp = 0x04;  // caller EBP
inline constexpr uint32_t kSavedRegistersBytes = 0x0C;  // ebx, esi, edi
inline constexpr uint32_t kEmptyChain = 0xFFFFFFFF;

enum class PrologueStatus : uint8_t {
    kVerified,
    kInvalidOwner,
    kMemoryUnavailable,
    kInvalidFrame,
    kInvalidScope,
    kInvalidChain,
    kInvalidHandler,
    kInvalidReturn,
};

inline const char* PrologueStatusName(PrologueStatus status) {
    switch (status) {
        case PrologueStatus::kVerified: return "verified";
        case PrologueStatus::kInvalidOwner: return "invalid owner";
        case PrologueStatus::kMemoryUnavailable: return "memory unavailable";
        case PrologueStatus::kInvalidFrame: return "invalid frame";
        case PrologueStatus::kInvalidScope: return "invalid scope record";
        case PrologueStatus::kInvalidChain: return "invalid exception chain";
        case PrologueStatus::kInvalidHandler: return "invalid handler target";
        case PrologueStatus::kInvalidReturn: return "invalid normal return";
    }
    return "unknown prologue status";
}

// call_esp is E. previous_head is fs:[0] before the registration; caller_ebp
// and handler are the values the prologue must have placed.
struct PrologueFrame {
    uint32_t call_esp = 0;
    uint32_t scope_bytes = 0;
    uint32_t scope_table = 0;
    uint32_t handler = 0;
    uint32_t previous_head = kEmptyChain;
    uint32_t caller_ebp = 0;  // 0 skips the check
};

struct PrologueResult {
    PrologueStatus status = PrologueStatus::kInvalidOwner;
    uint32_t detail = 0;  // failing address
};

namespace detail {

inline PrologueResult PrologueFail(PrologueStatus status, uint32_t detail) { return {status, detail}; }

// Owner, generation and block checks shared by both verifications. The caller
// keeps the returned leases alive for its reads.
inline PrologueResult AcquireFrameMemory(const X86Context& ctx, const Thread& owner, mem::ScopedPageLease& kpcr,
                                         mem::ScopedPageLease& stack, uint32_t& stack_bottom) {
    if (!mem::IsInitialized() || !mem::Generation()) return PrologueFail(PrologueStatus::kInvalidOwner, 0);
    const ThreadState state = owner.state.load(std::memory_order_acquire);
    stack_bottom = owner.stack_top - owner.stack_size;
    if (owner.ctx != &ctx || (state != ThreadState::kPrepared && state != ThreadState::kActive) ||
        !owner.memory_owner || owner.memory_generation != mem::Generation() || !owner.kpcr || !owner.ethread ||
        !owner.stack_top || !owner.stack_size || !stack_bottom || stack_bottom >= owner.stack_top ||
        owner.kpcr > UINT32_MAX - kKpcrSize || owner.ethread > UINT32_MAX - kEthreadSize ||
        ctx.fs_base != owner.kpcr)
        return PrologueFail(PrologueStatus::kInvalidOwner, ctx.fs_base);
    if (kpcr.Acquire(owner.kpcr, kKpcrSize, mem::kRead) != mem::AccessStatus::kOk ||
        stack.Acquire(stack_bottom, owner.stack_size, mem::kRead) != mem::AccessStatus::kOk)
        return PrologueFail(PrologueStatus::kMemoryUnavailable, stack_bottom);
    return {PrologueStatus::kVerified, 0};
}

inline bool InStack(uint32_t stack_bottom, uint32_t stack_top, uint32_t address, uint32_t bytes) {
    return address >= stack_bottom && uint64_t(address) + bytes <= stack_top;
}

inline PrologueResult ReadWord(uint32_t address, uint32_t& value) {
    return mem::Read(address, &value, sizeof(value)) == mem::AccessStatus::kOk
               ? PrologueResult{PrologueStatus::kVerified, 0}
               : PrologueFail(PrologueStatus::kMemoryUnavailable, address);
}

}  // namespace detail

// Verifies the live frame: the registration is the chain head, the record holds
// {previous, handler}, the scope flag is -1, the scope table and saved ESP are
// where the caller expects them, and EBP/ESP have the post-prologue values.
// The handler must resolve to a generated function; it is never called.
inline PrologueResult VerifyRegistration(const X86Context& ctx, const Thread& owner, const PrologueFrame& frame) {
    mem::ScopedVirtualRangeLock range_lock;
    mem::ScopedPageLease kpcr_lease, stack_lease;
    uint32_t stack_bottom = 0;
    const PrologueResult owner_result = detail::AcquireFrameMemory(ctx, owner, kpcr_lease, stack_lease, stack_bottom);
    if (owner_result.status != PrologueStatus::kVerified) return owner_result;

    const uint32_t e = frame.call_esp;
    if (!e || (e & 3u) || e > UINT32_MAX - 0x10) return detail::PrologueFail(PrologueStatus::kInvalidFrame, e);
    const uint32_t registration = e - kRegistrationBelowCallEsp;
    const uint32_t handler_slot = e - kHandlerBelowCallEsp;
    const uint32_t scope_slot = e - kScopeTableBelowCallEsp;
    const uint32_t saved_esp_slot = e - kSavedEspBelowCallEsp;
    const uint32_t post_prologue_esp = e - frame.scope_bytes - kSavedRegistersBytes - 0x0C;
    for (uint32_t address : {registration, handler_slot, scope_slot, saved_esp_slot, e, e + 4}) {
        if (!detail::InStack(stack_bottom, owner.stack_top, address, 4))
            return detail::PrologueFail(PrologueStatus::kInvalidFrame, address);
    }
    if (ctx.ebp != e + kFrameAboveCallEsp) return detail::PrologueFail(PrologueStatus::kInvalidFrame, ctx.ebp);
    // The caller may use the stack below the prologue frame while the record is
    // live, but it can never move above the saved post-prologue ESP.
    if (ctx.esp > post_prologue_esp || ctx.esp < stack_bottom)
        return detail::PrologueFail(PrologueStatus::kInvalidFrame, ctx.esp);

    uint32_t previous = 0, handler = 0, scope_flag = 0, scope_table = 0, saved_esp = 0, caller_ebp = 0, head = 0;
    for (const auto& read : {detail::ReadWord(registration, previous), detail::ReadWord(handler_slot, handler),
                             detail::ReadWord(e, scope_flag), detail::ReadWord(scope_slot, scope_table),
                             detail::ReadWord(saved_esp_slot, saved_esp), detail::ReadWord(e + 4, caller_ebp),
                             detail::ReadWord(owner.kpcr, head)}) {
        if (read.status != PrologueStatus::kVerified) return read;
    }
    if (previous != frame.previous_head) return detail::PrologueFail(PrologueStatus::kInvalidChain, previous);
    // The frame may be the head or sit under records the caller registered
    // later; it must be on the chain, and the chain must stay inside the stack.
    uint32_t link = head;
    bool found = false;
    for (uint32_t depth = 0; depth < 64 && link != kEmptyChain; ++depth) {
        if ((link & 3u) || !detail::InStack(stack_bottom, owner.stack_top, link, 8))
            return detail::PrologueFail(PrologueStatus::kInvalidChain, link);
        if (link == registration) {
            found = true;
            break;
        }
        uint32_t next = 0;
        const PrologueResult read = detail::ReadWord(link, next);
        if (read.status != PrologueStatus::kVerified) return read;
        link = next;
    }
    if (!found) return detail::PrologueFail(PrologueStatus::kInvalidChain, head);
    if (handler != frame.handler) return detail::PrologueFail(PrologueStatus::kInvalidHandler, handler);
    if (scope_flag != 0xFFFFFFFF || scope_table != frame.scope_table)
        return detail::PrologueFail(PrologueStatus::kInvalidScope, scope_table);
    if (frame.caller_ebp && caller_ebp != frame.caller_ebp)
        return detail::PrologueFail(PrologueStatus::kInvalidFrame, caller_ebp);
    if (saved_esp != post_prologue_esp) return detail::PrologueFail(PrologueStatus::kInvalidFrame, saved_esp);

    dispatch::CallLease dispatch_lease;
    const dispatch::ResolvedTarget resolved = dispatch_lease.Resolve(handler);
    if (resolved.kind != dispatch::TargetKind::kGeneratedFunction || !resolved.function)
        return detail::PrologueFail(PrologueStatus::kInvalidHandler, handler);
    if (frame.scope_table) {
        uint32_t scope_first = 0;
        const PrologueResult scope_read = detail::ReadWord(frame.scope_table, scope_first);
        if (scope_read.status != PrologueStatus::kVerified) return scope_read;
        // the reviewed table is empty: its first dword is the -1 terminator.
        if (scope_first != 0xFFFFFFFF)
            return detail::PrologueFail(PrologueStatus::kInvalidScope, scope_first);
    }
    return {PrologueStatus::kVerified, registration};
}

// Verifies that the caller unlinked the frame: fs:[0] is the previous head
// again, the record words are unchanged, and the caller's stack pointer and
// frame pointer are back where they were before the prologue call.
inline PrologueResult VerifyNormalReturn(const X86Context& ctx, const Thread& owner, const PrologueFrame& frame,
                                         uint32_t expected_esp, uint32_t expected_ebp) {
    mem::ScopedVirtualRangeLock range_lock;
    mem::ScopedPageLease kpcr_lease, stack_lease;
    uint32_t stack_bottom = 0;
    const PrologueResult owner_result = detail::AcquireFrameMemory(ctx, owner, kpcr_lease, stack_lease, stack_bottom);
    if (owner_result.status != PrologueStatus::kVerified) return owner_result;

    const uint32_t registration = frame.call_esp - kRegistrationBelowCallEsp;
    const uint32_t handler_slot = frame.call_esp - kHandlerBelowCallEsp;
    if (!detail::InStack(stack_bottom, owner.stack_top, registration, 4) ||
        !detail::InStack(stack_bottom, owner.stack_top, handler_slot, 4))
        return detail::PrologueFail(PrologueStatus::kInvalidFrame, registration);

    uint32_t head = 0, previous = 0, handler = 0;
    for (const auto& read : {detail::ReadWord(owner.kpcr, head), detail::ReadWord(registration, previous),
                             detail::ReadWord(handler_slot, handler)}) {
        if (read.status != PrologueStatus::kVerified) return read;
    }
    if (head != frame.previous_head) return detail::PrologueFail(PrologueStatus::kInvalidChain, head);
    if (previous != frame.previous_head || handler != frame.handler)
        return detail::PrologueFail(PrologueStatus::kInvalidFrame, registration);
    if (expected_esp && ctx.esp != expected_esp)
        return detail::PrologueFail(PrologueStatus::kInvalidReturn, ctx.esp);
    if (expected_ebp && ctx.ebp != expected_ebp)
        return detail::PrologueFail(PrologueStatus::kInvalidReturn, ctx.ebp);
    return {PrologueStatus::kVerified, registration};
}

}  // namespace kernel::seh
