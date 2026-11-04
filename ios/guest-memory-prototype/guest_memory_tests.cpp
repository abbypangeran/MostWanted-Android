#include "guest_memory.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace guest_memory_prototype {
namespace {

int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

Status ProbeSystemAllocate(GuestMemory& memory, uint32_t& cursor, uint32_t size, uint32_t alignment,
                           uint32_t& address) {
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    alignment = alignment < 16 ? 16 : alignment;
    uint64_t offset = (uint64_t(cursor) + alignment - 1) & ~uint64_t(alignment - 1);
    if (offset + size > kWindowSize) return Status::kInvalidAddress;
    address = kGuestBase + uint32_t(offset);
    Status status = memory.Commit(address, size, kReadWrite);
    if (status != Status::kOk) return status;
    status = memory.Fill(address, 0, size);
    if (status == Status::kOk) cursor = uint32_t(offset + size);
    return status;
}

template <typename T>
bool ReadValue(const GuestMemory& memory, uint32_t address, T& value) {
    return memory.Read(address, &value, sizeof(value)) == Status::kOk;
}

}  // namespace

int RunGuestMemoryPrototypeTests() {
    const char* original_tmp = std::getenv("TMPDIR");
    const bool had_tmp = original_tmp != nullptr;
    const std::string saved_tmp = original_tmp ? original_tmp : "";
    setenv("TMPDIR", "/dev/null", 1);
    GuestMemory failed_init;
    Check(failed_init.Init() == Status::kHostError, "invalid temporary path fails initialization");
    Check(!failed_init.IsInitialized(), "failed initialization publishes no aliases");
    Check(failed_init.Shutdown() == Status::kOk, "failed initialization cleans up idempotently");
    if (had_tmp) setenv("TMPDIR", saved_tmp.c_str(), 1);
    else unsetenv("TMPDIR");

    GuestMemory memory;
    Check(memory.Init() == Status::kOk, "file-backed aliases initialize");
    if (!memory.IsInitialized()) return 1;
    Check(memory.HostPageSize() == kHostPageSize, "prototype runs on a 16 KB host page");
    Check(memory.Init() == Status::kOk, "repeated initialization is idempotent");

    const uint32_t base = kGuestBase;
    for (uint32_t page = 0; page < 4; ++page) {
        Check(memory.Commit(base + page * kGuestPageSize, kGuestPageSize) == Status::kOk,
              "one 4 KB guest page commits independently");
        uint8_t marker = uint8_t(0xA0 + page);
        Check(memory.Write(base + page * kGuestPageSize, &marker, sizeof(marker)) == Status::kOk,
              "committed guest page accepts checked scalar write");
    }
    uint8_t preserved = 0;
    Check(memory.Commit(base, 4 * kGuestPageSize) == Status::kOk &&
              ReadValue(memory, base, preserved) && preserved == 0xA0,
          "repeated commit preserves existing guest data");

    const uint32_t page1 = base + kGuestPageSize;
    Check(memory.Protect(page1, kGuestPageSize, kRead | kExecute) == Status::kOk,
          "one guest page receives independent read/execute permissions");
    uint8_t byte = 0;
    Check(memory.Read(page1, &byte, 1) == Status::kOk && byte == 0xA1,
          "read-only page remains readable");
    byte = 0x11;
    Check(memory.Write(page1, &byte, 1) == Status::kPermissionDenied,
          "write to protected guest page is rejected");
    Check(memory.CheckExecute(page1, 1) == Status::kOk && memory.CheckExecute(base, 1) == Status::kPermissionDenied,
          "execute permission is independent per guest page");
    byte = 0x22;
    Check(memory.Write(base, &byte, 1) == Status::kOk,
          "neighboring guest page remains writable");
    Check(memory.Protect(page1, kGuestPageSize, kReadWrite) == Status::kOk,
          "guest page can restore read/write permissions");

    uint32_t crossing = 0x44332211;
    const uint32_t crossing_va = base + kGuestPageSize - 2;
    Check(memory.Write(crossing_va, &crossing, sizeof(crossing)) == Status::kOk,
          "scalar write crossing a 4 KB boundary succeeds when both pages allow it");
    uint32_t crossing_read = 0;
    Check(ReadValue(memory, crossing_va, crossing_read) && crossing_read == crossing,
          "scalar read crossing a 4 KB boundary returns complete value");
    Check(memory.Protect(page1, kGuestPageSize, kRead) == Status::kOk,
          "cross-boundary rejection test protects second page");
    uint16_t before = 0;
    Check(ReadValue(memory, crossing_va, before), "cross-boundary prefix can be sampled");
    Check(memory.Write(crossing_va, &crossing, sizeof(crossing)) == Status::kPermissionDenied,
          "cross-boundary write validates both pages before modifying either");
    uint16_t after = 0;
    Check(ReadValue(memory, crossing_va, after) && after == before,
          "rejected cross-boundary write leaves first page unchanged");
    Check(memory.Protect(page1, kGuestPageSize, kReadWrite) == Status::kOk,
          "cross-boundary test restores second page permissions");

    std::vector<uint8_t> bulk(48);
    for (size_t i = 0; i < bulk.size(); ++i) bulk[i] = uint8_t(i ^ 0x5A);
    Check(memory.Write(base + kGuestPageSize - 16, bulk.data(), bulk.size()) == Status::kOk,
          "bulk write across guest-page boundary succeeds");
    Check(memory.Copy(base + 2 * kGuestPageSize + 64, base + kGuestPageSize - 16, bulk.size()) == Status::kOk,
          "checked guest-to-guest bulk copy spans pages");
    std::vector<uint8_t> bulk_read(bulk.size());
    Check(memory.Read(base + 2 * kGuestPageSize + 64, bulk_read.data(), bulk_read.size()) == Status::kOk &&
              bulk_read == bulk,
          "bulk copy preserves all bytes");
    std::array<uint8_t, 16> overlap{};
    for (uint8_t i = 0; i < overlap.size(); ++i) overlap[i] = i;
    const uint32_t overlap_va = base + 2 * kGuestPageSize + 0x400;
    Check(memory.Write(overlap_va, overlap.data(), overlap.size()) == Status::kOk &&
              memory.Copy(overlap_va + 4, overlap_va, 12) == Status::kOk,
          "overlapping bulk copy succeeds with memmove semantics");
    std::array<uint8_t, 16> overlap_result{};
    std::array<uint8_t, 16> overlap_expected{0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    Check(memory.Read(overlap_va, overlap_result.data(), overlap_result.size()) == Status::kOk &&
              overlap_result == overlap_expected,
          "overlapping bulk copy preserves the original source span");

    std::array<uint8_t, 16> vector_in{};
    for (uint32_t i = 0; i < vector_in.size(); ++i) vector_in[i] = uint8_t(i + 3);
    const uint32_t vector_va = base + 2 * kGuestPageSize - 8;
    Check(memory.Write(vector_va, vector_in.data(), vector_in.size()) == Status::kOk,
          "16-byte SIMD-width write crossing guest-page boundary succeeds");
    std::array<uint8_t, 16> vector_local{}, vector_out{};
    Check(memory.Read(vector_va, vector_local.data(), vector_local.size()) == Status::kOk,
          "16-byte SIMD-width read validates full span");
#if defined(__ARM_NEON)
    uint8x16_t lanes = vld1q_u8(vector_local.data());
    lanes = vaddq_u8(lanes, vdupq_n_u8(1));
    vst1q_u8(vector_out.data(), lanes);
#else
    for (size_t i = 0; i < vector_out.size(); ++i) vector_out[i] = uint8_t(vector_local[i] + 1);
#endif
    Check(memory.Write(vector_va, vector_out.data(), vector_out.size()) == Status::kOk,
          "SIMD result writes through checked guest span");
    Check(memory.Read(vector_va, vector_local.data(), vector_local.size()) == Status::kOk &&
              vector_local[0] == vector_in[0] + 1 && vector_local[15] == vector_in[15] + 1,
          "SIMD-width data remains correct across the page boundary");

    const uint32_t alias_offset = 2 * kGuestPageSize + 0x100;
    byte = 0xC7;
    Check(memory.Write(kPhysicalAliasBase + alias_offset, &byte, 1) == Status::kOk,
          "physical alias accepts checked guest write");
    byte = 0;
    Check(memory.Read(kWriteCombineAliasBase + alias_offset, &byte, 1) == Status::kOk && byte == 0xC7,
          "write-combined alias observes shared physical backing");

    const uint32_t atomic_va = base + 2 * kGuestPageSize + 0x200;
    uint32_t zero = 0;
    Check(memory.Write(atomic_va, &zero, sizeof(zero)) == Status::kOk,
          "atomic counter initializes");
    std::atomic<bool> thread_ok{true};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            for (int i = 0; i < 500; ++i) {
                uint32_t previous = 0;
                if (memory.FetchAdd32(atomic_va, 1, previous) != Status::kOk) thread_ok = false;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    uint32_t atomic_total = 0;
    Check(thread_ok && ReadValue(memory, atomic_va, atomic_total) && atomic_total == 2000,
          "checked atomic increments serialize concurrent guest access");

    const uint32_t page0_marker = base;
    const uint32_t page2_marker = base + 2 * kGuestPageSize + 0x300;
    const uint32_t page3_marker = base + 3 * kGuestPageSize;
    uint8_t page2_value = 0xA2;
    Check(memory.Write(page2_marker, &page2_value, 1) == Status::kOk,
          "neighbor sentinel is isolated from prior test spans");
    Check(memory.Decommit(page1, kGuestPageSize) == Status::kOk,
          "one 4 KB guest page decommits independently");
    Check(memory.Read(page1, &byte, 1) == Status::kUncommitted,
          "decommitted guest page rejects access");
    uint8_t neighbor = 0;
    Check(memory.Read(page0_marker, &neighbor, 1) == Status::kOk && neighbor == 0x22,
          "decommit preserves previous guest page");
    Check(memory.Read(page2_marker, &neighbor, 1) == Status::kOk && neighbor == 0xA2,
          "decommit preserves next guest page");
    Check(memory.Read(page3_marker, &neighbor, 1) == Status::kOk && neighbor == 0xA3,
          "decommit preserves fourth guest page");
    Check(memory.Commit(page1, kGuestPageSize) == Status::kOk,
          "decommitted 4 KB guest page recommits");
    std::array<uint8_t, 32> zeros{};
    std::array<uint8_t, 32> recommitted{};
    Check(memory.Read(page1, recommitted.data(), recommitted.size()) == Status::kOk && recommitted == zeros,
          "recommit zero-fills only the selected guest page");
    Check(memory.Read(page2_marker, &neighbor, 1) == Status::kOk && neighbor == 0xA2,
          "recommit leaves neighboring guest page unchanged");

    Check(memory.Read(0x1234, &byte, 1) == Status::kInvalidAddress,
          "unmapped guest address is rejected");
    Check(memory.Read(0xFFFFFFFC, &crossing, sizeof(crossing)) == Status::kInvalidAddress,
          "guest address overflow is rejected");

    uint32_t system_cursor = 0, kpcr = 0, ethread = 0;
    Check(ProbeSystemAllocate(memory, system_cursor, 0x400, 64, kpcr) == Status::kOk && kpcr % 64 == 0,
          "prototype supports AllocSystem(0x400, 64) shape for KPCR");
    Check(ProbeSystemAllocate(memory, system_cursor, 0x200, 64, ethread) == Status::kOk && ethread % 64 == 0,
          "prototype supports aligned ETHREAD allocation shape");
    uint32_t self = kpcr;
    Check(memory.Write(kpcr + 0x18, &self, sizeof(self)) == Status::kOk,
          "KPCR fields write through checked guest access");
    Check(memory.Write(ethread + 0x28, &kpcr, sizeof(kpcr)) == Status::kOk,
          "ETHREAD fields write through checked guest access");

    const uint32_t stack_alloc = kGuestBase + 0x10000;
    const uint32_t stack_size = 0x10000, tls_size = 0x40;
    const uint32_t requested_stack_commit = stack_size + tls_size + 0x1000;
    const uint32_t rounded_stack_commit = (requested_stack_commit + 0xFFFF) & ~0xFFFFu;
    const uint32_t stack_top = stack_alloc + 0x10000 + rounded_stack_commit;
    const uint32_t tls = stack_top - tls_size;
    Check((stack_alloc & 0xFFFF) == 0 &&
              memory.Commit(stack_alloc + 0x10000, rounded_stack_commit) == Status::kOk,
          "prototype supports 64 KB-aligned guarded stack commit shape");
    Check(memory.Read(stack_alloc, &byte, 1) == Status::kUncommitted,
          "stack guard remains inaccessible");
    byte = 0x6D;
    Check(memory.Write(tls, &byte, 1) == Status::kOk,
          "small TLS allocation is accessible at stack top");
    Check(memory.Read(stack_top, &byte, 1) == Status::kInvalidAddress,
          "stack top does not expose memory beyond committed range");

    const uint32_t section = kGuestBase + 0x8000 + 0x200;
    Check(memory.Commit(section, 0x4E00) == Status::kOk,
          "unaligned image-section commit rounds to guest-page bounds");
    Check(memory.Protect(section, 0x4E00, kRead | kExecute) == Status::kOk,
          "image section receives read/execute logical protection");
    Check(memory.CheckExecute(section, 1) == Status::kOk &&
              memory.Write(section, &byte, 1) == Status::kPermissionDenied,
          "image-section permissions allow execution and deny writes");
    Check(memory.Read(kGuestBase + 0xD000, &byte, 1) == Status::kUncommitted,
          "neighboring uncommitted image page remains inaccessible");

    Check(memory.Shutdown() == Status::kOk, "shutdown releases both aliases and backing file");
    Check(memory.Shutdown() == Status::kOk, "repeated shutdown is idempotent");
    Check(memory.Read(base, &byte, 1) == Status::kNotInitialized,
          "access after shutdown is rejected");
    Check(memory.Init() == Status::kOk, "prototype can initialize after cleanup");
    Check(memory.Read(base, &byte, 1) == Status::kUncommitted,
          "reinitialized page state starts uncommitted");
    Check(memory.Shutdown() == Status::kOk, "final cleanup succeeds");
    return failures ? 1 : 0;
}

}  // namespace guest_memory_prototype

extern "C" int RunGuestMemoryPrototypeTests() {
    return guest_memory_prototype::RunGuestMemoryPrototypeTests();
}
