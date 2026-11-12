#include "kernel/kernel_objects.h"

#include <array>

namespace kernel {
namespace {

constexpr size_t kDpcSize = 0x1C;
constexpr size_t kTimerSize = 0x28;

void Put32(std::array<uint8_t, kDpcSize>& object, size_t offset, uint32_t value) {
    object[offset + 0] = uint8_t(value);
    object[offset + 1] = uint8_t(value >> 8);
    object[offset + 2] = uint8_t(value >> 16);
    object[offset + 3] = uint8_t(value >> 24);
}

void Put32(std::array<uint8_t, kTimerSize>& object, size_t offset, uint32_t value) {
    object[offset + 0] = uint8_t(value);
    object[offset + 1] = uint8_t(value >> 8);
    object[offset + 2] = uint8_t(value >> 16);
    object[offset + 3] = uint8_t(value >> 24);
}

KernelObjectStatus MapStatus(mem::AccessStatus status) {
    switch (status) {
        case mem::AccessStatus::kOk: return KernelObjectStatus::kOk;
        case mem::AccessStatus::kNotInitialized: return KernelObjectStatus::kNotInitialized;
        case mem::AccessStatus::kInvalidAddress: return KernelObjectStatus::kInvalidAddress;
        case mem::AccessStatus::kUncommitted: return KernelObjectStatus::kUncommitted;
        case mem::AccessStatus::kPermissionDenied: return KernelObjectStatus::kPermissionDenied;
        case mem::AccessStatus::kBusy: return KernelObjectStatus::kBusy;
        case mem::AccessStatus::kHostError: return KernelObjectStatus::kHostError;
    }
    return KernelObjectStatus::kHostError;
}

}  // namespace

const char* KernelObjectStatusName(KernelObjectStatus status) {
    switch (status) {
        case KernelObjectStatus::kOk: return "ok";
        case KernelObjectStatus::kNotInitialized: return "not-initialized";
        case KernelObjectStatus::kInvalidAddress: return "invalid-address";
        case KernelObjectStatus::kUncommitted: return "uncommitted";
        case KernelObjectStatus::kPermissionDenied: return "permission-denied";
        case KernelObjectStatus::kBusy: return "busy";
        case KernelObjectStatus::kInvalidType: return "invalid-type";
        case KernelObjectStatus::kHostError: return "host-error";
    }
    return "unknown";
}

KernelObjectStatus InitializeDpcChecked(uint32_t dpc, uint32_t routine, uint32_t context) {
    if (!mem::IsInitialized()) return KernelObjectStatus::kNotInitialized;
    mem::ScopedPageLease lease;
    const KernelObjectStatus acquired = MapStatus(lease.Acquire(dpc, kDpcSize, mem::kRead | mem::kWrite));
    if (acquired != KernelObjectStatus::kOk) return acquired;
    std::array<uint8_t, kDpcSize> object{};
    object[0] = 19;  // KDPC Type
    // Inserted and Padding stay zero; DpcListEntry stays empty.
    Put32(object, 0x0C, routine);
    Put32(object, 0x10, context);
    // SystemArgument1 at +0x14 and SystemArgument2 at +0x18 stay zero.
    return MapStatus(mem::Write(dpc, object.data(), object.size()));
}

KernelObjectStatus InitializeTimerExChecked(uint32_t timer, uint32_t type) {
    if (!mem::IsInitialized()) return KernelObjectStatus::kNotInitialized;
    if (type > 1) return KernelObjectStatus::kInvalidType;
    mem::ScopedPageLease lease;
    const KernelObjectStatus acquired = MapStatus(lease.Acquire(timer, kTimerSize, mem::kRead | mem::kWrite));
    if (acquired != KernelObjectStatus::kOk) return acquired;
    std::array<uint8_t, kTimerSize> object{};
    object[0] = uint8_t(8 + type);  // TimerNotificationObject / TimerSynchronizationObject
    object[2] = 10;                 // DISPATCHER_HEADER.Size in dwords
    const uint32_t wait_list = timer + 8;
    Put32(object, 8, wait_list);
    Put32(object, 12, wait_list);
    // SignalState at +4, DueTime, TimerListEntry, Dpc and Period stay zero.
    return MapStatus(mem::Write(timer, object.data(), object.size()));
}

}  // namespace kernel
