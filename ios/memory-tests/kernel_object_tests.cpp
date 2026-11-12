#include <array>
#include <cstdio>
#include <cstring>

#include "kernel/kernel_objects.h"
#include "kernel/memory.h"

namespace {

int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

std::array<uint8_t, 0x1C> ReadDpc(uint32_t address) {
    std::array<uint8_t, 0x1C> bytes{};
    mem::Read(address, bytes.data(), bytes.size());
    return bytes;
}

uint32_t Read32(const std::array<uint8_t, 0x1C>& bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 |
           uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}

std::array<uint8_t, 0x28> ReadTimer(uint32_t address) {
    std::array<uint8_t, 0x28> bytes{};
    mem::Read(address, bytes.data(), bytes.size());
    return bytes;
}

uint32_t Read32(const std::array<uint8_t, 0x28>& bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 |
           uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}

}  // namespace

extern "C" int RunKernelObjectTests() {
    if (!mem::Init()) {
        std::printf("FAIL: kernel-object tests initialize guest memory\n");
        return 1;
    }
    const uint32_t page = mem::kUserLow + 0x8000;
    const uint32_t dpc = page + 0x100;
    const uint32_t timer = page + 0x200;
    Check(mem::Commit(page, mem::kPage), "kernel-object test page commits");
    Check(mem::Write(dpc, std::array<uint8_t, 0x1C>{}.data(), 0x1C) == mem::AccessStatus::kOk,
          "DPC test object is writable");

    Check(kernel::InitializeDpcChecked(dpc, 0x002B5E00, 0) == kernel::KernelObjectStatus::kOk,
          "KeInitializeDpc checked initialisation succeeds");
    const std::array<uint8_t, 0x1C> dpc_state = ReadDpc(dpc);
    Check(dpc_state[0] == 19 && dpc_state[1] == 0 && dpc_state[2] == 0 && dpc_state[3] == 0,
          "KDPC type is 19 with zero Inserted and Padding");
    Check(Read32(dpc_state, 4) == 0 && Read32(dpc_state, 8) == 0, "KDPC list entry is empty");
    Check(Read32(dpc_state, 0x0C) == 0x002B5E00, "KDPC preserves the deferred routine");
    Check(Read32(dpc_state, 0x10) == 0, "KDPC preserves the deferred context");
    Check(Read32(dpc_state, 0x14) == 0 && Read32(dpc_state, 0x18) == 0,
          "KDPC system arguments are zero");

    Check(mem::Write(timer, std::array<uint8_t, 0x28>{}.data(), 0x28) == mem::AccessStatus::kOk,
          "timer test object is writable");
    Check(kernel::InitializeTimerExChecked(timer, 0) == kernel::KernelObjectStatus::kOk,
          "KeInitializeTimerEx notification timer initialises");
    const std::array<uint8_t, 0x28> timer_state = ReadTimer(timer);
    Check(timer_state[0] == 8 && timer_state[1] == 0 && timer_state[2] == 10 && timer_state[3] == 0,
          "KTIMER dispatcher type 8, absolute 0, size 10, inserted 0");
    Check(Read32(timer_state, 4) == 0, "KTIMER initial signal state is nonsignalled");
    Check(Read32(timer_state, 8) == timer + 8 && Read32(timer_state, 12) == timer + 8,
          "KTIMER wait list is empty and self-linked");
    bool timer_tail_zero = true;
    for (size_t i = 0x10; i < 0x28; ++i)
        if (timer_state[i] != 0) timer_tail_zero = false;
    Check(timer_tail_zero, "KTIMER due time, list entry, DPC and period are zero");

    Check(kernel::InitializeTimerExChecked(timer, 1) == kernel::KernelObjectStatus::kOk &&
              ReadTimer(timer)[0] == 9,
          "KeInitializeTimerEx synchronization type 1 is supported");
    Check(kernel::InitializeTimerExChecked(timer, 2) == kernel::KernelObjectStatus::kInvalidType,
          "KeInitializeTimerEx rejects an invalid timer type");
    Check(kernel::InitializeDpcChecked(mem::kUserLow + 0x7F000, 0x002B5E00, 0) ==
              kernel::KernelObjectStatus::kUncommitted,
          "KeInitializeDpc rejects an uncommitted object");
    Check(kernel::InitializeDpcChecked(dpc, 0x002B5E00, 0) == kernel::KernelObjectStatus::kOk,
          "DPC reinitialisation is deterministic");

    const uint32_t cross_page = page + mem::kPage - 0x10;
    Check(mem::Commit(page + mem::kPage, mem::kPage), "cross-page DPC page commits");
    Check(kernel::InitializeDpcChecked(cross_page, 0x002B5E00, 0) == kernel::KernelObjectStatus::kOk,
          "cross-page DPC initialisation succeeds through checked span validation");
    const std::array<uint8_t, 0x1C> cross_state = ReadDpc(cross_page);
    Check(Read32(cross_state, 0x0C) == 0x002B5E00, "cross-page DPC routine is preserved");

    Check(mem::Protect(page, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "DPC page becomes read-only");
    Check(kernel::InitializeDpcChecked(dpc, 0x002B5E00, 0) == kernel::KernelObjectStatus::kPermissionDenied,
          "KeInitializeDpc rejects a read-only object");
    Check(mem::Protect(page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "DPC page restoration succeeds");
    Check(mem::Decommit(page + mem::kPage, mem::kPage), "cross-page DPC page decommits");
    Check(kernel::InitializeDpcChecked(cross_page, 0x002B5E00, 0) == kernel::KernelObjectStatus::kUncommitted,
          "decommitted cross-page object is rejected");
    kernel::ClearShutdownRegistrationsForTest();
    const uint32_t registration = page + 0x300;
    std::array<uint8_t, 0x10> registration_bytes{};
    const uint32_t routine = 0x002B8D6D, priority = 0x80000000u;
    std::memcpy(registration_bytes.data(), &routine, 4);
    std::memcpy(registration_bytes.data() + 4, &priority, 4);
    Check(mem::Write(registration, registration_bytes.data(), registration_bytes.size()) == mem::AccessStatus::kOk,
          "shutdown registration object is writable");
    Check(kernel::RegisterShutdownNotificationChecked(registration, 1, 7, "test-image", 0x00011000u, 0x00380000u) ==
              kernel::KernelObjectStatus::kOk,
          "checked shutdown registration succeeds");
    Check(kernel::ShutdownRegistrationCountForTest() == 1, "shutdown registration record is observable");
    kernel::ShutdownRegistrationRecord record;
    Check(kernel::ShutdownRegistrationForTest(0, &record) && record.object == registration &&
              record.notification_routine == routine && record.priority == priority && record.owner_thread == 7 &&
              std::strcmp(record.image_sha1, "test-image") == 0,
          "shutdown record preserves the object, callback, priority and owner");
    Check(kernel::RegisterShutdownNotificationChecked(registration, 1, 7, "test-image", 0x00011000u, 0x00380000u) ==
              kernel::KernelObjectStatus::kDuplicateRegistration,
          "duplicate shutdown registration is rejected");
    Check(kernel::RegisterShutdownNotificationChecked(registration, 0, 7, "test-image", 0x00011000u, 0x00380000u) ==
              kernel::KernelObjectStatus::kUnsupportedAction,
          "shutdown removal remains unavailable");
    Check(kernel::RegisterShutdownNotificationChecked(registration, 1, 7, "", 0x00011000u, 0x00380000u) ==
              kernel::KernelObjectStatus::kStaleOwner,
          "shutdown registration rejects a missing image identity");
    Check(kernel::RegisterShutdownNotificationChecked(registration, 1, 7, "test-image", 0x00380000u, 0x00400000u) ==
              kernel::KernelObjectStatus::kInvalidCallback,
          "shutdown registration rejects a callback outside the reviewed range");
    Check(kernel::RegisterShutdownNotificationChecked(mem::kUserLow + 0x7F000, 1, 7, "test-image", 0x00011000u,
                                                      0x00380000u) == kernel::KernelObjectStatus::kUncommitted,
          "shutdown registration rejects an uncommitted object");
    kernel::ClearShutdownRegistrationsForTest();
    const uint32_t fresh_registration = page + 0x400;
    Check(mem::Write(fresh_registration, registration_bytes.data(), registration_bytes.size()) ==
              mem::AccessStatus::kOk,
          "fresh read-only registration object is writable before protection");
    Check(mem::Protect(page, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "shutdown registration page becomes read-only");
    Check(kernel::RegisterShutdownNotificationChecked(fresh_registration, 1, 7, "test-image", 0x00011000u,
                                                      0x00380000u) == kernel::KernelObjectStatus::kOk,
          "valid first registration accepts a readable read-only object");
    Check(mem::Protect(page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "read-only registration page restores");
    Check(kernel::ShutdownRegistrationCountForTest() == 1, "shutdown registration survives until teardown");
    mem::Shutdown();
    Check(kernel::ShutdownRegistrationCountForTest() == 0,
          "production image teardown clears shutdown registration metadata");
    return failures;
}
