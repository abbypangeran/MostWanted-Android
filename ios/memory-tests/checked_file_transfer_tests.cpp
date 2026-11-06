#include "kernel/checked_file_transfer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;
constexpr uint32_t kBase = 0x70000000;

void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

struct WriteContext {
    uint32_t guest_mutation = 0;
    std::vector<uint8_t> seen;
    std::size_t returned = SIZE_MAX;
    uint32_t calls = 0;
};

std::size_t HostWrite(void* raw, const uint8_t* source, std::size_t size) {
    auto& context = *static_cast<WriteContext*>(raw);
    ++context.calls;
    context.seen.assign(source, source + size);
    if (context.guest_mutation) {
        std::vector<uint8_t> replacement(size, 0xCC);
        mem::Write(context.guest_mutation, replacement.data(), replacement.size());
    }
    return std::min(size, context.returned);
}

struct ReadContext {
    std::vector<uint8_t> bytes;
    std::size_t returned = SIZE_MAX;
    uint32_t calls = 0;
    uint32_t decommit_address = 0;
    uint32_t decommit_size = 0;
    std::atomic<bool> decommit_finished{false};
    std::atomic<bool> decommit_succeeded{false};
    bool finished_before_callback_return = false;
    std::thread decommit_thread;
};

std::size_t HostRead(void* raw, uint8_t* destination, std::size_t size) {
    auto& context = *static_cast<ReadContext*>(raw);
    ++context.calls;
    const std::size_t count = std::min({size, context.bytes.size(), context.returned});
    std::copy_n(context.bytes.data(), count, destination);
    if (context.decommit_address) {
        context.decommit_thread = std::thread([&context] {
            context.decommit_succeeded.store(mem::Decommit(context.decommit_address, context.decommit_size));
            context.decommit_finished.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!context.decommit_finished.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        context.finished_before_callback_return = context.decommit_finished.load();
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

extern "C" int RunCheckedFileTransferTests() {
    failures = 0;
    Check(mem::IsInitialized() || mem::Init(), "guest memory is initialized for checked file-transfer tests");
    if (!mem::IsInitialized()) return failures + 1;
    Check(mem::Commit(kBase, 2 * mem::kPage), "file-transfer guest pages commit");

    const std::vector<uint8_t> original{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<uint8_t> replacement(original.size(), 0xCC);
    Check(GuestWrite(kBase, original), "guest write fixture is seeded through checked access");
    WriteContext write_context{kBase, {}, 3};
    auto write = kernel::checked_file_transfer::WriteToHost(kBase, uint32_t(original.size()), &write_context, HostWrite);
    Check(write.status == kernel::checked_file_transfer::Status::kOk && write.transferred == 3 &&
              write_context.calls == 1 && write_context.seen == original,
          "host write receives one full checked snapshot and preserves short-transfer count");
    std::vector<uint8_t> observed(original.size());
    Check(GuestRead(kBase, observed) && observed == replacement,
          "guest changes during host write cannot alter the staged snapshot");

    std::vector<uint8_t> initial(8, 0xA5);
    Check(GuestWrite(kBase + 0x100, initial), "short-read destination is seeded");
    ReadContext short_read{{0x11, 0x22, 0x33}, 3};
    auto read = kernel::checked_file_transfer::ReadFromHost(kBase + 0x100, uint32_t(initial.size()), &short_read, HostRead);
    std::vector<uint8_t> partial(initial.size());
    Check(read.status == kernel::checked_file_transfer::Status::kOk && read.transferred == 3 &&
              GuestRead(kBase + 0x100, partial) &&
              partial == std::vector<uint8_t>({0x11, 0x22, 0x33, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5}),
          "short host read publishes only transferred bytes with one checked write");

    const uint32_t crossing = kBase + mem::kPage - 4;
    const std::vector<uint8_t> sentinel(4, 0x6D);
    Check(GuestWrite(crossing, sentinel), "cross-page destination prefix is seeded");
    Check(mem::Decommit(kBase + mem::kPage, mem::kPage), "second guest page decommits for full-range check");
    ReadContext invalid_range{{1, 2, 3, 4, 5, 6, 7, 8}};
    read = kernel::checked_file_transfer::ReadFromHost(crossing, 8, &invalid_range, HostRead);
    std::vector<uint8_t> prefix(4);
    Check(read.status == kernel::checked_file_transfer::Status::kGuestMemory && invalid_range.calls == 0 &&
              GuestRead(crossing, prefix) && prefix == sentinel,
          "invalid full destination is rejected before host read without a guest prefix write");
    Check(mem::Commit(kBase + mem::kPage, mem::kPage), "second guest page recommits for transfer race check");

    const std::vector<uint8_t> race_sentinel(8, 0x5A);
    Check(GuestWrite(crossing, race_sentinel), "race destination is seeded across both guest pages");
    ReadContext raced{{0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38}};
    raced.decommit_address = kBase + mem::kPage;
    raced.decommit_size = mem::kPage;
    read = kernel::checked_file_transfer::ReadFromHost(crossing, 8, &raced, HostRead);
    if (raced.decommit_thread.joinable()) raced.decommit_thread.join();
    prefix.assign(4, 0);
    Check(raced.finished_before_callback_return && raced.decommit_succeeded.load() &&
              read.status == kernel::checked_file_transfer::Status::kGuestMemory && read.transferred == 0 &&
              GuestRead(crossing, prefix) && prefix == std::vector<uint8_t>(4, 0x5A),
          "decommit during host read publishes no partial guest write and holds no memory lock across I/O");
    Check(mem::Commit(kBase + mem::kPage, mem::kPage), "raced guest page recommits for remaining checks");

    Check(mem::Protect(kBase + mem::kPage, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "destination page becomes read-only for permission check");
    ReadContext denied{{1, 2, 3, 4}};
    read = kernel::checked_file_transfer::ReadFromHost(kBase + mem::kPage, 4, &denied, HostRead);
    Check(read.status == kernel::checked_file_transfer::Status::kGuestMemory && denied.calls == 0,
          "read-only destination is rejected before host read");
    Check(mem::Protect(kBase + mem::kPage, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "destination page returns to read-write");

    WriteContext overflow_write;
    write = kernel::checked_file_transfer::WriteToHost(UINT32_MAX - 3, 8, &overflow_write, HostWrite);
    ReadContext overflow_read{{1, 2, 3, 4}};
    read = kernel::checked_file_transfer::ReadFromHost(UINT32_MAX - 3, 8, &overflow_read, HostRead);
    Check(write.status == kernel::checked_file_transfer::Status::kGuestMemory && overflow_write.calls == 0 &&
              read.status == kernel::checked_file_transfer::Status::kGuestMemory && overflow_read.calls == 0,
          "overflowing guest spans fail before host I/O");

    WriteContext oversized;
    write = kernel::checked_file_transfer::WriteToHost(0, kernel::checked_file_transfer::kMaxStagedBytes + 1,
                                                        &oversized, HostWrite);
    Check(write.status == kernel::checked_file_transfer::Status::kRequestTooLarge && oversized.calls == 0,
          "oversized transfers fail without unbounded staging allocation or host I/O");

    Check(mem::Decommit(kBase, 2 * mem::kPage), "file-transfer guest pages release cleanly");
    return failures;
}
