#include <cstdlib>

#include "core/log.h"
#include "cpu/x86_ops.h"

void x86_memory_fault(X86Context& ctx, const X86MemoryFault& fault) {
    LOGE("checked guest %s fault at %08X for %08X: %s", fault.write ? "write" : "read", fault.instruction,
         fault.address, mem::StatusName(fault.status));
    logx::Flush();
    (void)ctx;
    std::abort();
}

void x86_checked_trap(X86Context& ctx, uint32_t va, const char* what) {
    LOGE("checked recompiler blocked instruction at %08X: %s", va, what);
    logx::Flush();
    (void)ctx;
    std::abort();
}
