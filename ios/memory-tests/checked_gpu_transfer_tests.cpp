#include "kernel/checked_gpu_transfer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <new>
#include <thread>
#include <vector>

namespace {

int failures = 0;
constexpr uint32_t kBase = 0x72000000;

void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

struct ReadbackContext {
    std::vector<uint8_t> bytes;
    std::size_t returned = SIZE_MAX;
    uint32_t calls = 0;
    uint32_t decommit_address = 0;
    uint32_t decommit_size = 0;
    std::atomic<bool> decommit_finished{false};
    std::atomic<bool> decommit_succeeded{false};
    bool finished_before_return = false;
    bool throw_allocation = false;
    std::thread decommit_thread;
};

std::size_t Readback(void* raw, uint8_t* destination, std::size_t capacity) {
    auto& context = *static_cast<ReadbackContext*>(raw);
    ++context.calls;
    if (context.throw_allocation) throw std::bad_alloc();
    const std::size_t count = std::min({capacity, context.bytes.size(), context.returned});
    if (count) std::copy_n(context.bytes.data(), count, destination);
    if (context.decommit_address) {
        context.decommit_thread = std::thread([&context] {
            context.decommit_succeeded.store(mem::Decommit(context.decommit_address, context.decommit_size));
            context.decommit_finished.store(true);
        });
        context.decommit_thread.join();
        context.finished_before_return = context.decommit_finished.load();
    }
    return count;
}

bool GuestRead(uint32_t address, std::vector<uint8_t>& bytes) {
    return mem::Read(address, bytes.data(), bytes.size()) == mem::AccessStatus::kOk;
}

bool GuestWrite(uint32_t address, const std::vector<uint8_t>& bytes) {
    return mem::Write(address, bytes.data(), bytes.size()) == mem::AccessStatus::kOk;
}

}  // namespace

extern "C" int RunCheckedGpuTransferTests() {
    failures = 0;
    Check(mem::IsInitialized() || mem::Init(), "guest memory is initialized for checked GPU-transfer tests");
    if (!mem::IsInitialized()) return failures + 1;
    Check(mem::Commit(kBase, 2 * mem::kPage), "GPU-transfer guest pages commit");

    constexpr uint32_t width = 2, height = 2;
    constexpr std::size_t source_pitch = 12;
    constexpr std::size_t guest_span = 128;
    const std::vector<uint8_t> initial(guest_span, 0xA5);
    const std::vector<uint8_t> source{1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE,
                                      9, 10, 11, 12, 13, 14, 15, 16, 0xDD, 0xDD, 0xDD, 0xDD};
    Check(GuestWrite(kBase, initial), "GPU-transfer destination padding is seeded");
    ReadbackContext valid{source};
    std::vector<uint8_t> published;
    auto result = kernel::checked_gpu_transfer::Transfer(kBase, guest_span, width, height, source_pitch,
                                                        &valid, Readback, published);
    std::vector<uint8_t> actual(guest_span);
    std::vector<uint8_t> expected = initial;
    std::copy_n(source.data(), 8, expected.data());
    std::copy_n(source.data() + source_pitch, 8, expected.data() + 64);
    Check(result.status == kernel::checked_gpu_transfer::Status::kOk && valid.calls == 1 &&
              GuestRead(kBase, actual) && actual == expected &&
              published == std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}),
          "completed padded source rows publish pixels through checked writes and preserve guest row padding");

    Check(GuestWrite(kBase, initial), "partial-readback destination is seeded");
    const std::vector<uint8_t> prior_output = published;
    ReadbackContext partial{source};
    partial.returned = source.size() - 1;
    result = kernel::checked_gpu_transfer::Transfer(kBase, guest_span, width, height, source_pitch,
                                                   &partial, Readback, published);
    Check(result.status == kernel::checked_gpu_transfer::Status::kReadbackFailure &&
              GuestRead(kBase, actual) && actual == initial && published == prior_output,
          "partial host output publishes no guest rows and leaves retained pixels unchanged");

    auto bad = kernel::checked_gpu_transfer::MakeLayout(width, height, 7, guest_span);
    auto small = kernel::checked_gpu_transfer::MakeLayout(width, height, source_pitch, guest_span - 1);
    auto zero = kernel::checked_gpu_transfer::MakeLayout(0, height, source_pitch, guest_span);
    auto overflow = kernel::checked_gpu_transfer::MakeLayout(UINT32_MAX, UINT32_MAX, SIZE_MAX, SIZE_MAX);
    auto too_large = kernel::checked_gpu_transfer::MakeLayout(4096, 4097, 16384, SIZE_MAX);
    ReadbackContext address_overflow{source};
    const auto invalid_address = kernel::checked_gpu_transfer::Transfer(UINT32_MAX - 64, guest_span, width, height,
                                                                       source_pitch, &address_overflow, Readback,
                                                                       published);
    Check(bad.status == kernel::checked_gpu_transfer::Status::kInvalidLayout &&
              small.status == kernel::checked_gpu_transfer::Status::kInvalidLayout &&
              zero.status == kernel::checked_gpu_transfer::Status::kInvalidLayout &&
              overflow.status != kernel::checked_gpu_transfer::Status::kOk &&
              too_large.status == kernel::checked_gpu_transfer::Status::kRequestTooLarge &&
              invalid_address.status == kernel::checked_gpu_transfer::Status::kInvalidLayout &&
              address_overflow.calls == 0,
          "invalid dimensions, pitches, capacities, and overflowing layouts are rejected");
    ReadbackContext allocation_failure;
    allocation_failure.throw_allocation = true;
    result = kernel::checked_gpu_transfer::Transfer(kBase, guest_span, width, height, source_pitch,
                                                   &allocation_failure, Readback, published);
    Check(result.status == kernel::checked_gpu_transfer::Status::kNoMemory && allocation_failure.calls == 1 &&
              GuestRead(kBase, actual) && actual == initial && published == prior_output,
          "injected readback allocation failure leaves guest and retained pixels unchanged");
    ReadbackContext gpu_failure;
    result = kernel::checked_gpu_transfer::Transfer(kBase, guest_span, width, height, source_pitch,
                                                   &gpu_failure, Readback, published);
    Check(result.status == kernel::checked_gpu_transfer::Status::kReadbackFailure &&
              GuestRead(kBase, actual) && actual == initial && published == prior_output,
          "GPU buffer allocation or submission failure leaves guest bytes unchanged");

    Check(mem::Protect(kBase, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "GPU-transfer destination page becomes read-only");
    ReadbackContext readonly{source};
    result = kernel::checked_gpu_transfer::Transfer(kBase, guest_span, width, height, source_pitch,
                                                   &readonly, Readback, published);
    Check(result.status == kernel::checked_gpu_transfer::Status::kGuestMemory &&
              result.memory_status == mem::AccessStatus::kPermissionDenied && readonly.calls == 0,
          "read-only destination is rejected before GPU readback");
    Check(mem::Protect(kBase, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "GPU-transfer destination page returns to read-write");

    const uint32_t crossing = kBase + mem::kPage - 64;
    Check(GuestWrite(crossing, std::vector<uint8_t>(64, 0x5A)), "cross-page destination prefix is seeded");
    Check(mem::Decommit(kBase + mem::kPage, mem::kPage), "second GPU-transfer guest page decommits");
    ReadbackContext uncommitted{source};
    result = kernel::checked_gpu_transfer::Transfer(crossing, guest_span, width, height, source_pitch,
                                                   &uncommitted, Readback, published);
    std::vector<uint8_t> prefix(64);
    Check(result.status == kernel::checked_gpu_transfer::Status::kGuestMemory &&
              result.memory_status == mem::AccessStatus::kUncommitted && uncommitted.calls == 0 &&
              GuestRead(crossing, prefix) && prefix == std::vector<uint8_t>(64, 0x5A),
          "full-span validation rejects an uncommitted later page before publishing a prefix");
    Check(mem::Commit(kBase + mem::kPage, mem::kPage), "second GPU-transfer guest page recommits");

    const std::vector<uint8_t> race_seed(guest_span, 0x6B);
    Check(GuestWrite(crossing, race_seed), "concurrent-decommit destination is seeded");
    ReadbackContext raced{source};
    raced.decommit_address = kBase + mem::kPage;
    raced.decommit_size = mem::kPage;
    result = kernel::checked_gpu_transfer::Transfer(crossing, guest_span, width, height, source_pitch,
                                                   &raced, Readback, published);
    if (raced.decommit_thread.joinable()) raced.decommit_thread.join();
    prefix.assign(64, 0);
    Check(raced.finished_before_return && raced.decommit_succeeded.load() &&
              result.status == kernel::checked_gpu_transfer::Status::kGuestMemory &&
              result.memory_status == mem::AccessStatus::kUncommitted &&
              GuestRead(crossing, prefix) && prefix == std::vector<uint8_t>(64, 0x6B),
          "decommit during readback wins before publication and leaves prior guest rows intact");
    Check(mem::Commit(kBase + mem::kPage, mem::kPage), "raced guest page recommits for cleanup");
    Check(mem::Decommit(kBase, 2 * mem::kPage), "GPU-transfer guest pages release cleanly");
    return failures;
}
