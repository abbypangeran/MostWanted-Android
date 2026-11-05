#include "kernel/memory.h"

#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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
    Check(!mem::IsInitialized(), "failed initialization leaves no guest memory state");
    Check(!mem::Commit(mem::kUserLow, mem::kPage), "commit before initialization fails safely");
    if (had_tmp) setenv("TMPDIR", saved_tmp.c_str(), 1);
    else unsetenv("TMPDIR");

    uint64_t rss_before = ResidentBytes();
    Check(mem::Init(), "reserve guest address space and create RAM backing");
    Check(mem::IsInitialized(), "guest memory state published only after successful initialization");
#ifdef __APPLE__
    Check(mem::Base() == nullptr, "Darwin does not expose an unrestricted guest base pointer");
#else
    Check(mem::Base() != nullptr, "legacy guest base remains available on non-Darwin platforms");
#endif
    Check(mem::Init() && mem::IsInitialized(), "repeated initialization is idempotent");

    const size_t host_page = size_t(sysconf(_SC_PAGESIZE));
    Check(host_page >= mem::kPage && host_page % mem::kPage == 0, "host page is a multiple of the guest page");
    Check(!mem::Commit(UINT32_MAX, 2), "commit beyond the 4 GB guest range is rejected");
    Check(!mem::Decommit(UINT32_MAX, 2), "decommit beyond the 4 GB guest range is rejected");
    Check(!mem::Commit(mem::kContiguousBase + mem::kPhysicalSize - 4, 8),
          "a commit crossing a physical alias boundary is rejected");
    constexpr uint32_t physical_offset = 0x12000;
    uint8_t physical_byte = 0xA7, alias_byte = 0;
    Check(mem::Write(mem::kContiguousBase + physical_offset, &physical_byte, 1) == mem::AccessStatus::kOk,
          "checked write reaches physical memory");
    Check(mem::Read(mem::kWriteCombineBase + physical_offset, &alias_byte, 1) == mem::AccessStatus::kOk &&
              alias_byte == physical_byte,
          "write-combined alias reads the same physical backing");
    physical_byte = 0x5C;
    Check(mem::Write(mem::kWriteCombineBase + physical_offset, &physical_byte, 1) == mem::AccessStatus::kOk &&
              mem::Read(mem::kContiguousBase + physical_offset, &alias_byte, 1) == mem::AccessStatus::kOk &&
              alias_byte == physical_byte,
          "writes through either physical alias remain coherent");
    std::array<uint8_t, 2 * mem::kPage + 64> alias_source{}, alias_result{};
    for (size_t i = 0; i < alias_source.size(); ++i) alias_source[i] = uint8_t(i);
    Check(mem::Write(mem::kContiguousBase + physical_offset, alias_source.data(), alias_source.size()) ==
                  mem::AccessStatus::kOk &&
              mem::Copy(mem::kWriteCombineBase + physical_offset + 8,
                        mem::kContiguousBase + physical_offset, alias_source.size() - 8) == mem::AccessStatus::kOk &&
              mem::Read(mem::kContiguousBase + physical_offset, alias_result.data(), alias_result.size()) ==
                  mem::AccessStatus::kOk &&
              std::equal(alias_result.begin(), alias_result.begin() + 8, alias_source.begin()) &&
              std::equal(alias_result.begin() + 8, alias_result.end(), alias_source.begin()),
          "multi-chunk overlapping copy across physical aliases preserves source order and prefix");

    const uint32_t guest_va = 0x20000;
    const uint8_t page_markers[] = {0xA0, 0xA1, 0xA2, 0xA3};
    for (uint32_t page = 0; page < 4; ++page) {
        const uint32_t va = guest_va + page * mem::kPage;
        Check(mem::Commit(va, mem::kPage), "one guest page commits within a host page");
        Check(mem::Write(va, &page_markers[page], 1) == mem::AccessStatus::kOk,
              "committed guest page accepts checked write");
    }
    uint8_t guest_byte = 0;
    Check(mem::Read(guest_va + 4 * mem::kPage, &guest_byte, 1) == mem::AccessStatus::kUncommitted,
          "checked read rejects adjacent uncommitted guest page");
    Check(mem::Commit(guest_va, 4 * mem::kPage), "repeated commit preserves already committed guest pages");
    Check(mem::Read(guest_va, &guest_byte, 1) == mem::AccessStatus::kOk && guest_byte == page_markers[0],
          "repeated commit preserves existing guest data");

    Check(mem::Protect(guest_va + mem::kPage, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "one guest page receives independent read-only protection");
    Check(mem::Read(guest_va + mem::kPage, &guest_byte, 1) == mem::AccessStatus::kOk &&
              guest_byte == page_markers[1],
          "protected guest page remains readable");
    Check(mem::Write(guest_va + mem::kPage, &physical_byte, 1) == mem::AccessStatus::kPermissionDenied,
          "checked write rejects protected guest page");
    Check(mem::Write(guest_va + 2 * mem::kPage, &physical_byte, 1) == mem::AccessStatus::kOk,
          "neighboring guest page retains write permission");

    uint32_t crossing = 0x44332211, crossing_read = 0;
    const uint32_t crossing_va = guest_va + mem::kPage - 2;
    uint16_t crossing_prefix_before = 0, crossing_prefix_after = 0;
    Check(mem::Read(crossing_va, &crossing_prefix_before, sizeof(crossing_prefix_before)) == mem::AccessStatus::kOk,
          "cross-page prefix can be sampled before write");
    Check(mem::Write(crossing_va, &crossing, sizeof(crossing)) == mem::AccessStatus::kPermissionDenied,
          "cross-page write checks permissions on every guest page");
    Check(mem::Read(crossing_va, &crossing_prefix_after, sizeof(crossing_prefix_after)) == mem::AccessStatus::kOk &&
              crossing_prefix_after == crossing_prefix_before,
          "rejected cross-page write leaves its first page unchanged");
    Check(mem::Protect(guest_va + mem::kPage, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "guest page protection can be restored");
    Check(mem::Write(crossing_va, &crossing, sizeof(crossing)) == mem::AccessStatus::kOk &&
              mem::Read(crossing_va, &crossing_read, sizeof(crossing_read)) == mem::AccessStatus::kOk &&
              crossing_read == crossing,
          "scalar read and write cross a guest-page boundary");

    std::array<uint8_t, 48> bulk{};
    for (size_t i = 0; i < bulk.size(); ++i) bulk[i] = uint8_t(i ^ 0x5A);
    Check(mem::Write(guest_va + mem::kPage - 16, bulk.data(), bulk.size()) == mem::AccessStatus::kOk &&
              mem::Copy(guest_va + 2 * mem::kPage + 64, guest_va + mem::kPage - 16, bulk.size()) ==
                  mem::AccessStatus::kOk,
          "checked bulk copy spans guest pages");
    std::array<uint8_t, 48> bulk_read{};
    Check(mem::Read(guest_va + 2 * mem::kPage + 64, bulk_read.data(), bulk_read.size()) == mem::AccessStatus::kOk &&
              bulk_read == bulk,
          "checked bulk copy preserves bytes");
    Check(mem::Move(guest_va + 2 * mem::kPage + 0x404, guest_va + 2 * mem::kPage + 0x400, 12) ==
                  mem::AccessStatus::kOk,
          "checked overlapping guest move succeeds");

    std::array<uint8_t, 10> x87_in{}, x87_out{};
    x87_in.fill(0x6B);
    const uint32_t x87_va = guest_va + 2 * mem::kPage - 5;
    Check(mem::Write(x87_va, x87_in.data(), x87_in.size()) == mem::AccessStatus::kOk &&
              mem::Read(x87_va, x87_out.data(), x87_out.size()) == mem::AccessStatus::kOk && x87_out == x87_in,
          "10-byte x87-width access crosses a guest-page boundary");
    std::array<uint8_t, 16> simd_in{}, simd_out{};
    simd_in.fill(0x3C);
    const uint32_t simd_va = guest_va + 3 * mem::kPage - 8;
    Check(mem::Write(simd_va, simd_in.data(), simd_in.size()) == mem::AccessStatus::kOk &&
              mem::Read(simd_va, simd_out.data(), simd_out.size()) == mem::AccessStatus::kOk &&
              simd_out == simd_in,
          "16-byte SIMD-width access crosses a guest-page boundary");

    uint32_t atomic_previous = 0, atomic_value = 7;
    const uint32_t atomic_va = guest_va + 2 * mem::kPage + 0x200;
    Check(mem::Write(atomic_va, &atomic_value, sizeof(atomic_value)) == mem::AccessStatus::kOk &&
              mem::AtomicFetchAdd32(atomic_va, 5, atomic_previous) == mem::AccessStatus::kOk &&
              atomic_previous == 7,
          "checked atomic add returns the prior guest value");
    atomic_value = 0;
    Check(mem::Read(atomic_va, &atomic_value, sizeof(atomic_value)) == mem::AccessStatus::kOk && atomic_value == 12,
          "checked atomic add stores the updated value");
    constexpr uint32_t atomic_iterations = 1000;
    std::array<std::thread, 4> atomic_threads;
    bool atomic_ok[4] = {true, true, true, true};
    for (size_t i = 0; i < atomic_threads.size(); ++i)
        atomic_threads[i] = std::thread([&, i] {
            uint32_t prior = 0;
            for (uint32_t n = 0; n < atomic_iterations; ++n)
                if (mem::AtomicFetchAdd32(atomic_va, 1, prior) != mem::AccessStatus::kOk) atomic_ok[i] = false;
        });
    for (auto& thread : atomic_threads) thread.join();
    Check(std::all_of(std::begin(atomic_ok), std::end(atomic_ok), [](bool ok) { return ok; }) &&
              mem::Read(atomic_va, &atomic_value, sizeof(atomic_value)) == mem::AccessStatus::kOk &&
              atomic_value == 12 + atomic_iterations * atomic_threads.size(),
          "concurrent checked atomic adds preserve every update");

    const uint32_t page2_sentinel = guest_va + 2 * mem::kPage + 0x300;
    const uint32_t page3_sentinel = guest_va + 3 * mem::kPage + 0x300;
    Check(mem::Write(page2_sentinel, &page_markers[2], 1) == mem::AccessStatus::kOk &&
              mem::Write(page3_sentinel, &page_markers[3], 1) == mem::AccessStatus::kOk,
          "neighbor sentinels are initialized");
    Check(mem::Decommit(guest_va + mem::kPage, mem::kPage), "one guest page decommits independently");
    Check(mem::Read(guest_va + mem::kPage, &guest_byte, 1) == mem::AccessStatus::kUncommitted,
          "decommitted guest page rejects checked reads");
    Check(mem::Read(page2_sentinel, &guest_byte, 1) == mem::AccessStatus::kOk && guest_byte == page_markers[2] &&
              mem::Read(page3_sentinel, &guest_byte, 1) == mem::AccessStatus::kOk && guest_byte == page_markers[3],
          "decommit preserves neighboring guest pages");
    std::atomic<bool> reader_done{false};
    std::atomic<uint32_t> reader_checks{0};
    std::atomic<bool> reader_ok{true};
    std::thread reader([&] {
        uint8_t byte = 0;
        while (!reader_done.load()) {
            auto status = mem::Read(guest_va, &byte, 1);
            if (status != mem::AccessStatus::kOk && status != mem::AccessStatus::kUncommitted &&
                status != mem::AccessStatus::kPermissionDenied) reader_ok = false;
            ++reader_checks;
        }
    });
    while (!reader_checks.load()) std::this_thread::yield();
    bool changes_ok = true;
    for (int i = 0; i < 32; ++i) {
        changes_ok &= mem::Protect(guest_va, mem::kPage, mem::kNoAccess) == mem::AccessStatus::kOk;
        changes_ok &= mem::Protect(guest_va, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk;
        changes_ok &= mem::Decommit(guest_va, mem::kPage);
        changes_ok &= mem::Commit(guest_va, mem::kPage);
    }
    reader_done = true;
    reader.join();
    Check(changes_ok && reader_ok && reader_checks > 0,
          "concurrent checked reads stay synchronized with protection and decommit");
    Check(mem::Commit(guest_va + mem::kPage, mem::kPage), "decommitted guest page recommits independently");
    std::array<uint8_t, 32> zero_bytes{}, recommitted{};
    Check(mem::Read(guest_va + mem::kPage, recommitted.data(), recommitted.size()) == mem::AccessStatus::kOk &&
              recommitted == zero_bytes,
          "recommitted guest page is zero-filled");
    Check(mem::Read(page2_sentinel, &guest_byte, 1) == mem::AccessStatus::kOk && guest_byte == page_markers[2],
          "recommit leaves neighboring guest page unchanged");
    Check(mem::Decommit(guest_va, 4 * mem::kPage), "guest test pages release cleanly");

    Check(mem::Read(0x1234, &guest_byte, 1) == mem::AccessStatus::kUncommitted,
          "uncommitted guest address is rejected");
    Check(mem::Read(UINT32_MAX - 2, &crossing, sizeof(crossing)) == mem::AccessStatus::kInvalidAddress,
          "checked access rejects 32-bit guest address overflow");
    Check(mem::Read(mem::kUserLow, &guest_byte, SIZE_MAX) == mem::AccessStatus::kInvalidAddress,
          "checked access rejects oversized host lengths without wrapping");
    Check(mem::Read(mem::kUserLow, nullptr, 1) == mem::AccessStatus::kInvalidAddress,
          "checked access rejects null host buffers");

    mem::RegionInfo info{};
    Check(!mem::QueryVirtual(mem::kUserHigh + 1, info), "out-of-range guest query is rejected");
    uint32_t small_base = 0, small_size = mem::kPage;
    uint32_t small_status = mem::AllocateVirtual(small_base, small_size,
                                                  mem::kMemReserve | mem::kMemCommit, 4);
    Check(small_status == 0 && small_size == mem::kPage,
          "real 4 KB reserve+commit succeeds on Darwin host pages");
    Check(small_status != 0 || mem::FreeVirtual(small_base, small_size, mem::kMemRelease) == 0,
          "4 KB reserve+commit allocation can be released");
    uint32_t allocation = mem::kUserLow;
    uint32_t allocation_size = uint32_t(host_page);
    Check(mem::AllocateVirtual(allocation, allocation_size, mem::kMemReserve, 4) == 0,
          "fixed reservation respects guest address and allocation granularity");
    Check(allocation % 0x10000 == 0, "virtual allocation obeys 64 KB guest allocation alignment");
    Check(mem::QueryVirtual(allocation, info) && info.state == mem::kMemReserve,
          "allocator query reports a reserved guest range");
    Check(mem::FreeVirtual(allocation, allocation_size, mem::kMemRelease) == 0,
          "allocator release returns the guest range");

    uint32_t kpcr = mem::AllocSystem(0x400, 64);
    uint32_t ethread = mem::AllocSystem(0x200, 64);
    Check(kpcr && kpcr % 64 == 0, "real KPCR-sized runtime allocation is 64-byte aligned");
    Check(ethread && ethread % 64 == 0, "real ETHREAD-sized runtime allocation is 64-byte aligned");
    std::array<uint8_t, 0x400> zero_kpcr{};
    std::array<uint8_t, 0x200> zero_ethread{};
    Check(kpcr && mem::Read(kpcr, zero_kpcr.data(), zero_kpcr.size()) == mem::AccessStatus::kOk &&
              std::all_of(zero_kpcr.begin(), zero_kpcr.end(), [](uint8_t value) { return value == 0; }),
          "KPCR-sized runtime allocation is zero-filled");
    Check(ethread && mem::Read(ethread, zero_ethread.data(), zero_ethread.size()) == mem::AccessStatus::kOk &&
              std::all_of(zero_ethread.begin(), zero_ethread.end(), [](uint8_t value) { return value == 0; }),
          "ETHREAD-sized runtime allocation is zero-filled");
    const uint32_t self_pcr = kpcr, current_thread = ethread;
    uint32_t readback = 0;
    Check(kpcr && mem::Write(kpcr + 0x18, &self_pcr, sizeof(self_pcr)) == mem::AccessStatus::kOk &&
              mem::Read(kpcr + 0x18, &readback, sizeof(readback)) == mem::AccessStatus::kOk && readback == kpcr,
          "KPCR self field initializes through checked guest access");
    Check(kpcr && mem::Write(kpcr + 0x28, &current_thread, sizeof(current_thread)) == mem::AccessStatus::kOk &&
              mem::Read(kpcr + 0x28, &readback, sizeof(readback)) == mem::AccessStatus::kOk && readback == ethread,
          "KPCR current-thread field initializes through checked guest access");
    Check(ethread && mem::Write(ethread, &page_markers[1], 1) == mem::AccessStatus::kOk &&
              mem::Read(ethread, &guest_byte, 1) == mem::AccessStatus::kOk && guest_byte == page_markers[1],
          "ETHREAD header initializes through checked guest access");
    if (kpcr) mem::FreeSystem(kpcr);
    if (ethread) mem::FreeSystem(ethread);

    uint32_t stack_top = mem::AllocStack(0x10000 + 0x40 + 0x1000);
    Check(stack_top && stack_top % 0x10000 == 0, "real runtime stack top preserves 64 KB alignment");
    if (stack_top) {
        uint8_t tls_marker = 0x6D, stack_byte = 0;
        const uint32_t tls = stack_top - 0x40;
        Check(mem::IsCommitted(stack_top - 1) &&
                  mem::Write(tls, &tls_marker, 1) == mem::AccessStatus::kOk &&
                  mem::Read(tls, &stack_byte, 1) == mem::AccessStatus::kOk && stack_byte == tls_marker,
              "runtime stack and TLS bytes use checked guest access");
        Check(mem::Read(stack_top - 0x20000 - 1, &stack_byte, 1) == mem::AccessStatus::kUncommitted,
              "runtime stack guard rejects checked access");
        mem::FreeStack(stack_top);
    }

    Check(mem::Commit(0x30000, 0x5000), "unaligned image-section range commits in 4 KB guest pages");
    Check(IsCommitted(0x30000, 0x5000), "unaligned image-section commit marks every covered guest page");
    Check(mem::Protect(0x32001, 0x2FFE, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk &&
              mem::CheckAccess(0x32000, mem::kPage, mem::kExecute) == mem::AccessStatus::kOk &&
              mem::Write(0x32000, &guest_byte, 1) == mem::AccessStatus::kPermissionDenied,
          "image-section protection applies to rounded guest pages");
    Check(mem::Decommit(0x30000, 0x5000), "unaligned image-section range decommits cleanly");

    constexpr uint32_t image_base = 0x02000000;
    Check(mem::ReserveImage(image_base, 0x9000), "real image reservation commits through runtime allocator");
    Check(mem::Write(image_base + 0x8FFF, &guest_byte, 1) == mem::AccessStatus::kOk,
          "reserved image tail accepts checked write");
    Check(mem::Protect(image_base + 0x1000, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk &&
              mem::CheckAccess(image_base + 0x1000, 1, mem::kExecute) == mem::AccessStatus::kOk &&
              mem::Write(image_base + 0x1000, &guest_byte, 1) == mem::AccessStatus::kPermissionDenied,
          "reserved image page keeps independent logical protection");
    uint32_t release_base = image_base, release_size = 0;
    Check(mem::FreeVirtual(release_base, release_size, mem::kMemRelease) == 0,
          "real image reservation releases cleanly");

    // Local direct access is a lower-bound proxy; Darwin intentionally hides the old raw guest pointer.
    constexpr int scalar_rounds = 50000, bulk_rounds = 1000;
    auto timed_ns = [](auto&& operation) {
        auto start = std::chrono::steady_clock::now();
        operation();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    };
    volatile uint32_t direct_value = 0;
    auto direct_scalar_ns = timed_ns([&] {
        for (int i = 0; i < scalar_rounds; ++i) direct_value = direct_value + 1;
    });
    uint32_t checked_value = 0;
    const uint32_t bench_va = mem::kContiguousBase + 0x20000;
    auto checked_scalar_ns = timed_ns([&] {
        for (int i = 0; i < scalar_rounds; ++i) {
            mem::Read(bench_va, &checked_value, sizeof(checked_value));
            ++checked_value;
            mem::Write(bench_va, &checked_value, sizeof(checked_value));
        }
    });
    std::array<uint8_t, mem::kPage> bench_source{}, bench_local_guest{}, bench_target{};
    bench_source.fill(0x5A);
    volatile uint64_t direct_checksum = 0, checked_checksum = 0;
    auto direct_bulk_ns = timed_ns([&] {
        for (int i = 0; i < bulk_rounds; ++i) {
            bench_source[0] = uint8_t(i);
            std::memcpy(bench_local_guest.data(), bench_source.data(), bench_source.size());
            std::memcpy(bench_target.data(), bench_local_guest.data(), bench_target.size());
            direct_checksum = direct_checksum + bench_target[0];
        }
    });
    auto checked_bulk_ns = timed_ns([&] {
        for (int i = 0; i < bulk_rounds; ++i) {
            bench_source[0] = uint8_t(i);
            mem::Write(bench_va, bench_source.data(), bench_source.size());
            mem::Read(bench_va, bench_target.data(), bench_target.size());
            checked_checksum = checked_checksum + bench_target[0];
        }
    });
    Check(direct_value == scalar_rounds && checked_value == scalar_rounds && bench_target == bench_source &&
              direct_checksum == checked_checksum,
          "direct proxy and checked access benchmark complete");
    std::printf("memory benchmark (ns/round): local scalar %lld, checked scalar read+write %lld; "
                "local 4KB write+read %lld, checked 4KB write+read %lld\n",
                static_cast<long long>(direct_scalar_ns / scalar_rounds),
                static_cast<long long>(checked_scalar_ns / scalar_rounds),
                static_cast<long long>(direct_bulk_ns / bulk_rounds),
                static_cast<long long>(checked_bulk_ns / bulk_rounds));

    uint64_t rss_after = ResidentBytes();
    Check(rss_before == UINT64_MAX || rss_after == UINT64_MAX || rss_after <= rss_before ||
              rss_after - rss_before < (8u << 20),
          "4 GB reservation does not commit gigabytes of resident memory");

    mem::Shutdown();
    Check(!mem::IsInitialized(), "shutdown releases guest address space");
    mem::Shutdown();
    Check(!mem::IsInitialized(), "repeated shutdown is idempotent");
    Check(mem::Init(), "initialization succeeds after cleanup");
    mem::Shutdown();
    return failures ? 1 : 0;
}
