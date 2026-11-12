#pragma once

// Reviewed non-volatile settings and NTSTATUS conversion used by the authentic
// startup continuation and by the production kernel exports.

#include "kernel/kernel.h"

namespace kernel::nv {

// XC_MISC (0x0011) only. The reviewed title queries this REG_DWORD and masks
// its low bit (auto power-down). Unsupported indexes are reported as
// STATUS_OBJECT_NAME_NOT_FOUND.
uint32_t QueryReviewedNonVolatileSetting(const Args& a);
// The reviewed NTSTATUS-to-DOS-error table; unknown failures stay failures.
uint32_t NtStatusToDosError(uint32_t status);

// Harness entry points for the experimental target, which does not link the
// full xboxkrnl translation unit.
void QueryReviewedNonVolatileSettingEntry(X86Context& ctx, uint8_t* base);
void NtStatusToDosErrorEntry(X86Context& ctx, uint8_t* base);

#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
void SetMiscFlagsForTest(uint32_t value);
void SetQueryFaultForTest(uint32_t status);
#endif

}  // namespace kernel::nv
