#include "kernel/kernel.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kBase = 0x72000000;
constexpr uint32_t kHostPage = 4 * mem::kPage;
constexpr uint32_t kCommittedBytes = 2 * kHostPage;
constexpr uint32_t kStack = kBase + mem::kPage + 0x400;
constexpr uint32_t kBuffer = kBase + mem::kPage + 0x100;
constexpr uint32_t kDeniedIosb = kBase + 0x20;
constexpr uint32_t kGoodIosb = kBase + 0x40;
constexpr uint32_t kRaceIosb = kBase + kHostPage + 0x20;

int failures = 0;
#ifdef MOSTWANTED_FILE_TRANSFER_TEST_STUBS
std::mutex g_handles_lock;
std::unordered_map<uint32_t, kernel::ObjectRef> g_handles;
uint32_t g_next_handle = 0x7000;
#endif

void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

FILE* MakeHostFile(const char* bytes, size_t size) {
    FILE* fp = std::tmpfile();
    if (!fp) return nullptr;
    if (size && std::fwrite(bytes, 1, size, fp) != size) {
        std::fclose(fp);
        return nullptr;
    }
    std::fflush(fp);
    std::fseek(fp, 0, SEEK_SET);
    return fp;
}

bool ReadHostFile(FILE* fp, std::vector<char>& bytes, size_t size) {
    std::fflush(fp);
    if (std::fseek(fp, 0, SEEK_SET) != 0) return false;
    bytes.assign(size, 0);
    return !size || std::fread(bytes.data(), 1, size, fp) == size;
}

bool ReadGuest(uint32_t address, void* out, size_t size) {
    return mem::Read(address, out, size) == mem::AccessStatus::kOk;
}

bool WriteGuest(uint32_t address, const void* bytes, size_t size) {
    return mem::Write(address, bytes, size) == mem::AccessStatus::kOk;
}

bool FileSignal(uint32_t handle, int32_t value) {
    kernel::ObjectRef object = kernel::FromHandle(handle);
    return object && object->signal == value;
}

struct Call {
    X86Context context{};
    uint32_t original_esp = kStack;
    uint32_t original_flags[6] = {1, 0, 1, 1, 0, 1};
    bool stack_ready = false;

    Call(uint32_t handle, uint32_t iosb, uint32_t buffer, uint32_t length) {
        const uint32_t words[] = {0x12345678, handle, 0, 0, 0, iosb, buffer, length, 0};
        stack_ready = WriteGuest(original_esp, words, sizeof(words));
        context.esp = original_esp;
        context.eax = 0xDEADBEEF;
        context.cf = original_flags[0];
        context.zf = original_flags[1];
        context.sf = original_flags[2];
        context.of = original_flags[3];
        context.pf = original_flags[4];
        context.af = original_flags[5];
    }

    bool Returned(uint32_t eax) const {
        uint32_t ret = 0;
        return stack_ready && context.eax == eax && context.esp == original_esp + 36 &&
               context.cf == original_flags[0] && context.zf == original_flags[1] &&
               context.sf == original_flags[2] && context.of == original_flags[3] &&
               context.pf == original_flags[4] && context.af == original_flags[5] &&
               ReadGuest(original_esp, &ret, sizeof(ret)) && ret == 0x12345678;
    }
};

std::atomic<uint32_t> g_hook_calls{0};
std::atomic<bool> g_decommit_succeeded{false};

void DecommitIosbAfterIo(uint32_t iosb) {
    ++g_hook_calls;
    const uint32_t page = iosb & ~(kHostPage - 1);
    std::barrier started(2);
    std::thread mutator([&] {
        started.arrive_and_wait();
        g_decommit_succeeded.store(mem::Decommit(page, kHostPage));
    });
    started.arrive_and_wait();
    mutator.join();
}

}  // namespace

namespace kernel {

#ifdef MOSTWANTED_FILE_TRANSFER_TEST_STUBS
uint32_t NewHandle(ObjectRef object) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    const uint32_t handle = g_next_handle;
    g_next_handle += 4;
    g_handles.emplace(handle, std::move(object));
    return handle;
}

ObjectRef FromHandle(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    auto found = g_handles.find(handle);
    return found == g_handles.end() ? nullptr : found->second;
}

bool CloseHandle(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    return g_handles.erase(handle) != 0;
}

void RegisterExport(uint32_t ordinal, const char*, KernelFn fn) {
    (void)ordinal;
    (void)fn;
}

int32_t SetEvent(Object& object) {
    object.signal = 1;
    return 0;
}

int32_t ResetEvent(Object& object) {
    object.signal = 0;
    return 0;
}

Thread* Current() { return nullptr; }

void QueueApc(Thread&, uint32_t, uint32_t, uint32_t, uint32_t) {}
#endif

}  // namespace kernel

extern "C" uint32_t FileTransferTestCreateHandle(FILE* fp);
extern "C" void FileTransferTestSetAfterIoHook(void (*hook)(uint32_t));
extern "C" bool FileTransferTestInvokeExport(uint32_t ordinal, X86Context& ctx);

extern "C" int RunFileTransferExportTests() {
    failures = 0;
    g_hook_calls.store(0);
    g_decommit_succeeded.store(false);

    Check(mem::IsInitialized() || mem::Init(), "memory is initialized for file-export tests");
    const bool pages_committed = mem::Commit(kBase, kCommittedBytes);
    Check(pages_committed, "guest pages for file-export tests commit");
    if (!mem::IsInitialized() || !pages_committed) return failures + 1;

    const char read_bytes[] = "READ";
    FILE* read_fp = MakeHostFile(read_bytes, sizeof(read_bytes) - 1);
    Check(read_fp != nullptr, "temporary host file opens for export-level read");
    if (!read_fp) return failures + 1;
    const uint32_t read_handle = FileTransferTestCreateHandle(read_fp);
    Call successful_read(read_handle, kGoodIosb, kBuffer, sizeof(read_bytes) - 1);
    Check(FileTransferTestInvokeExport(219, successful_read.context), "ordinal 219 reaches the real NtReadFile entry");
    char guest_read[sizeof(read_bytes) - 1]{};
    uint32_t iosb[2]{};
    Check(successful_read.Returned(kernel::kSuccess) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kSuccess && iosb[1] == sizeof(read_bytes) - 1 &&
              ReadGuest(kBuffer, guest_read, sizeof(guest_read)) &&
              std::memcmp(guest_read, read_bytes, sizeof(guest_read)) == 0 && FileSignal(read_handle, 1),
          "NtReadFile publishes data, IOSB, EAX and the eight-argument stdcall return");
    kernel::CloseHandle(read_handle);

    Check(mem::Protect(kBase, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "IOSB page becomes read-only for preflight failure");
    const char original[] = "OLD!";
    const char replacement[] = "NEW!";
    FILE* denied_fp = MakeHostFile(original, sizeof(original) - 1);
    Check(denied_fp != nullptr, "temporary host file opens for IOSB preflight test");
    if (!denied_fp) return failures + 1;
    const uint32_t denied_handle = FileTransferTestCreateHandle(denied_fp);
    if (kernel::ObjectRef denied_object = kernel::FromHandle(denied_handle)) denied_object->signal = 1;
    Check(WriteGuest(kBuffer, replacement, sizeof(replacement) - 1), "write source buffer is seeded");
    Call denied_write(denied_handle, kDeniedIosb, kBuffer, sizeof(replacement) - 1);
    Check(FileTransferTestInvokeExport(236, denied_write.context), "ordinal 236 reaches the real NtWriteFile entry");
    std::vector<char> denied_contents;
    Check(denied_write.context.eax == kernel::kAccessViolation, "invalid IOSB returns access violation in EAX");
    Check(denied_write.context.esp == denied_write.original_esp + 36 &&
              denied_write.context.cf == denied_write.original_flags[0] &&
              denied_write.context.zf == denied_write.original_flags[1] &&
              denied_write.context.sf == denied_write.original_flags[2] &&
              denied_write.context.of == denied_write.original_flags[3] &&
              denied_write.context.pf == denied_write.original_flags[4] &&
              denied_write.context.af == denied_write.original_flags[5],
          "invalid IOSB preserves flags and eight-argument stdcall stack cleanup");
    uint32_t denied_return = 0;
    Check(ReadGuest(denied_write.original_esp, &denied_return, sizeof(denied_return)) &&
              denied_return == 0x12345678,
          "invalid IOSB preserves the caller return address");
    const bool denied_unchanged = ReadHostFile(denied_fp, denied_contents, sizeof(original) - 1) &&
                                  std::memcmp(denied_contents.data(), original, sizeof(original) - 1) == 0;
    Check(denied_unchanged, "invalid IOSB rejects NtWriteFile before host I/O");
    Check(FileSignal(denied_handle, 1), "invalid IOSB leaves the file event state unchanged");
    kernel::CloseHandle(denied_handle);
    Check(mem::Protect(kBase, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "IOSB page returns to read-write");

    FILE* write_fp = MakeHostFile(original, sizeof(original) - 1);
    Check(write_fp != nullptr, "temporary host file opens for export-level write");
    if (!write_fp) return failures + 1;
    const uint32_t write_handle = FileTransferTestCreateHandle(write_fp);
    Call successful_write(write_handle, kGoodIosb, kBuffer, sizeof(replacement) - 1);
    Check(FileTransferTestInvokeExport(236, successful_write.context), "ordinal 236 invokes the real NtWriteFile entry");
    std::vector<char> written_contents;
    Check(successful_write.Returned(kernel::kSuccess) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kSuccess && iosb[1] == sizeof(replacement) - 1 &&
              ReadHostFile(write_fp, written_contents, sizeof(replacement) - 1) &&
              std::memcmp(written_contents.data(), replacement, sizeof(replacement) - 1) == 0 &&
              FileSignal(write_handle, 1),
          "NtWriteFile writes the checked guest snapshot and publishes matching IOSB and ABI state");
    kernel::CloseHandle(write_handle);

    const char race_bytes[] = "RACE";
    FILE* race_fp = MakeHostFile(race_bytes, sizeof(race_bytes) - 1);
    Check(race_fp != nullptr, "temporary host file opens for publication-race injection");
    if (!race_fp) return failures + 1;
    const uint32_t race_handle = FileTransferTestCreateHandle(race_fp);
    Call publication_race(race_handle, kRaceIosb, kBuffer, sizeof(race_bytes) - 1);
    FileTransferTestSetAfterIoHook(DecommitIosbAfterIo);
    Check(FileTransferTestInvokeExport(219, publication_race.context), "race fixture invokes the real NtReadFile entry");
    FileTransferTestSetAfterIoHook(nullptr);
    char race_guest[sizeof(race_bytes) - 1]{};
    Check(publication_race.Returned(kernel::kAccessViolation) && g_hook_calls.load() == 1 &&
              g_decommit_succeeded.load() && ReadGuest(kBuffer, race_guest, sizeof(race_guest)) &&
              std::memcmp(race_guest, race_bytes, sizeof(race_guest)) == 0 && std::ftell(race_fp) == 4 &&
              FileSignal(race_handle, 0),
          "concurrent IOSB decommit reports AV after read data and host cursor already changed");
    Check(mem::Commit(kBase + kHostPage, kHostPage), "race IOSB host page recommits for cursor check");
    Call after_failed_publish(race_handle, kGoodIosb, kBuffer, sizeof(race_bytes) - 1);
    FileTransferTestInvokeExport(219, after_failed_publish.context);
    Check(after_failed_publish.Returned(kernel::kEndOfFile) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kEndOfFile && iosb[1] == 0,
          "failed status publication does not roll back the file cursor before a later EOF");
    kernel::CloseHandle(race_handle);

    FileTransferTestSetAfterIoHook(nullptr);
    Check(mem::Decommit(kBase, kCommittedBytes), "file-export guest pages release cleanly");
    return failures;
}
