#include "kernel/nv_settings.h"

#include "kernel/exports.h"

namespace kernel::nv {
namespace {

#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
uint32_t g_misc_flags = 0;
uint32_t g_query_fault = 0;
#endif

uint32_t MiscFlags() {
#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
    return g_misc_flags;
#else
    return 0;  // EEPROM user default: auto power-down disabled
#endif
}

uint32_t QueryMisc(const Args& a) {
    const uint32_t type_out = a[1], value = a[2], len = a[3], result_len = a[4];
    if (!type_out || !value) return kAccessViolation;
    const uint32_t size = sizeof(uint32_t);
    if (mem::CheckAccess(type_out, size, mem::kWrite) != mem::AccessStatus::kOk ||
        mem::CheckAccess(value, len, mem::kWrite) != mem::AccessStatus::kOk ||
        (result_len && mem::CheckAccess(result_len, size, mem::kWrite) != mem::AccessStatus::kOk))
        return kAccessViolation;
    if (result_len) {
        const uint32_t result = size;
        if (mem::Write(result_len, &result, size) != mem::AccessStatus::kOk) return kAccessViolation;
    }
    if (len < size) return kBufferTooSmall;
    const uint32_t flags = MiscFlags();
    if (mem::Fill(value, 0, len) != mem::AccessStatus::kOk ||
        mem::Write(value, &flags, size) != mem::AccessStatus::kOk)
        return kAccessViolation;
    const uint32_t type = 4;  // REG_DWORD
    return mem::Write(type_out, &type, size) == mem::AccessStatus::kOk ? kSuccess : kAccessViolation;
}

}  // namespace

uint32_t QueryReviewedNonVolatileSetting(const Args& a) {
    if (a[0] != 0x11) return kObjectNameNotFound;
#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
    if (g_query_fault) return g_query_fault;
#endif
    return QueryMisc(a);
}

uint32_t NtStatusToDosError(uint32_t status) {
    switch (status) {
        case kSuccess: return 0;
        case kPending: return 997;
        case kTimeout: return 1460;
        case kNoSuchFile:
        case kObjectNameNotFound: return 2;
        case kObjectPathNotFound: return 3;
        case kAccessDenied:
        case kCannotDelete:
        case kFileIsADirectory: return 5;
        case kInvalidHandle: return 6;
        case kNoMemory: return 8;
        case kNoMoreFiles: return 18;
        case kEndOfFile: return 38;
        case kNotSupported: return 50;
        case kInvalidParameter: return 87;
        case kNotImplemented: return 120;
        case kBufferTooSmall: return 122;
        case kDeviceDataError: return 23;  // ERROR_CRC
        case kObjectNameInvalid: return 123;
        case kObjectNameCollision: return 183;
        case kNotADirectory: return 267;
        case 0xC000007F: return 112;  // disk full
        case kDeviceNotConnected: return 1167;
        default: return (status & 0xC0000000) ? 317 : 0;
    }
}

void QueryReviewedNonVolatileSettingEntry(X86Context& ctx, uint8_t*) {
    Args args{ctx};
    Return(ctx, QueryReviewedNonVolatileSetting(args), 5);
}

void NtStatusToDosErrorEntry(X86Context& ctx, uint8_t*) {
    Args args{ctx};
    Return(ctx, NtStatusToDosError(args[0]), 1);
}

#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
void SetMiscFlagsForTest(uint32_t value) { g_misc_flags = value; }
void SetQueryFaultForTest(uint32_t status) { g_query_fault = status; }
#endif

}  // namespace kernel::nv

#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
namespace kernel {
void SetNonVolatileMiscFlagsForTest(uint32_t value) { nv::SetMiscFlagsForTest(value); }
void SetNonVolatileQueryFaultForTest(uint32_t status) { nv::SetQueryFaultForTest(status); }
}  // namespace kernel
#endif

KERNEL_EXPORT(301, RtlNtStatusToDosError, 1) { return ::kernel::nv::NtStatusToDosError(a[0]); }
