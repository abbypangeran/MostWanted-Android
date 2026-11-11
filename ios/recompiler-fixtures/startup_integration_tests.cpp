#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/crypto.h"
#include "kernel/kernel.h"
#include "kernel/kernel_data_init.h"
#include "kernel/kernel_thunks.h"
#include "kernel/clock_worker.h"
#include "kernel/memory.h"
#include "xbe/image_loader.h"
#include "x86_recomp_shared.h"

namespace kernel {
uint8_t* g_base = nullptr;
}

namespace {

constexpr uint32_t kImageBase = 0x10000;
constexpr uint32_t kImageSize = 0x4000;
constexpr uint32_t kEntry = 0x11000;
constexpr uint32_t kImport = 0x12000;
constexpr uint32_t kDataBase = 0x30000;
constexpr uint32_t kOutput = 0x40000;
constexpr uint32_t kStackArguments = 0x51000;

int failures = 0;
kernel::ThreadRef prepared_thread;
std::array<uint32_t, 10> service_arguments{};

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::string Sha1(const std::vector<uint8_t>& bytes) {
    alignas(8) uint8_t context[128];
    uint8_t digest[20];
    crypto::ShaInit(context);
    crypto::ShaUpdate(context, bytes.data(), bytes.size());
    crypto::ShaFinal(context, digest);
    std::string result;
    result.reserve(40);
    for (uint8_t byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}

bool ReadFile(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), {});
    return bool(file) || file.eof();
}

bool AddOrdinal255Section(std::vector<uint8_t>& bytes) {
    constexpr uint32_t table = 0x180;
    constexpr uint32_t data_record = table + 2 * 56;  // .text and .text2 already exist
    constexpr uint32_t data_name = 0x258;
    constexpr uint32_t data_raw = 0x3000;             // after .text2's raw bytes
    if (bytes.size() < 0x1000 || bytes.size() > 0x3000) return false;
    bytes.resize(kImageSize, 0);
    Put32(bytes, 0x10C, kImageSize);
    Put32(bytes, 0x11C, 3);
    Put32(bytes, 0x158, (kImport ^ xbe::kRetailThunkKey));
    Put32(bytes, data_record + 0, xbe::kPreload);
    Put32(bytes, data_record + 4, kImport);
    Put32(bytes, data_record + 8, 0x1000);
    Put32(bytes, data_record + 12, data_raw);
    Put32(bytes, data_record + 16, 0x1000);
    Put32(bytes, data_record + 20, kImageBase + data_name);
    std::memcpy(bytes.data() + data_name, ".data", 6);
    Put32(bytes, data_raw, 0x800000FF);
    Put32(bytes, data_raw + 4, 0);
    return true;
}

void TestKernel255(X86Context& ctx, uint8_t*) {
    if (mem::Read(ctx.esp + 4, service_arguments.data(), sizeof(service_arguments)) != mem::AccessStatus::kOk) {
        kernel::Return(ctx, kernel::kAccessViolation, 10);
        return;
    }
    kernel::Return(ctx,
                   kernel::PrepareSystemThreadEx(service_arguments[0], service_arguments[2], service_arguments[3],
                                                 service_arguments[4], service_arguments[5], service_arguments[6],
                                                 service_arguments[7] != 0, service_arguments[9], prepared_thread),
                   10);
}

bool ResolveOrdinal255(uint32_t ordinal, uint32_t& address, bool& implemented, void*) {
    if (ordinal != 255) return false;
    address = dispatch::KernelThunkAddress(255);
    implemented = true;
    return true;
}

unsigned write_calls = 0;
unsigned fail_write_at = 0;

mem::AccessStatus InjectWriteFailure(uint32_t address, const void* source, size_t size) {
    if (++write_calls == fail_write_at) return mem::AccessStatus::kHostError;
    return mem::Write(address, source, size);
}

kernel::KernelDataLayout KernelLayout() {
    return {kDataBase, kDataBase + 0x100, kDataBase + 0x200, kDataBase + 0x300, kDataBase + 0x400,
            kDataBase + 0x500, kDataBase + 0x600, kDataBase + 0x700, kDataBase + 0x800,
            kDataBase + 0x900, kDataBase + 0xA00};
}

void InvokePreparedImport(uint32_t stack, uint32_t& eax, uint32_t& esp) {
    const std::array<uint32_t, 11> frame = {kOutput, 0, 0x10000, 0x20, kOutput + 4,
                                            0x111B2, 0x12345678, 0, 0, 0x111B2, 0xAABBCCDD};
    mem::Write(stack, frame.data(), sizeof(frame));
    X86Context context{};
    context.esp = stack;
    sub_0001119E(context, nullptr);
    eax = context.eax;
    esp = context.esp;
}

bool RunCycle(const std::vector<uint8_t>& fixture_bytes) {
#if defined(__APPLE__)
    const char* previous_tmp = std::getenv("TMPDIR");
    const std::string saved_tmp = previous_tmp ? previous_tmp : "";
    const bool had_tmp = previous_tmp != nullptr;
    const bool set_failed_tmp = setenv("TMPDIR", "/dev/null", 1) == 0;
    const bool init_failed_cleanly = set_failed_tmp && !mem::Init() && !mem::IsInitialized();
    if (had_tmp) setenv("TMPDIR", saved_tmp.c_str(), 1);
    else unsetenv("TMPDIR");
    Check(init_failed_cleanly, "memory initialization failure leaves no published guest state");
#endif
    Check(mem::Init(), "guest memory initializes for startup integration");
    if (!mem::IsInitialized()) return false;
    kernel::g_base = mem::Base();
    Check(kernel::RestartKernelWorkers(), "startup integration opens dispatch gate");

    std::string error;
    Check(dispatch::Init(error), "synthetic generated dispatch initializes");
    const dispatch::Metadata invalid_metadata{X86CodeBase, X86CodeEnd, X86EntryPoint,
                                               std::span<const X86FuncMapping>{}, {}};
    Check(!dispatch::Init(invalid_metadata, error) &&
              dispatch::ResolveTarget(kEntry).kind == dispatch::TargetKind::kUninitialized &&
              dispatch::Init(error),
          "dispatch publication failure leaves no partial mapping and restores cleanly");

    std::vector<uint8_t> bytes = fixture_bytes;
    Check(AddOrdinal255Section(bytes), "synthetic XBE gains a bounded ordinal-255 data section");
    xbe::Image image;
    Check(image.Parse(bytes, error), "synthetic XBE parses before guest publication");
    if (image.file.empty()) return false;
    const std::string sha1 = Sha1(image.file);
    write_calls = 0;
    fail_write_at = 2;
    Check(!xbe::PrepareImage(image, kEntry, sha1, error, InjectWriteFailure) &&
              !mem::IsCommitted(kImageBase),
          "injected XBE section publication failure releases the image reservation");
    fail_write_at = 0;
    Check(xbe::PrepareImage(image, kEntry, sha1, error), "synthetic XBE prepares through checked memory writes");

    Check(mem::Commit(kDataBase, mem::kPage) && mem::Fill(kDataBase, 0xA5, mem::kPage) == mem::AccessStatus::kOk,
          "kernel-data fixture page commits and receives rollback sentinels");
    std::array<uint8_t, mem::kPage> before{}, after{};
    mem::Read(kDataBase, before.data(), before.size());
    write_calls = 0;
    fail_write_at = 4;
    kernel::KernelDataMemory failing_data{nullptr, InjectWriteFailure};
    Check(!kernel::InitializeKernelData(KernelLayout(), error, failing_data) &&
              mem::Read(kDataBase, after.data(), after.size()) == mem::AccessStatus::kOk && after == before,
          "kernel-data publication failure restores every prior byte");
    fail_write_at = 0;
    Check(kernel::InitializeKernelData(KernelLayout(), error), "kernel structures initialize synchronously");

    uint32_t import = 0;
    mem::Read(kImport, &import, sizeof(import));
    Check(import == 0x800000FF, "XBE import starts as ordinal 255 before patching");
    write_calls = 0;
    fail_write_at = 1;
    std::vector<uint32_t> missing;
    kernel::ThunkMemory failing_thunks{nullptr, InjectWriteFailure};
    Check(!kernel::PatchKernelThunks(image, ResolveOrdinal255, nullptr, missing, error, failing_thunks) &&
              mem::Read(kImport, &import, sizeof(import)) == mem::AccessStatus::kOk && import == 0x800000FF,
          "injected import patch failure restores the original ordinal");
    fail_write_at = 0;
    Check(kernel::PatchKernelThunks(image, ResolveOrdinal255, nullptr, missing, error) && missing.empty() &&
              mem::Read(kImport, &import, sizeof(import)) == mem::AccessStatus::kOk &&
              import == dispatch::KernelThunkAddress(255),
          "ordinal-255 import patches to the checked PsCreateSystemThreadEx thunk");

    Check(mem::Protect(kEntry, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk &&
              mem::Commit(kOutput, mem::kPage) && mem::Commit(kStackArguments - mem::kPage, 2 * mem::kPage),
          "generated code page becomes executable for callback-ticket validation");
    kernel::RegisterExport(255, "PsCreateSystemThreadEx", TestKernel255);
    Check(dispatch::SetKernel255FixtureBridge(true), "checked test bridge admits only ordinal 255");
    const auto resource_baseline = mem::Query();
    const uint32_t workers_before = kernel::ThreadWorkerLaunchCountForTest();
    const std::array<kernel::ThreadPreparationFailPoint, 4> failpoints = {
        kernel::ThreadPreparationFailPoint::kAfterStack, kernel::ThreadPreparationFailPoint::kAfterKpcr,
        kernel::ThreadPreparationFailPoint::kAfterEthread, kernel::ThreadPreparationFailPoint::kBeforePublish};
    for (const auto point : failpoints) {
        const std::array<uint32_t, 2> sentinels{0xAAAAAAAA, 0xBBBBBBBB};
        mem::Write(kOutput, sentinels.data(), sizeof(sentinels));
        kernel::SetThreadPreparationFailureForTest(point);
        uint32_t eax = 0, esp = 0;
        InvokePreparedImport(kStackArguments, eax, esp);
        std::array<uint32_t, 2> published{};
        mem::Read(kOutput, published.data(), sizeof(published));
        Check(eax == kernel::kNoMemory && esp == kStackArguments + 44 && !prepared_thread &&
                  published == sentinels && kernel::Threads().empty() &&
                  kernel::ThreadWorkerLaunchCountForTest() == workers_before &&
                  mem::Query().system_used == resource_baseline.system_used,
              "injected prepared-thread allocation/publication failure rolls back cleanly");
    }

    const std::array<uint32_t, 2> output_sentinel{0xAAAAAAAA, 0xBBBBBBBB};
    mem::Write(kOutput, output_sentinel.data(), sizeof(output_sentinel));
    uint32_t eax = 0, esp = 0;
    InvokePreparedImport(kStackArguments, eax, esp);
    std::array<uint32_t, 2> outputs{};
    mem::Read(kOutput, outputs.data(), sizeof(outputs));
    Check(eax == kernel::kSuccess && esp == kStackArguments + 44 && prepared_thread &&
              prepared_thread->state.load() == kernel::ThreadState::kPrepared && outputs[0] &&
              outputs[1] == prepared_thread->id && kernel::FromHandle(outputs[0]).get() == prepared_thread.get() &&
              kernel::Threads().empty() && kernel::ThreadWorkerLaunchCountForTest() == workers_before,
          "checked generated ABI prepares an inactive guest thread without launching a worker");

    kernel::GuestCallbackTicket start_ticket, system_ticket, rejected_ticket;
    Check(kernel::PrepareGuestCallback(prepared_thread->start_routine, kernel::GuestCallbackClass::kThreadStart,
                                       start_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              kernel::PrepareGuestCallback(prepared_thread->system_routine,
                                           kernel::GuestCallbackClass::kSystemRoutine, system_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              start_ticket.target == prepared_thread->start_routine &&
              system_ticket.target == prepared_thread->system_routine && start_ticket.dispatch_generation &&
              start_ticket.dispatch_generation == system_ticket.dispatch_generation,
          "start and system callback tickets bind to current generated dispatch entries");
    Check(kernel::PrepareGuestCallback(0x1119F, kernel::GuestCallbackClass::kThreadStart, rejected_ticket) ==
              kernel::GuestCallbackStatus::kRejectedTarget,
          "continuation callback registration fails closed");
    Check(kernel::StopKernelWorkers() &&
              kernel::PrepareGuestCallback(prepared_thread->start_routine,
                                           kernel::GuestCallbackClass::kThreadStart, rejected_ticket) ==
                  kernel::GuestCallbackStatus::kCancelled &&
              kernel::RestartKernelWorkers(),
          "callback registration fails while shutdown gate is closed and reopens cleanly");

    std::array<uint8_t, 0x400> kpcr{};
    std::array<uint8_t, 0x200> ethread{};
    uint32_t kpcr_self = 0, ethread_tls = 0;
    const bool structures_read = mem::Read(prepared_thread->kpcr, kpcr.data(), kpcr.size()) == mem::AccessStatus::kOk &&
                                 mem::Read(prepared_thread->ethread, ethread.data(), ethread.size()) ==
                                     mem::AccessStatus::kOk;
    std::memcpy(&kpcr_self, kpcr.data() + 0x18, sizeof(kpcr_self));
    std::memcpy(&ethread_tls, ethread.data() + 0x28, sizeof(ethread_tls));
    Check(structures_read && kpcr_self == prepared_thread->kpcr && ethread_tls == prepared_thread->tls &&
              mem::IsCommitted(prepared_thread->stack_top - 1) && mem::IsCommitted(prepared_thread->tls),
          "prepared guest stack, TLS, KPCR self pointer, and ETHREAD TLS pointer validate");

    X86Context callback_context{};
    callback_context.esp = (prepared_thread->tls & ~15u) - 16;
    const std::array<uint32_t, 1> callback_args{prepared_thread->start_context};
    uint32_t callback_result = 0;
#if defined(__APPLE__)
    Check(kernel::CallGuestCallback(callback_context, start_ticket, start_ticket.target, callback_args,
                                    prepared_thread->stack_top - prepared_thread->stack_size,
                                    prepared_thread->stack_size, callback_result) ==
              kernel::GuestCallbackStatus::kExecutionDisabled,
          "valid callback frame reaches the production Apple gate and remains disabled");
    const std::array<uint32_t, 2> system_args{prepared_thread->start_routine,
                                             prepared_thread->start_context};
    Check(kernel::CallGuestCallback(callback_context, system_ticket, system_ticket.target, system_args,
                                    prepared_thread->stack_top - prepared_thread->stack_size,
                                    prepared_thread->stack_size, callback_result) ==
              kernel::GuestCallbackStatus::kExecutionDisabled,
          "system-routine callback validates function and context arguments, then remains disabled");
#endif

    const uint32_t handle = outputs[0], ethread_address = prepared_thread->ethread;
    const uint32_t stack_top = prepared_thread->stack_top;
    Check(kernel::CancelPreparedThread(prepared_thread) && !kernel::FromHandle(handle) &&
              !kernel::IsPointerBoundForTest(ethread_address, prepared_thread.get()) &&
              !mem::IsCommitted(stack_top - 1) && kernel::Threads().empty() &&
              kernel::ThreadWorkerLaunchCountForTest() == workers_before &&
              mem::Read(kOutput, outputs.data(), sizeof(outputs)) == mem::AccessStatus::kOk && !outputs[0] &&
              !outputs[1] && mem::Query().system_used == resource_baseline.system_used,
          "prepared thread cancellation clears publication and releases every guest allocation");
    prepared_thread.reset();

    Check(dispatch::ResetWouldBlockForTest() == false, "no callback lease remains at startup teardown");
    dispatch::Reset();
    Check(dispatch::ResolveTarget(kEntry).kind == dispatch::TargetKind::kUninitialized &&
#if defined(__APPLE__)
              kernel::CallGuestCallback(callback_context, start_ticket, start_ticket.target, callback_args, 0, 0,
                                        callback_result) == kernel::GuestCallbackStatus::kStaleDispatch,
#else
              start_ticket.dispatch_generation != 0,
#endif
          "dispatch reset invalidates every queued ticket before image teardown");
    Check(mem::FreeVirtual(image.base, image.image_size, mem::kMemRelease) == 0 &&
              mem::Decommit(kDataBase, mem::kPage) && mem::Decommit(kOutput, mem::kPage) &&
              mem::Decommit(kStackArguments - mem::kPage, 2 * mem::kPage),
          "startup-owned image and test data release before memory shutdown");
    Check(dispatch::SetKernel255FixtureBridge(false) && kernel::StopKernelWorkers() &&
              mem::Query().system_used == 0 && !kernel::KernelClockWorkerRunning(),
          "startup shutdown leaves no worker, callback gate, or kernel allocation");
    mem::Shutdown();
    kernel::g_base = nullptr;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: synthetic_startup_integration_tests checked_fixture.xbe\n");
        return 2;
    }
    std::vector<uint8_t> fixture_bytes;
    Check(ReadFile(argv[1], fixture_bytes), "generated synthetic XBE fixture reads");
    if (failures) return 1;

    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        (void)cycle;
        RunCycle(fixture_bytes);
        Check(!mem::IsInitialized(), "integration cycle shuts down guest memory");
    }
    return failures ? 1 : 0;
}
