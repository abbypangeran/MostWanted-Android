#include "kernel/memory.h"

#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char *name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

uint64_t ResidentBytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return UINT64_MAX;
    return info.resident_size;
}

bool IsCommitted(uint32_t base, size_t bytes) {
    for (size_t offset = 0; offset < bytes; offset += mem::kPage)
        if (!mem::IsCommitted(base + uint32_t(offset))) return false;
    return true;
}

}  // namespace

extern "C" int RunMemoryTests() {
    const char *old_tmp = std::getenv("TMPDIR");
    std::string saved_tmp = old_tmp ? old_tmp : "";
    bool had_tmp = old_tmp != nullptr;
    setenv("TMPDIR", "/dev/null", 1);
    Check(!mem::Init(), "unwritable temporary directory fails initialization");
    Check(mem::Base() == nullptr, "failed initialization leaves no guest base");
    Check(!mem::Commit(mem::kUserLow, mem::kPage), "commit before initialization fails safely");
    if (had_tmp) setenv("TMPDIR", saved_tmp.c_str(), 1);
    else unsetenv("TMPDIR");

    uint64_t rss_before = ResidentBytes();
    Check(mem::Init(), "reserve guest address space and create RAM backing");
    uint8_t *base = mem::Base();
    Check(base != nullptr, "guest base published only after successful initialization");
    if (!base) return 1;
    Check(mem::Init() && mem::Base() == base, "repeated initialization is idempotent");

    const size_t host_page = size_t(sysconf(_SC_PAGESIZE));
    Check(host_page >= mem::kPage && host_page % mem::kPage == 0, "host page is a multiple of the guest page");
    Check(!mem::Commit(UINT32_MAX, 2), "commit beyond the 4 GB guest range is rejected");
    Check(!mem::Decommit(UINT32_MAX, 2), "decommit beyond the 4 GB guest range is rejected");
    Check(!mem::Commit(mem::kContiguousBase + mem::kPhysicalSize - 4, 8),
          "a commit crossing a physical alias boundary is rejected");
    char residency = 0;
    void *last_guest_page = base + (uint64_t(1) << 32) - host_page;
    Check(mincore(last_guest_page, host_page, &residency) == 0, "the reservation includes its final host page");

    constexpr uint32_t physical_offset = 0x12000;
    base[mem::kContiguousBase + physical_offset] = 0xA7;
    Check(base[mem::kWriteCombineBase + physical_offset] == 0xA7, "physical alias windows share backing");
    base[mem::kWriteCombineBase + physical_offset] = 0x5C;
    Check(base[mem::kContiguousBase + physical_offset] == 0x5C, "writes through either physical alias are coherent");

    const uint32_t guest_va = 0x20000;
    if (host_page > mem::kPage) {
        Check(!mem::Commit(guest_va, mem::kPage), "partial host-page commit fails closed");
        Check(!IsCommitted(guest_va, host_page), "partial commit leaves neighboring guest pages uncommitted");
    }
    Check(mem::Commit(guest_va, uint32_t(host_page)), "whole host-page guest range commits");
    if (IsCommitted(guest_va, host_page)) {
        for (size_t offset = 0; offset < host_page; offset += mem::kPage)
            base[guest_va + offset] = uint8_t(offset / mem::kPage + 1);
        Check(mem::Commit(guest_va, uint32_t(host_page)), "repeated commit preserves valid guest state");
        Check(base[guest_va] == 1, "repeated commit preserves existing data");
        if (host_page > mem::kPage) {
            Check(!mem::Decommit(guest_va + mem::kPage, mem::kPage), "partial host-page decommit fails closed");
            Check(IsCommitted(guest_va, host_page), "partial decommit preserves every neighboring page state");
            Check(base[guest_va] == 1, "partial decommit preserves neighboring contents");
        }
        Check(mem::Decommit(guest_va, uint32_t(host_page)), "whole host-page guest range decommits");
        Check(!IsCommitted(guest_va, host_page), "decommit clears guest commitment state");
        Check(mem::Commit(guest_va, uint32_t(host_page)), "decommitted range can be recommitted");
        bool zeroed = true;
        for (size_t offset = 0; offset < host_page; offset += mem::kPage)
            zeroed = zeroed && base[guest_va + offset] == 0;
        Check(zeroed, "recommitted anonymous memory is zero-filled");
        Check(mem::Decommit(guest_va, uint32_t(host_page)), "test range can be released");
    }

    mem::RegionInfo info{};
    Check(!mem::QueryVirtual(mem::kUserHigh + 1, info), "out-of-range guest query is rejected");
    uint32_t small_base = 0, small_size = mem::kPage;
    uint32_t small_status = mem::AllocateVirtual(small_base, small_size,
                                                  mem::kMemReserve | mem::kMemCommit, 4);
    if (host_page > mem::kPage) {
        Check(small_status == 0xC0000017, "4 KB reserve+commit fails closed on larger host pages");
        Check(small_base == 0 && small_size == mem::kPage, "failed 4 KB allocation preserves caller inputs");
        Check(mem::QueryVirtual(mem::kUserLow, info) && info.state == 0x10000,
              "failed 4 KB allocation rolls back its reservation");
    } else {
        Check(small_status == 0 && small_size == mem::kPage, "4 KB reserve+commit preserves guest page size");
        Check(mem::FreeVirtual(small_base, small_size, mem::kMemRelease) == 0,
              "4 KB reserve+commit allocation can be released");
    }
    uint32_t allocation = mem::kUserLow;
    uint32_t allocation_size = uint32_t(host_page);
    Check(mem::AllocateVirtual(allocation, allocation_size, mem::kMemReserve, 4) == 0,
          "fixed reservation respects guest address and allocation granularity");
    Check(allocation % 0x10000 == 0, "virtual allocation obeys 64 KB guest allocation alignment");
    Check(mem::QueryVirtual(allocation, info) && info.state == mem::kMemReserve,
          "allocator query reports a reserved guest range");
    Check(mem::FreeVirtual(allocation, allocation_size, mem::kMemRelease) == 0,
          "allocator release returns the guest range");

    uint64_t rss_after = ResidentBytes();
    Check(rss_before == UINT64_MAX || rss_after == UINT64_MAX || rss_after <= rss_before ||
              rss_after - rss_before < (8u << 20),
          "4 GB reservation does not commit gigabytes of resident memory");

    mem::Shutdown();
    Check(mem::Base() == nullptr, "shutdown releases guest address space");
    mem::Shutdown();
    Check(mem::Base() == nullptr, "repeated shutdown is idempotent");
    Check(mem::Init(), "initialization succeeds after cleanup");
    mem::Shutdown();
    return failures ? 1 : 0;
}
