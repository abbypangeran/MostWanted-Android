#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

dispatch::Table g_dispatch;
std::shared_mutex g_dispatch_lock;
thread_local size_t g_call_lease_depth = 0;
constexpr uint32_t kHostThunkBase = 0xFD000000;
std::vector<X86Func*> g_host_thunks;
std::mutex g_host_lock;

struct Export {
    const char* name = nullptr;
    kernel::KernelFn fn = nullptr;
    uint32_t data = 0;
};
Export g_exports[400];

}  // namespace

// synthetic guest addresses for kernel functions reached through a pointer
constexpr uint32_t kKernelThunkBase = 0xFE000000;

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

}  // namespace dispatch

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
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
}

namespace {

void CheckedCall(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t return_address, uint32_t call_pc) {
    dispatch::CallLease lease;
    const dispatch::ResolvedTarget resolved = lease.Resolve(target);
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
}

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
