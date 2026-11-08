#include "kernel/kernel.h"
#include "kernel/worker_lifecycle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
kernel::Thread* g_current_for_test = nullptr;
#endif

std::atomic<uint32_t> g_expected_iosb{0};
std::atomic<uint32_t> g_expected_status{0};
std::atomic<uint32_t> g_expected_information{0};
std::atomic<bool> g_event_after_publication{false};
std::atomic<bool> g_apc_after_publication{false};
std::atomic<bool> g_file_apc_class_ok{false};

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

    Call(uint32_t handle, uint32_t iosb, uint32_t buffer, uint32_t length, uint32_t event = 0,
         uint32_t apc = 0, uint32_t apc_context = 0) {
        const uint32_t words[] = {0x12345678, handle, event, apc, apc_context, iosb, buffer, length, 0};
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
struct PublicationGate {
    std::mutex lock;
    std::condition_variable changed;
    bool io_reached = false;
    bool mutation_waiting = false;
    bool release_io = false;
};
PublicationGate g_publication_gate;

bool PublishedExpectedIosb() {
    const uint32_t address = g_expected_iosb.load();
    uint32_t values[2]{};
    return address && ReadGuest(address, values, sizeof(values)) && values[0] == g_expected_status.load() &&
           values[1] == g_expected_information.load();
}

void ObserveCompletion(uint32_t iosb, uint32_t status, uint32_t information) {
    if (iosb == g_expected_iosb.load() && status == g_expected_status.load() &&
        information == g_expected_information.load() && PublishedExpectedIosb())
        g_event_after_publication = true;
}

void PauseAfterIo(uint32_t) {
    ++g_hook_calls;
    std::unique_lock lock(g_publication_gate.lock);
    g_publication_gate.io_reached = true;
    g_publication_gate.changed.notify_all();
    g_publication_gate.changed.wait(lock, [] { return g_publication_gate.release_io; });
}

void SignalMutationWait() {
    std::lock_guard lock(g_publication_gate.lock);
    g_publication_gate.mutation_waiting = true;
    g_publication_gate.changed.notify_all();
}

bool WaitForGate(bool PublicationGate::*field) {
    std::unique_lock lock(g_publication_gate.lock);
    return g_publication_gate.changed.wait_for(lock, std::chrono::seconds(5), [&] {
        return g_publication_gate.*field;
    });
}

void ResetPublicationGate() {
    std::lock_guard lock(g_publication_gate.lock);
    g_publication_gate.io_reached = false;
    g_publication_gate.mutation_waiting = false;
    g_publication_gate.release_io = false;
}

void ReleasePublicationGate() {
    {
        std::lock_guard lock(g_publication_gate.lock);
        g_publication_gate.release_io = true;
    }
    g_publication_gate.changed.notify_all();
}

void ExpectPublication(uint32_t iosb, uint32_t status, uint32_t information) {
    g_expected_iosb.store(iosb);
    g_expected_status.store(status);
    g_expected_information.store(information);
    g_event_after_publication = false;
    g_apc_after_publication = false;
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

Thread* Current() { return g_current_for_test; }

bool QueueApc(Thread&, uint32_t, uint32_t, uint32_t iosb, uint32_t, GuestCallbackClass callback_class) {
    g_file_apc_class_ok = callback_class == GuestCallbackClass::kFileApc;
    if (iosb == g_expected_iosb.load() && PublishedExpectedIosb()) g_apc_after_publication = true;
    return true;
}
#endif

}  // namespace kernel

extern "C" uint32_t FileTransferTestCreateHandle(FILE* fp);
extern "C" void FileTransferTestSetAfterIoHook(void (*hook)(uint32_t));
extern "C" void FileTransferTestSetAfterCompletionHook(void (*hook)(uint32_t, uint32_t, uint32_t));
extern "C" void FileTransferTestSetHostIoErrorForTest(int mode);
extern "C" bool FileTransferTestInvokeExport(uint32_t ordinal, X86Context& ctx);

namespace {
struct ExportTestCleanup {
    ~ExportTestCleanup() {
        FileTransferTestSetAfterIoHook(nullptr);
        FileTransferTestSetAfterCompletionHook(nullptr);
        FileTransferTestSetHostIoErrorForTest(0);
        mem::SetPageMutationWaitHookForTest(nullptr);
#ifdef MOSTWANTED_FILE_TRANSFER_TEST_STUBS
        g_current_for_test = nullptr;
#endif
    }
};
}  // namespace

extern "C" int RunFileTransferExportTests() {
    failures = 0;
    g_hook_calls.store(0);
    ExportTestCleanup cleanup;

    Check(mem::IsInitialized() || mem::Init(), "memory is initialized for file-export tests");
    const bool pages_committed = mem::Commit(kBase, kCommittedBytes);
    Check(pages_committed, "guest pages for file-export tests commit");
    if (!mem::IsInitialized() || !pages_committed) return failures + 1;
    FileTransferTestSetAfterCompletionHook(ObserveCompletion);

    const char read_bytes[] = "READ";
    FILE* read_fp = MakeHostFile(read_bytes, sizeof(read_bytes) - 1);
    Check(read_fp != nullptr, "temporary host file opens for export-level read");
    if (!read_fp) return failures + 1;
    const uint32_t read_handle = FileTransferTestCreateHandle(read_fp);
    kernel::Thread test_current_thread;
#ifdef MOSTWANTED_FILE_TRANSFER_TEST_STUBS
    g_current_for_test = &test_current_thread;
#endif
    ExpectPublication(kGoodIosb, kernel::kSuccess, sizeof(read_bytes) - 1);
    Call successful_read(read_handle, kGoodIosb, kBuffer, sizeof(read_bytes) - 1, 0, 0x1234, 0x5678);
    Check(FileTransferTestInvokeExport(219, successful_read.context), "ordinal 219 reaches the real NtReadFile entry");
    char guest_read[sizeof(read_bytes) - 1]{};
    uint32_t iosb[2]{};
    Check(successful_read.Returned(kernel::kSuccess) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kSuccess && iosb[1] == sizeof(read_bytes) - 1 &&
              ReadGuest(kBuffer, guest_read, sizeof(guest_read)) &&
              std::memcmp(guest_read, read_bytes, sizeof(guest_read)) == 0 && FileSignal(read_handle, 1),
          "NtReadFile publishes data, IOSB, EAX and the eight-argument stdcall return");
    Check(g_event_after_publication,
          "file completion signal follows the fully published IOSB");
#ifdef MOSTWANTED_FILE_TRANSFER_TEST_STUBS
    Check(g_apc_after_publication && g_file_apc_class_ok,
          "file APC is classed and queued only after the fully published IOSB");
#endif
#ifdef MOSTWANTED_FILE_TRANSFER_TEST_STUBS
    g_current_for_test = nullptr;
#endif
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
    Check(race_fp != nullptr, "temporary host file opens for IOSB decommit race");
    if (!race_fp) return failures + 1;
    const uint32_t race_handle = FileTransferTestCreateHandle(race_fp);
    const uint32_t sentinel[2] = {0xFEEDCAFE, 0xBAADF00D};
    Check(WriteGuest(kRaceIosb, sentinel, sizeof(sentinel)), "decommit-race IOSB sentinel is seeded");
    ExpectPublication(kRaceIosb, kernel::kSuccess, sizeof(race_bytes) - 1);
    Call decommit_race(race_handle, kRaceIosb, kBuffer, sizeof(race_bytes) - 1);
    ResetPublicationGate();
    mem::SetPageMutationWaitHookForTest(SignalMutationWait);
    FileTransferTestSetAfterIoHook(PauseAfterIo);
    std::thread decommit_export([&] { FileTransferTestInvokeExport(219, decommit_race.context); });
    const bool io_paused = WaitForGate(&PublicationGate::io_reached);
    Check(io_paused, "NtReadFile pauses after host and guest data effects");
    char race_guest[sizeof(race_bytes) - 1]{};
    Check(ReadGuest(kBuffer, race_guest, sizeof(race_guest)) &&
              std::memcmp(race_guest, race_bytes, sizeof(race_guest)) == 0 && std::ftell(race_fp) == 4,
          "page lease does not hold the global memory lock across host I/O");
    std::atomic<bool> decommit_done = false;
    std::atomic<bool> decommit_succeeded = false;
    const uint32_t race_page = kRaceIosb & ~(kHostPage - 1);
    std::thread decommitter([&] {
        decommit_succeeded = mem::Decommit(race_page, kHostPage);
        decommit_done = true;
    });
    const bool decommit_waiting = WaitForGate(&PublicationGate::mutation_waiting);
    uint32_t pending_status[2]{};
    Check(decommit_waiting && !decommit_done.load() && ReadGuest(kRaceIosb, pending_status, sizeof(pending_status)) &&
              pending_status[0] == sentinel[0] && pending_status[1] == sentinel[1] && FileSignal(race_handle, 0),
          "decommit waits while the IOSB lease preserves its old status and event state");
    ReleasePublicationGate();
    decommit_export.join();
    decommitter.join();
    FileTransferTestSetAfterIoHook(nullptr);
    mem::SetPageMutationWaitHookForTest(nullptr);
    Check(decommit_race.Returned(kernel::kSuccess) && g_hook_calls.load() == 1 && decommit_succeeded.load() &&
              g_event_after_publication && FileSignal(race_handle, 1),
          "IOSB status publishes before the waiting decommit and file completion signal");
    Check(mem::Commit(race_page, kHostPage), "decommitted IOSB pages recommit for remaining cases");
    ExpectPublication(kGoodIosb, kernel::kEndOfFile, 0);
    Call eof(race_handle, kGoodIosb, kBuffer, sizeof(race_bytes) - 1);
    FileTransferTestInvokeExport(219, eof.context);
    Check(eof.Returned(kernel::kEndOfFile) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kEndOfFile && iosb[1] == 0,
          "a repeated read reports EOF with a matching zero-byte IOSB");
    kernel::CloseHandle(race_handle);

    const char protection_bytes[] = "PROT";
    FILE* protection_fp = MakeHostFile(protection_bytes, sizeof(protection_bytes) - 1);
    Check(protection_fp != nullptr, "temporary host file opens for IOSB protection race");
    if (!protection_fp) return failures + 1;
    const uint32_t protection_handle = FileTransferTestCreateHandle(protection_fp);
    Check(WriteGuest(kRaceIosb, sentinel, sizeof(sentinel)), "protection-race IOSB sentinel is seeded");
    ExpectPublication(kRaceIosb, kernel::kSuccess, sizeof(protection_bytes) - 1);
    Call protection_race(protection_handle, kRaceIosb, kBuffer, sizeof(protection_bytes) - 1);
    ResetPublicationGate();
    g_hook_calls = 0;
    mem::SetPageMutationWaitHookForTest(SignalMutationWait);
    FileTransferTestSetAfterIoHook(PauseAfterIo);
    std::thread protection_export([&] { FileTransferTestInvokeExport(219, protection_race.context); });
    const bool protection_io_paused = WaitForGate(&PublicationGate::io_reached);
    std::atomic<bool> protection_done = false;
    std::atomic<bool> protection_succeeded = false;
    std::thread protector([&] {
        protection_succeeded = mem::Protect(race_page, kHostPage, mem::kRead) == mem::AccessStatus::kOk;
        protection_done = true;
    });
    const bool protection_waiting = WaitForGate(&PublicationGate::mutation_waiting);
    const auto writable_while_leased = mem::CheckAccess(kRaceIosb, 8, mem::kWrite);
    Check(protection_io_paused && protection_waiting && !protection_done.load() &&
              writable_while_leased == mem::AccessStatus::kOk,
          "protection change waits for the leased writable IOSB");
    ReleasePublicationGate();
    protection_export.join();
    protector.join();
    FileTransferTestSetAfterIoHook(nullptr);
    mem::SetPageMutationWaitHookForTest(nullptr);
    Check(protection_race.Returned(kernel::kSuccess) && protection_succeeded.load() &&
              ReadGuest(kRaceIosb, iosb, sizeof(iosb)) && iosb[0] == kernel::kSuccess &&
              iosb[1] == sizeof(protection_bytes) - 1 && g_event_after_publication &&
              mem::CheckAccess(kRaceIosb, 8, mem::kWrite) == mem::AccessStatus::kPermissionDenied,
          "IOSB publication completes before the waiting protection change");
    Check(mem::Protect(race_page, kHostPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "IOSB pages return to read-write after protection race");
    kernel::CloseHandle(protection_handle);

    FILE* short_fp = MakeHostFile("AB", 2);
    Check(short_fp != nullptr, "temporary host file opens for short-read export");
    if (!short_fp) return failures + 1;
    const uint32_t short_handle = FileTransferTestCreateHandle(short_fp);
    ExpectPublication(kGoodIosb, kernel::kSuccess, 2);
    Call short_read(short_handle, kGoodIosb, kBuffer, 4);
    FileTransferTestInvokeExport(219, short_read.context);
    Check(short_read.Returned(kernel::kSuccess) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kSuccess && iosb[1] == 2,
          "short read publishes the actual byte count as successful completion");
    kernel::CloseHandle(short_handle);

    FILE* failed_read_fp = MakeHostFile("FAIL", 4);
    Check(failed_read_fp != nullptr, "temporary host file opens for injected read error");
    if (!failed_read_fp) return failures + 1;
    const uint32_t failed_read_handle = FileTransferTestCreateHandle(failed_read_fp);
    ExpectPublication(kGoodIosb, kernel::kUnsuccessful, 0);
    FileTransferTestSetHostIoErrorForTest(1);
    Call failed_read(failed_read_handle, kGoodIosb, kBuffer, 4);
    FileTransferTestInvokeExport(219, failed_read.context);
    Check(failed_read.Returned(kernel::kUnsuccessful) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kUnsuccessful && iosb[1] == 0 && g_event_after_publication,
          "host read error publishes unsuccessful status and zero bytes before signaling");
    kernel::CloseHandle(failed_read_handle);

    FILE* failed_write_fp = MakeHostFile("SAFE", 4);
    Check(failed_write_fp != nullptr, "temporary host file opens for injected write error");
    if (!failed_write_fp) return failures + 1;
    const uint32_t failed_write_handle = FileTransferTestCreateHandle(failed_write_fp);
    ExpectPublication(kGoodIosb, kernel::kUnsuccessful, 0);
    FileTransferTestSetHostIoErrorForTest(2);
    Call failed_write(failed_write_handle, kGoodIosb, kBuffer, 4);
    FileTransferTestInvokeExport(236, failed_write.context);
    std::vector<char> failed_write_contents;
    Check(failed_write.Returned(kernel::kUnsuccessful) && ReadGuest(kGoodIosb, iosb, sizeof(iosb)) &&
              iosb[0] == kernel::kUnsuccessful && iosb[1] == 0 &&
              ReadHostFile(failed_write_fp, failed_write_contents, 4) &&
              std::memcmp(failed_write_contents.data(), "SAFE", 4) == 0 && g_event_after_publication,
          "host write error publishes failure without changing the host file contents");
    kernel::CloseHandle(failed_write_handle);

    FILE* shutdown_fp = MakeHostFile("STOP", 4);
    Check(shutdown_fp != nullptr, "temporary host file opens for shutdown race");
    if (!shutdown_fp) return failures + 1;
    const uint32_t shutdown_handle = FileTransferTestCreateHandle(shutdown_fp);
    ExpectPublication(kRaceIosb, kernel::kSuccess, 4);
    Call shutdown_transfer(shutdown_handle, kRaceIosb, kBuffer, 4);
    ResetPublicationGate();
    FileTransferTestSetAfterIoHook(PauseAfterIo);
    std::atomic<bool> shutdown_export_returned = false;
    Check(kernel::StartKernelWorker([&](kernel::WorkerStopToken) {
              kernel::GuestDispatchLease dispatch_lease;
              if (dispatch_lease.Allowed()) {
                  FileTransferTestInvokeExport(219, shutdown_transfer.context);
                  shutdown_export_returned = shutdown_transfer.Returned(kernel::kSuccess);
              }
          }),
          "managed guest-dispatch worker starts the shutdown export");
    const bool shutdown_io_paused = WaitForGate(&PublicationGate::io_reached);
    std::atomic<bool> shutdown_done = false;
    std::thread shutdown([&] {
        mem::Shutdown();
        shutdown_done = true;
    });
    Check(shutdown_io_paused && !shutdown_done.load(), "memory shutdown waits for an active export's IOSB lease");
    ReleasePublicationGate();
    shutdown.join();
    FileTransferTestSetAfterIoHook(nullptr);
    Check(shutdown_export_returned.load() && shutdown_done.load() && !mem::IsInitialized() &&
              g_event_after_publication,
          "guest dispatch and IOSB publication finish before shutdown releases guest memory");
    kernel::CloseHandle(shutdown_handle);
    Check(mem::Init() && mem::Commit(kBase, kCommittedBytes), "guest memory restarts after export shutdown test");

    FileTransferTestSetAfterIoHook(nullptr);
    Check(mem::Decommit(kBase, kCommittedBytes), "file-export guest pages release cleanly");
    return failures;
}
