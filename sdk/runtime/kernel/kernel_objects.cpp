#include "kernel/kernel_objects.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

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

std::mutex g_shutdown_lock;
std::vector<ShutdownRegistrationRecord> g_shutdown_registrations;

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
        case KernelObjectStatus::kInvalidCallback: return "invalid-callback";
        case KernelObjectStatus::kUnsupportedAction: return "unsupported-action";
        case KernelObjectStatus::kDuplicateRegistration: return "duplicate-registration";
        case KernelObjectStatus::kStaleOwner: return "stale-owner";
        case KernelObjectStatus::kInternalFailure: return "internal-failure";
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

KernelObjectStatus RegisterShutdownNotificationChecked(uint32_t object, uint32_t action, uint32_t owner_thread,
                                                       const char* image_sha1, uint32_t callback_low,
                                                       uint32_t callback_high) {
    if (!mem::IsInitialized()) return KernelObjectStatus::kNotInitialized;
    if (action != 1) return KernelObjectStatus::kUnsupportedAction;
    if (!image_sha1 || !*image_sha1) return KernelObjectStatus::kStaleOwner;
    mem::ScopedPageLease lease;
    const KernelObjectStatus acquired = MapStatus(lease.Acquire(object, 0x10, mem::kRead));
    if (acquired != KernelObjectStatus::kOk) return acquired;
    std::array<uint8_t, 0x10> bytes{};
    if (mem::Read(object, bytes.data(), bytes.size()) != mem::AccessStatus::kOk)
        return KernelObjectStatus::kInvalidAddress;
    const uint32_t routine = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 |
                             uint32_t(bytes[3]) << 24;
    const uint32_t priority = uint32_t(bytes[4]) | uint32_t(bytes[5]) << 8 | uint32_t(bytes[6]) << 16 |
                              uint32_t(bytes[7]) << 24;
    if (routine < callback_low || routine >= callback_high) return KernelObjectStatus::kInvalidCallback;
    std::lock_guard lock(g_shutdown_lock);
    for (const ShutdownRegistrationRecord& existing : g_shutdown_registrations)
        if (existing.object == object) return KernelObjectStatus::kDuplicateRegistration;
    ShutdownRegistrationRecord record;
    record.object = object;
    record.notification_routine = routine;
    record.priority = priority;
    record.owner_thread = owner_thread;
    record.memory_generation = mem::Generation();
    std::snprintf(record.image_sha1, sizeof(record.image_sha1), "%s", image_sha1);
    try {
        g_shutdown_registrations.push_back(record);
    } catch (...) {
        return KernelObjectStatus::kInternalFailure;
    }
    return KernelObjectStatus::kOk;
}

size_t ShutdownRegistrationCountForTest() {
    std::lock_guard lock(g_shutdown_lock);
    return g_shutdown_registrations.size();
}

bool ShutdownRegistrationForTest(size_t index, ShutdownRegistrationRecord* record) {
    std::lock_guard lock(g_shutdown_lock);
    if (index >= g_shutdown_registrations.size() || !record) return false;
    *record = g_shutdown_registrations[index];
    return true;
}

void ClearShutdownRegistrations() {
    std::lock_guard lock(g_shutdown_lock);
    g_shutdown_registrations.clear();
}

void ClearShutdownRegistrationsForTest() { ClearShutdownRegistrations(); }

}  // namespace kernel
