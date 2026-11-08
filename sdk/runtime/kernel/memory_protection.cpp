#include "kernel/exports.h"
#include "cpu/x86_ops.h"

namespace kernel {
namespace {

// Xbox protection codes and argument contracts are unverified; stop before pretending the operation succeeded.
[[noreturn]] void TrapUnverifiedProtection(X86Context& ctx, const char* reason) {
    const uint32_t return_address = x86_mem_read<uint32_t>(ctx.last_icall, ctx.esp);
    x86_checked_trap(ctx, return_address, reason);
}

KERNEL_EXPORT(179, MmQueryAddressProtect, 1) {
    TrapUnverifiedProtection(ctx, "MmQueryAddressProtect semantics are unverified");
}
KERNEL_EXPORT(182, MmSetAddressProtect, 3) {
    TrapUnverifiedProtection(ctx, "MmSetAddressProtect semantics are unverified");
}
KERNEL_EXPORT(204, NtProtectVirtualMemory, 4) {
    TrapUnverifiedProtection(ctx, "NtProtectVirtualMemory semantics are unverified");
}

}  // namespace
}  // namespace kernel
