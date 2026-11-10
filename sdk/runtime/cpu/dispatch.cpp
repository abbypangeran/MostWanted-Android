#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
#include "kernel/synthetic_worker.h"
#endif

namespace {

dispatch::Table g_dispatch;
std::shared_mutex g_dispatch_lock;
uint64_t g_dispatch_generation = 1;
thread_local size_t g_call_lease_depth = 0;
constexpr uint32_t kHostThunkBase = 0xFD000000;
// synthetic guest addresses for kernel functions reached through a pointer
constexpr uint32_t kKernelThunkBase = 0xFE000000;
std::vector<X86Func*> g_host_thunks;
std::mutex g_host_lock;

struct Export {
    const char* name = nullptr;
    kernel::KernelFn fn = nullptr;
    uint32_t data = 0;
};
Export g_exports[400];
#ifdef MOSTWANTED_CHECKED_KERNEL_ABI_TESTS
struct FixtureBridge {
    uint32_t ordinal;
    const char* name;
    uint32_t arguments;
    bool enabled;
};
FixtureBridge g_fixture_bridges[] = {
    {255, "PsCreateSystemThreadEx", 10, false},
    {187, "NtClose", 1, false},
    {277, "RtlEnterCriticalSection", 1, false},
    {294, "RtlLeaveCriticalSection", 1, false},
    {258, "PsTerminateSystemThread", 1, false},
};

bool SetFixtureBridge(uint32_t ordinal, bool enabled) {
    for (FixtureBridge& bridge : g_fixture_bridges) {
        if (bridge.ordinal != ordinal) continue;
        if (enabled && (!g_exports[ordinal].fn || !g_exports[ordinal].name ||
                        std::strcmp(g_exports[ordinal].name, bridge.name) != 0))
            return false;
        bridge.enabled = enabled;
        return true;
    }
    return false;
}

FixtureBridge* FindFixtureBridge(uint32_t target) {
    for (FixtureBridge& bridge : g_fixture_bridges)
        if (bridge.enabled && target == kKernelThunkBase + bridge.ordinal * 4) return &bridge;
    return nullptr;
}
#endif

}  // namespace

namespace {

dispatch::ResolvedTarget ResolveTargetLocked(uint32_t guest) {
    if (!guest) return {dispatch::TargetKind::kNull, nullptr};
    if (guest >= kKernelThunkBase && guest < kKernelThunkBase + 400 * 4)
        return {dispatch::TargetKind::kKernelThunk, nullptr};
    if (guest >= kHostThunkBase) {
        const size_t index = (guest - kHostThunkBase) / 4;
        std::lock_guard<std::mutex> lock(g_host_lock);
        if (index < g_host_thunks.size() && g_host_thunks[index])
            return {dispatch::TargetKind::kHostThunk, g_host_thunks[index]};
        return {dispatch::TargetKind::kUnknown, nullptr};
    }
    if (!g_dispatch.Ready()) return {dispatch::TargetKind::kUninitialized, nullptr};
    if (g_dispatch.IsContinuation(guest))
        return {dispatch::TargetKind::kContinuation, g_dispatch.Find(guest)};
    if (X86Func* function = g_dispatch.Find(guest)) return {dispatch::TargetKind::kGeneratedFunction, function};
    if (guest >= g_dispatch.CodeBase() && guest < g_dispatch.CodeEnd())
        return {dispatch::TargetKind::kUnmapped, nullptr};
    return {dispatch::TargetKind::kUnknown, nullptr};
}

}  // namespace

namespace dispatch {

bool Init(const Metadata& metadata, std::string& error) {
    std::unique_lock<std::shared_mutex> lock(g_dispatch_lock);
    if (++g_dispatch_generation == 0) ++g_dispatch_generation;
    if (!g_dispatch.Initialize(metadata, error)) {
        g_dispatch.Reset();
        lock.unlock();
        LOGE("dispatch: %s", error.c_str());
        return false;
    }
    LOGI("dispatch: %zu functions over %08X..%08X", metadata.functions.size(), metadata.code_base, metadata.code_end);
    return true;
}

bool Init(std::string& error) {
    const Metadata metadata{X86CodeBase, X86CodeEnd, X86EntryPoint,
                            std::span<const X86FuncMapping>(X86FuncMappings, X86FuncMappingCount),
                            std::span<const uint32_t>(X86Continuations, X86ContinuationCount)};
    return Init(metadata, error);
}

void Reset() {
    std::unique_lock<std::shared_mutex> lock(g_dispatch_lock);
    g_dispatch.Reset();
    if (++g_dispatch_generation == 0) ++g_dispatch_generation;
}

X86Func* Find(uint32_t guest) {
    if (g_call_lease_depth) return g_dispatch.Find(guest);
    std::shared_lock<std::shared_mutex> lock(g_dispatch_lock);
    return g_dispatch.Find(guest);
}

bool IsContinuation(uint32_t guest) {
    if (g_call_lease_depth) return g_dispatch.IsContinuation(guest);
    std::shared_lock<std::shared_mutex> lock(g_dispatch_lock);
    return g_dispatch.IsContinuation(guest);
}

uint32_t HostThunk(X86Func* fn) {
    std::lock_guard<std::mutex> lock(g_host_lock);
    for (size_t i = 0; i < g_host_thunks.size(); i++)
        if (g_host_thunks[i] == fn) return kHostThunkBase + uint32_t(i) * 4;
    g_host_thunks.push_back(fn);
    return kHostThunkBase + uint32_t(g_host_thunks.size() - 1) * 4;
}

uint32_t KernelThunkAddress(uint32_t ordinal) { return ordinal < 400 ? kKernelThunkBase + ordinal * 4 : 0; }

#ifdef MOSTWANTED_CHECKED_KERNEL_ABI_TESTS
bool SetKernel255FixtureBridge(bool enabled) { return SetFixtureBridge(255, enabled); }
bool SetKernel187FixtureBridge(bool enabled) { return SetFixtureBridge(187, enabled); }
bool SetKernel277FixtureBridge(bool enabled) { return SetFixtureBridge(277, enabled); }
bool SetKernel294FixtureBridge(bool enabled) { return SetFixtureBridge(294, enabled); }
bool SetKernel258FixtureBridge(bool enabled) { return SetFixtureBridge(258, enabled); }
#endif

ResolvedTarget ResolveTarget(uint32_t guest) {
    if (g_call_lease_depth) return ResolveTargetLocked(guest);
    std::shared_lock<std::shared_mutex> lock(g_dispatch_lock);
    return ResolveTargetLocked(guest);
}

const char* TargetKindName(TargetKind kind) {
    switch (kind) {
        case TargetKind::kGeneratedFunction: return "generated function entry";
        case TargetKind::kContinuation: return "generated continuation";
        case TargetKind::kKernelThunk: return "kernel export thunk";
        case TargetKind::kHostThunk: return "registered host thunk";
        case TargetKind::kNull: return "null target";
        case TargetKind::kUninitialized: return "uninitialized dispatch table";
        case TargetKind::kUnmapped: return "unmapped target";
        case TargetKind::kUnknown: return "unknown target";
    }
    return "unknown target";
}

CallLease::CallLease() : lock_(g_dispatch_lock, std::defer_lock), outer_(g_call_lease_depth == 0) {
    if (outer_) lock_.lock();
    ++g_call_lease_depth;
}

CallLease::~CallLease() { --g_call_lease_depth; }

ResolvedTarget CallLease::Resolve(uint32_t guest) const { return ResolveTargetLocked(guest); }

uint64_t CallLease::Generation() const { return g_dispatch_generation; }
uint32_t CallLease::CodeBase() const { return g_dispatch.CodeBase(); }
uint32_t CallLease::CodeEnd() const { return g_dispatch.CodeEnd(); }

#ifdef MOSTWANTED_GUEST_CALLBACK_TESTS
bool ResetWouldBlockForTest() {
    std::unique_lock<std::shared_mutex> lock(g_dispatch_lock, std::try_to_lock);
    return !lock.owns_lock();
}
#endif

}  // namespace dispatch

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
#if defined(__APPLE__) && !defined(MOSTWANTED_GUEST_CALLBACK_TESTS)
    (void)base;
    x86_trap(ctx, target, "guest indirect execution is disabled in Phase 2C-12");
#else
    ctx.last_icall = target;
    dispatch::CallLease lease;
    const dispatch::ResolvedTarget resolved = lease.Resolve(target);
    if (resolved.kind == dispatch::TargetKind::kContinuation) throw X86Resume{target, ctx.esp};
    if (resolved.kind == dispatch::TargetKind::kGeneratedFunction || resolved.kind == dispatch::TargetKind::kHostThunk) {
        resolved.function(ctx, base);
        return;
    }
    if (resolved.kind == dispatch::TargetKind::kKernelThunk) {
        x86_kernel(ctx, base, (target - kKernelThunkBase) / 4);
        return;
    }
    uint32_t ret = kernel::R32(ctx.esp);
    LOGE("indirect call to %08X has no recompiled function (return address %08X, esp %08X)", target, ret, ctx.esp);
    LOGE("add 0x%X to [functions] in the image's config/*.toml and rebuild", target);
    x86_trap(ctx, ret, "indirect call to unknown code");
#endif
}

namespace {

void CheckedCall(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t return_address, uint32_t call_pc) {
    dispatch::CallLease lease;
    const dispatch::ResolvedTarget resolved = lease.Resolve(target);
#ifdef MOSTWANTED_CHECKED_KERNEL_ABI_TESTS
    if (resolved.kind == dispatch::TargetKind::kKernelThunk) {
        if (FixtureBridge* bridge = FindFixtureBridge(target)) {
            const uint32_t argument_bytes = bridge->arguments * sizeof(uint32_t);
            const uint32_t argument_address = ctx.esp;
            if (return_address < g_dispatch.CodeBase() || return_address >= g_dispatch.CodeEnd())
                x86_checked_trap(ctx, call_pc, "checked kernel CALL return address is outside generated code");
            if (argument_address > UINT32_MAX - argument_bytes)
                throw X86MemoryFault{call_pc, argument_address, mem::AccessStatus::kInvalidAddress, false};
            mem::ScopedLock memory_lock;
            const mem::AccessStatus args_status = mem::CheckAccess(argument_address, argument_bytes, mem::kRead);
            if (args_status != mem::AccessStatus::kOk)
                throw X86MemoryFault{call_pc, argument_address, args_status, false};
            const Export& registered = g_exports[bridge->ordinal];
            if (!registered.fn || !registered.name || std::strcmp(registered.name, bridge->name) != 0) {
                char reason[128];
                std::snprintf(reason, sizeof(reason), "checked kernel CALL ordinal %u is not registered as %s",
                              bridge->ordinal, bridge->name);
                x86_checked_trap(ctx, call_pc, reason);
            }

            const uint32_t stack_address = ctx.esp - 4;
            x86_mem_write<uint32_t>(call_pc, stack_address, return_address);
            const uint32_t stdcall_esp = ctx.esp + argument_bytes;
            X86CheckedCallFrame frame{ctx.checked_call, return_address, false};
            ctx.checked_call = &frame;
            struct RestoreFrame {
                X86Context& ctx;
                X86CheckedCallFrame& frame;
                ~RestoreFrame() {
                    if (ctx.checked_call == &frame) ctx.checked_call = frame.previous;
                }
            } restore{ctx, frame};

            ctx.esp = stack_address;
            ctx.last_icall = target;
            registered.fn(ctx, base);
            if (ctx.esp != stdcall_esp) {
                char reason[128];
                std::snprintf(reason, sizeof(reason),
                              "checked kernel CALL violated the %u-argument stdcall stack cleanup", bridge->arguments);
                x86_checked_trap(ctx, call_pc, reason);
            }
            const uint32_t actual_return = x86_mem_read<uint32_t>(call_pc, stack_address);
            if (actual_return != return_address)
                x86_checked_trap(ctx, call_pc, "checked kernel CALL return address was modified");
            frame.returned = true;
            return;
        }
    }
#endif
    if (resolved.kind != dispatch::TargetKind::kGeneratedFunction || !resolved.function) {
        char reason[128];
        std::snprintf(reason, sizeof(reason), "checked CALL target %08X rejected: %s", target,
                      dispatch::TargetKindName(resolved.kind));
        x86_checked_trap(ctx, call_pc, reason);
    }

    const uint32_t stack_address = ctx.esp - 4;
    x86_mem_write<uint32_t>(call_pc, stack_address, return_address);
    ctx.esp = stack_address;

    X86CheckedCallFrame frame{ctx.checked_call, return_address, false};
    ctx.checked_call = &frame;
    struct RestoreFrame {
        X86Context& ctx;
        X86CheckedCallFrame& frame;
        ~RestoreFrame() {
            if (ctx.checked_call == &frame) ctx.checked_call = frame.previous;
        }
    } restore{ctx, frame};

    ctx.last_icall = target;
    resolved.function(ctx, base);
    if (!frame.returned) x86_checked_trap(ctx, call_pc, "direct CALL target returned without checked RET");
}

}  // namespace

void x86_checked_call(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t return_address, uint32_t call_pc) {
    CheckedCall(ctx, base, target, return_address, call_pc);
}

void x86_checked_indirect_call(X86Context& ctx, uint8_t* base, uint32_t operand_address, uint32_t return_address,
                               uint32_t call_pc) {
    const uint32_t target = x86_mem_read<uint32_t>(call_pc, operand_address);
#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
    mem::ScopedPageLease callback_code_lease;
    if (kernel::SyntheticWorkerClaimHeld()) {
        dispatch::CallLease lease;
        if (lease.Resolve(target).kind == dispatch::TargetKind::kGeneratedFunction) {
            if (!kernel::SyntheticWorkerHasNestedCallback() ||
                !kernel::SyntheticWorkerAuthorizesNestedCallback(target, lease.Generation()))
                x86_checked_trap(ctx, call_pc, "unauthorized synthetic nested callback target");
            const mem::AccessStatus status = callback_code_lease.Acquire(target, 1, mem::kExecute);
            if (status != mem::AccessStatus::kOk) throw X86MemoryFault{call_pc, target, status, false};
        }
    }
#endif
    CheckedCall(ctx, base, target, return_address, call_pc);
}

void x86_checked_ret(X86Context& ctx, uint32_t pc, uint32_t pop_bytes) {
    if (X86CheckedCallFrame* frame = ctx.checked_call) {
        const uint32_t actual = x86_mem_read<uint32_t>(pc, ctx.esp);
        if (frame->returned || actual != frame->return_address) {
            char reason[112];
            std::snprintf(reason, sizeof(reason), "invalid guest return address: expected %08X, found %08X",
                          frame->return_address, actual);
            x86_checked_trap(ctx, pc, reason);
        }
        ctx.checked_call = frame->previous;
        frame->returned = true;
    }
    ctx.esp += 4 + pop_bytes;
}

void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r) {
    dispatch::CallLease lease;
    const dispatch::ResolvedTarget resolved = lease.Resolve(r.target);
    if (resolved.kind != dispatch::TargetKind::kContinuation) x86_trap(ctx, r.target, "stale or invalid continuation");
    if (!resolved.function) x86_trap(ctx, r.target, "continuation has no recompiled function");
    ctx.esp = r.esp;
    resolved.function(ctx, base);
}

void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
    if (ordinal < 400 && g_exports[ordinal].fn) {
        g_exports[ordinal].fn(ctx, base);
        return;
    }
    LOGE("kernel export %u (%s) is not implemented, return address %08X", ordinal, kernel::ExportName(ordinal),
         kernel::R32(ctx.esp));
    x86_trap(ctx, kernel::R32(ctx.esp), "unimplemented kernel export");
}

namespace kernel {

void RegisterExport(uint32_t ordinal, const char* name, KernelFn fn) {
    g_exports[ordinal].name = name;
    g_exports[ordinal].fn = fn;
}

static uint32_t g_kdata_next = mem::kKernelDataBase + 0x1000;

uint32_t RegisterData(uint32_t ordinal, const char* name, uint32_t size) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (g_exports[ordinal].data) return g_exports[ordinal].data;
    uint32_t a = (g_kdata_next + 15) & ~15u;
    g_kdata_next = a + ((size + 15) & ~15u);
    g_exports[ordinal].name = name;
    g_exports[ordinal].data = a;
    return a;
}

uint32_t DataAddress(uint32_t ordinal) { return ordinal < 400 ? g_exports[ordinal].data : 0; }
const char* ExportName(uint32_t ordinal) { return ordinal < 400 && g_exports[ordinal].name ? g_exports[ordinal].name : OrdinalName(ordinal); }
bool HasExport(uint32_t ordinal) { return ordinal < 400 && (g_exports[ordinal].fn || g_exports[ordinal].data); }

uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* args, size_t count) {
#if defined(__APPLE__) && !defined(MOSTWANTED_GUEST_CALLBACK_TESTS)
    (void)args;
    (void)count;
    x86_trap(ctx, fn, "CallGuest execution is disabled in Phase 2C-12");
#else
    // the D3D and DirectSound hooks call this several times a draw: no heap, only the registers put back are kept
    struct {
        uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    } saved{ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    uint32_t esp = ctx.esp;
    for (const uint32_t* p = args + count; p != args;) {
        ctx.esp -= 4;
        W32(ctx.esp, *--p);
    }
    ctx.esp -= 4;
    constexpr uint32_t host_return = 0xFFFFFFF0u;  // never used as a target, returns through the host stack
    W32(ctx.esp, host_return);
    X86CheckedCallFrame call_frame{ctx.checked_call, host_return, false};
    ctx.checked_call = &call_frame;
    struct RestoreCallFrame {
        X86Context& ctx;
        X86CheckedCallFrame& frame;
        ~RestoreCallFrame() {
            if (ctx.checked_call == &frame) ctx.checked_call = frame.previous;
        }
    } restore{ctx, call_frame};
    x86_icall(ctx, g_base, fn);
    uint32_t result = ctx.eax;
    ctx.eax = saved.eax;
    ctx.ecx = saved.ecx;
    ctx.edx = saved.edx;
    ctx.ebx = saved.ebx;
    ctx.esi = saved.esi;
    ctx.edi = saved.edi;
    ctx.ebp = saved.ebp;
    ctx.esp = esp;
    return result;
#endif
}

namespace {

constexpr size_t kMaximumCallbackArguments = 16;
constexpr uint32_t kHostCallbackReturn = 0xFFFFFFF0;
constexpr GuestCallbackArgumentContract kThreadStartArguments[] = {{GuestCallbackArgumentKind::kNullableUnresolvedPointer}};
constexpr GuestCallbackArgumentContract kSystemRoutineArguments[] = {
    {GuestCallbackArgumentKind::kGuestFunction}, {GuestCallbackArgumentKind::kNullableUnresolvedPointer}};
constexpr GuestCallbackArgumentContract kApcArguments[] = {
    {GuestCallbackArgumentKind::kOpaqueDword}, {GuestCallbackArgumentKind::kOpaqueDword},
    {GuestCallbackArgumentKind::kOpaqueDword}};
constexpr GuestCallbackArgumentContract kFileApcArguments[] = {
    {GuestCallbackArgumentKind::kNullableUnresolvedPointer}, {GuestCallbackArgumentKind::kBoundedRead, 8},
    {GuestCallbackArgumentKind::kScalar}};
constexpr GuestCallbackArgumentContract kDpcArguments[] = {
    {GuestCallbackArgumentKind::kBoundedReadWrite, 0x1C},
    {GuestCallbackArgumentKind::kNullableUnresolvedPointer}, {GuestCallbackArgumentKind::kOpaqueDword},
    {GuestCallbackArgumentKind::kOpaqueDword}};
constexpr GuestCallbackArgumentContract kTimerApcArguments[] = {
    {GuestCallbackArgumentKind::kNullableUnresolvedPointer}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}};
constexpr GuestCallbackArgumentContract kTimerDpcArguments[] = {
    {GuestCallbackArgumentKind::kBoundedReadWrite, 0x1C},
    {GuestCallbackArgumentKind::kNullableUnresolvedPointer}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}};
constexpr GuestCallbackArgumentContract kVBlankArguments[] = {{GuestCallbackArgumentKind::kBoundedRead, 12}};
constexpr GuestCallbackArgumentContract kSynchronizeArguments[] = {
    {GuestCallbackArgumentKind::kNullableUnresolvedPointer}};
constexpr GuestCallbackArgumentContract kFixtureArguments[] = {
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar},
    {GuestCallbackArgumentKind::kScalar}, {GuestCallbackArgumentKind::kScalar}};

GuestCallbackStatus ValidateCallbackArguments(const GuestCallbackTicket& ticket, std::span<const uint32_t> arguments,
                                              dispatch::CallLease& dispatch_lease,
                                              std::array<mem::ScopedPageLease, kMaximumCallbackArguments>& leases) {
#if !defined(MOSTWANTED_GUEST_CALLBACK_TESTS)
    if (ticket.callback_class == GuestCallbackClass::kFixture) return GuestCallbackStatus::kArgumentRejected;
#endif
    const GuestCallbackContract& contract = CallbackContract(ticket.callback_class);
    if (contract.callback_class != ticket.callback_class) return GuestCallbackStatus::kArgumentRejected;
    if ((!contract.variadic && arguments.size() != contract.arguments.size()) ||
        arguments.size() > contract.arguments.size())
        return GuestCallbackStatus::kArgumentRejected;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const uint32_t value = arguments[i];
        const GuestCallbackArgumentContract& argument = contract.arguments[i];
        switch (argument.kind) {
            case GuestCallbackArgumentKind::kScalar:
            case GuestCallbackArgumentKind::kOpaqueDword:
            case GuestCallbackArgumentKind::kNullableUnresolvedPointer:
                break;
            case GuestCallbackArgumentKind::kGuestFunction:
                if (dispatch_lease.Resolve(value).kind != dispatch::TargetKind::kGeneratedFunction)
                    return GuestCallbackStatus::kArgumentRejected;
                break;
            case GuestCallbackArgumentKind::kBoundedRead:
                if (!value || leases[i].Acquire(value, argument.size, mem::kRead) != mem::AccessStatus::kOk)
                    return GuestCallbackStatus::kArgumentRejected;
                break;
            case GuestCallbackArgumentKind::kBoundedReadWrite:
                if (!value || leases[i].Acquire(value, argument.size, mem::kReadWrite) != mem::AccessStatus::kOk)
                    return GuestCallbackStatus::kArgumentRejected;
                break;
        }
    }
    return GuestCallbackStatus::kReady;
}

GuestCallbackStatus ValidateMappedCallback(uint32_t target, dispatch::CallLease& lease, X86Func*& function) {
    const dispatch::ResolvedTarget resolved = lease.Resolve(target);
    if (resolved.kind != dispatch::TargetKind::kGeneratedFunction || !resolved.function)
        return GuestCallbackStatus::kRejectedTarget;
    function = resolved.function;
    return GuestCallbackStatus::kReady;
}

GuestCallbackStatus ValidateCallbackTicket(const GuestCallbackTicket& ticket, uint32_t target,
                                           dispatch::CallLease& lease, X86Func*& function,
                                           mem::ScopedPageLease& code_lease) {
    if (!ticket.target) return GuestCallbackStatus::kNullTarget;
    if (target != ticket.target) return GuestCallbackStatus::kTargetChanged;
    if (!ticket.dispatch_generation || ticket.dispatch_generation != lease.Generation())
        return GuestCallbackStatus::kStaleDispatch;
    const GuestCallbackStatus mapped = ValidateMappedCallback(target, lease, function);
    if (mapped != GuestCallbackStatus::kReady) return mapped;
    const uint32_t code_base = lease.CodeBase();
    const uint32_t code_end = lease.CodeEnd();
    return code_end > code_base &&
                   code_lease.Acquire(code_base, code_end - code_base, mem::kExecute) == mem::AccessStatus::kOk
               ? GuestCallbackStatus::kReady
               : GuestCallbackStatus::kCodePageDenied;
}

GuestCallbackStatus ValidateCallbackStack(X86Context& ctx, uint32_t stack_base, uint32_t stack_size,
                                          std::span<const uint32_t> arguments, uint32_t& stack_pointer,
                                          mem::ScopedPageLease& stack_lease) {
    if (arguments.size() > kMaximumCallbackArguments) return GuestCallbackStatus::kTooManyArguments;
    const uint64_t stack_end = uint64_t(stack_base) + stack_size;
    const uint64_t frame_size = (arguments.size() + 1) * sizeof(uint32_t);
    if (!stack_size || stack_end > (uint64_t{1} << 32) || ctx.esp > stack_end ||
        uint64_t(ctx.esp) < uint64_t(stack_base) + frame_size)
        return GuestCallbackStatus::kInvalidStack;
    stack_pointer = ctx.esp - uint32_t(frame_size);
    return stack_lease.Acquire(stack_base, stack_size, mem::kWrite) == mem::AccessStatus::kOk
               ? GuestCallbackStatus::kReady
               : GuestCallbackStatus::kStackWriteDenied;
}

}  // namespace

const GuestCallbackContract& CallbackContract(GuestCallbackClass callback_class) {
    static const GuestCallbackContract contracts[] = {
        {GuestCallbackClass::kThreadStart, kThreadStartArguments},
        {GuestCallbackClass::kSystemRoutine, kSystemRoutineArguments},
        {GuestCallbackClass::kApc, kApcArguments},
        {GuestCallbackClass::kFileApc, kFileApcArguments},
        {GuestCallbackClass::kDpc, kDpcArguments},
        {GuestCallbackClass::kTimerApc, kTimerApcArguments},
        {GuestCallbackClass::kTimerDpc, kTimerDpcArguments},
        {GuestCallbackClass::kVBlank, kVBlankArguments},
        {GuestCallbackClass::kSynchronizeExecution, kSynchronizeArguments},
        {GuestCallbackClass::kFixture, kFixtureArguments, true},
    };
    const size_t index = static_cast<size_t>(callback_class);
    if (index < sizeof(contracts) / sizeof(contracts[0])) return contracts[index];
    static const GuestCallbackContract invalid{static_cast<GuestCallbackClass>(0xFF), {}};
    return invalid;
}

GuestCallbackStatus PrepareGuestCallback(uint32_t target, GuestCallbackClass callback_class,
                                         GuestCallbackTicket& ticket) {
    ticket = {};
    if (!target) return GuestCallbackStatus::kNullTarget;
#if !defined(MOSTWANTED_GUEST_CALLBACK_TESTS)
    if (callback_class == GuestCallbackClass::kFixture) return GuestCallbackStatus::kArgumentRejected;
#endif
    if (CallbackContract(callback_class).callback_class != callback_class)
        return GuestCallbackStatus::kArgumentRejected;
#if defined(__APPLE__) || defined(MOSTWANTED_GUEST_CALLBACK_TESTS)
    if (!KernelWorkersAccepting()) return GuestCallbackStatus::kCancelled;
    dispatch::CallLease lease;
    X86Func* function = nullptr;
    const GuestCallbackStatus mapped = ValidateMappedCallback(target, lease, function);
    if (mapped != GuestCallbackStatus::kReady) return mapped;
    mem::ScopedPageLease code_lease;
    if (code_lease.Acquire(target, 1, mem::kExecute) != mem::AccessStatus::kOk)
        return GuestCallbackStatus::kCodePageDenied;
    if (!KernelWorkersAccepting()) return GuestCallbackStatus::kCancelled;
    ticket = {target, lease.Generation(), callback_class};
    return GuestCallbackStatus::kReady;
#else
    ticket = {target, 0, callback_class};
    return GuestCallbackStatus::kReady;
#endif
}

GuestCallbackStatus CallGuestCallback(X86Context& ctx, const GuestCallbackTicket& ticket, uint32_t current_target,
                                      std::span<const uint32_t> arguments, uint32_t stack_base, uint32_t stack_size,
                                      uint32_t& result) {
    result = 0;
    if (!ticket.target) return GuestCallbackStatus::kNullTarget;
    if (current_target != ticket.target) return GuestCallbackStatus::kTargetChanged;
    if (arguments.size() > kMaximumCallbackArguments) return GuestCallbackStatus::kTooManyArguments;
#if defined(__APPLE__)
    if (!KernelWorkersAccepting()) return GuestCallbackStatus::kCancelled;
    GuestDispatchLease dispatch_gate;
    if (!dispatch_gate.Allowed()) return GuestCallbackStatus::kCancelled;
    dispatch::CallLease dispatch_lease;
    X86Func* function = nullptr;
    mem::ScopedPageLease code_lease;
    const GuestCallbackStatus target_status =
        ValidateCallbackTicket(ticket, current_target, dispatch_lease, function, code_lease);
    if (target_status != GuestCallbackStatus::kReady) return target_status;
    std::array<mem::ScopedPageLease, kMaximumCallbackArguments> argument_leases;
    const GuestCallbackStatus argument_status =
        ValidateCallbackArguments(ticket, arguments, dispatch_lease, argument_leases);
    if (argument_status != GuestCallbackStatus::kReady) return argument_status;
    (void)function;
    uint32_t stack_pointer = 0;
    mem::ScopedPageLease stack_lease;
    const GuestCallbackStatus stack_status =
        ValidateCallbackStack(ctx, stack_base, stack_size, arguments, stack_pointer, stack_lease);
    if (stack_status != GuestCallbackStatus::kReady) return stack_status;
    return GuestCallbackStatus::kExecutionDisabled;
#else
    if (!KernelWorkersAccepting()) return GuestCallbackStatus::kCancelled;
    GuestDispatchLease dispatch_gate;
    if (!dispatch_gate.Allowed()) return GuestCallbackStatus::kCancelled;
    const uint32_t no_arguments = 0;
    result = CallGuest(ctx, current_target, arguments.empty() ? &no_arguments : arguments.data(), arguments.size());
    return GuestCallbackStatus::kInvoked;
#endif
}

#ifdef MOSTWANTED_GUEST_CALLBACK_TESTS
GuestCallbackStatus CallGuestCallbackForTest(X86Context& ctx, const GuestCallbackTicket& ticket,
                                             uint32_t current_target, std::span<const uint32_t> arguments,
                                             uint32_t stack_base, uint32_t stack_size, uint32_t& result) {
    result = 0;
    if (arguments.size() > kMaximumCallbackArguments) return GuestCallbackStatus::kTooManyArguments;
    if (!KernelWorkersAccepting()) return GuestCallbackStatus::kCancelled;
    GuestDispatchLease dispatch_gate;
    if (!dispatch_gate.Allowed()) return GuestCallbackStatus::kCancelled;
    dispatch::CallLease dispatch_lease;
    X86Func* function = nullptr;
    mem::ScopedPageLease code_lease;
    const GuestCallbackStatus target_status =
        ValidateCallbackTicket(ticket, current_target, dispatch_lease, function, code_lease);
    if (target_status != GuestCallbackStatus::kReady) return target_status;
    std::array<mem::ScopedPageLease, kMaximumCallbackArguments> argument_leases;
    const GuestCallbackStatus argument_status =
        ValidateCallbackArguments(ticket, arguments, dispatch_lease, argument_leases);
    if (argument_status != GuestCallbackStatus::kReady) return argument_status;
    uint32_t stack_pointer = 0;
    mem::ScopedPageLease stack_lease;
    const GuestCallbackStatus stack_status =
        ValidateCallbackStack(ctx, stack_base, stack_size, arguments, stack_pointer, stack_lease);
    if (stack_status != GuestCallbackStatus::kReady) return stack_status;

    std::array<uint32_t, kMaximumCallbackArguments + 1> frame{};
    frame[0] = kHostCallbackReturn;
    std::copy(arguments.begin(), arguments.end(), frame.begin() + 1);
    if (mem::Write(stack_pointer, frame.data(), (arguments.size() + 1) * sizeof(uint32_t)) != mem::AccessStatus::kOk)
        return GuestCallbackStatus::kStackWriteDenied;
    struct RestoreContext {
        X86Context& ctx;
        X86Context saved;
        ~RestoreContext() { ctx = saved; }
    } restore{ctx, ctx};
    ctx.esp = stack_pointer;
    ctx.last_icall = current_target;
    function(ctx, mem::Base());
    result = ctx.eax;
    return GuestCallbackStatus::kInvoked;
}
#endif

}  // namespace kernel

namespace kernel {
// return addresses found on the guest stack, the cheapest way to place a guest event
void LogGuestStack(X86Context& ctx, int words) {
    LOGE("  eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", ctx.eax, ctx.ecx, ctx.edx, ctx.ebx,
         ctx.esp, ctx.ebp, ctx.esi, ctx.edi);
    for (int i = 0; i < words; i++) {
        uint32_t a = ctx.esp + uint32_t(i) * 4;
        if (!mem::IsCommitted(a)) break;
        uint32_t v = R32(a);
        if (v >= X86CodeBase && v < X86CodeEnd) LOGE("  [esp+%03X] %08X", i * 4, v);
    }
}
}  // namespace kernel

void x86_trap(X86Context& ctx, uint32_t va, const char* what) {
    LOGE("guest trap at %08X: %s", va, what);
    kernel::LogGuestStack(ctx, 64);
    logx::Flush();
#ifdef _WIN32
    __debugbreak();
#endif
    std::abort();
}

uint32_t x86_trace(X86Context& ctx, uint32_t va, uint32_t ret) {
    using kernel::R32;
    if (!ret) {
        ret = R32(ctx.esp);
        LOGI("trace %08X from %08X (%08X, %08X, %08X, %08X) ecx %08X", va, ret, R32(ctx.esp + 4), R32(ctx.esp + 8),
             R32(ctx.esp + 12), R32(ctx.esp + 16), ctx.ecx);
    } else {
        LOGI("trace %08X returned %08X to %08X", va, ctx.eax, ret);
    }
    return ret;
}

void x86_priv(X86Context& ctx, uint32_t va, const char* what) {
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(va).second) LOGW("privileged instruction %s at %08X ignored", what, va);
    (void)ctx;
}

// the guest measured time against a 733.33 MHz time stamp counter
uint64_t x86_rdtsc() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    auto ns = duration_cast<nanoseconds>(steady_clock::now() - start).count();
    uint64_t sec = uint64_t(ns) / 1000000000ull, rem = uint64_t(ns) % 1000000000ull;
    return sec * 733333333ull + rem * 733333333ull / 1000000000ull;
}

void x86_cpuid(X86Context& ctx) {
    switch (ctx.eax) {
        case 0:
            ctx.eax = 2;
            ctx.ebx = 0x756E6547;  // GenuineIntel
            ctx.edx = 0x49656E69;
            ctx.ecx = 0x6C65746E;
            break;
        case 1:
            ctx.eax = 0x0000068A;  // family 6 model 8, Pentium III Coppermine
            ctx.ebx = 0;
            ctx.ecx = 0;
            ctx.edx = 0x0383F9FF;  // fpu tsc cx8 cmov mmx fxsr sse
            break;
        default:
            ctx.eax = ctx.ebx = ctx.ecx = ctx.edx = 0;
            break;
    }
}
