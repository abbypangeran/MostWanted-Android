#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace {

dispatch::Table g_dispatch;

struct Export {
    const char* name = nullptr;
    kernel::KernelFn fn = nullptr;
    uint32_t data = 0;
};
Export g_exports[400];

}  // namespace

// synthetic guest addresses for kernel functions reached through a pointer
constexpr uint32_t kKernelThunkBase = 0xFE000000;

namespace dispatch {

bool Init(std::string& error) {
    const Metadata metadata{X86CodeBase, X86CodeEnd, X86EntryPoint,
                            std::span<const X86FuncMapping>(X86FuncMappings, X86FuncMappingCount),
                            std::span<const uint32_t>(X86Continuations, X86ContinuationCount)};
    if (!g_dispatch.Initialize(metadata, error)) {
        LOGE("dispatch: %s", error.c_str());
        g_dispatch.Reset();
        return false;
    }
    LOGI("dispatch: %zu functions over %08X..%08X", metadata.functions.size(), X86CodeBase, X86CodeEnd);
    return true;
}

void Reset() { g_dispatch.Reset(); }

X86Func* Find(uint32_t guest) {
    return g_dispatch.Find(guest);
}

// host code the guest may call through a pointer gets an address in this range
constexpr uint32_t kHostThunkBase = 0xFD000000;
static std::vector<X86Func*> g_host_thunks;
static std::mutex g_host_lock;

uint32_t HostThunk(X86Func* fn) {
    std::lock_guard<std::mutex> lock(g_host_lock);
    for (size_t i = 0; i < g_host_thunks.size(); i++)
        if (g_host_thunks[i] == fn) return kHostThunkBase + uint32_t(i) * 4;
    g_host_thunks.push_back(fn);
    return kHostThunkBase + uint32_t(g_host_thunks.size() - 1) * 4;
}

uint32_t KernelThunkAddress(uint32_t ordinal) { return ordinal < 400 ? kKernelThunkBase + ordinal * 4 : 0; }

}  // namespace dispatch

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
    ctx.last_icall = target;
    if (g_dispatch.IsContinuation(target)) throw X86Resume{target, ctx.esp};
    if (X86Func* f = dispatch::Find(target)) {
        f(ctx, base);
        return;
    }
    if (target >= kKernelThunkBase && target < kKernelThunkBase + 400 * 4) {
        x86_kernel(ctx, base, (target - kKernelThunkBase) / 4);
        return;
    }
    if (target >= dispatch::kHostThunkBase) {
        size_t i = (target - dispatch::kHostThunkBase) / 4;
        X86Func* f = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch::g_host_lock);
            if (i < dispatch::g_host_thunks.size()) f = dispatch::g_host_thunks[i];
        }
        if (f) {
            f(ctx, base);
            return;
        }
    }
    uint32_t ret = kernel::R32(ctx.esp);
    LOGE("indirect call to %08X has no recompiled function (return address %08X, esp %08X)", target, ret, ctx.esp);
    LOGE("add 0x%X to [functions] in the image's config/*.toml and rebuild", target);
    x86_trap(ctx, ret, "indirect call to unknown code");
}

void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r) {
    ctx.esp = r.esp;
    X86Func* function = dispatch::Find(r.target);
    if (!function) x86_trap(ctx, r.target, "continuation has no recompiled function");
    function(ctx, base);
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
    W32(ctx.esp, 0xFFFFFFF0u);  // never used as a target, calls return through the host stack
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
