#include <array>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <bit>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/thread_memory.h"
#include "x86_recomp_shared.h"

namespace kernel {
#ifndef MOSTWANTED_FIXTURE_EXTERNAL_G_BASE
uint8_t* g_base = nullptr;
#endif
}

namespace dispatch {
uint32_t HostThunk(X86Func* fn);
}

namespace {

struct FixtureStop {};
int failures = 0;
int faults = 0;
int traps = 0;
std::atomic<uint32_t> g_callback_fake_calls{0};
std::atomic<bool> g_callback_fake_stack_ok{false};
std::mutex g_callback_fake_lock;
std::condition_variable g_callback_fake_changed;
bool g_callback_fake_pause = false;
bool g_callback_fake_entered = false;
bool g_callback_fake_release = false;
size_t g_callback_page_mutation_waits = 0;
std::array<uint32_t, 17> g_callback_fake_stack{};
size_t g_callback_fake_stack_words = 0;
X86MemoryFault last_fault{};
uint32_t last_trap_pc = 0;
std::string last_trap_reason;

enum class Kernel255Mode { kAbiOnly, kPrepareThread, kBadCleanup, kBadReturnAddress };
Kernel255Mode g_kernel255_mode = Kernel255Mode::kAbiOnly;
std::array<uint32_t, 10> g_kernel255_args{};
std::array<kernel::ThreadRef, 8> g_prepared_threads{};
size_t g_prepared_thread_count = 0;
uint32_t g_kernel255_calls = 0;
constexpr uint32_t kKernel255Result = 0x13579BDF;

void Kernel255FixtureHandler(X86Context& ctx, uint8_t*) {
    ++g_kernel255_calls;
    if (mem::Read(ctx.esp + 4, g_kernel255_args.data(), sizeof(g_kernel255_args)) != mem::AccessStatus::kOk) {
        kernel::Return(ctx, kernel::kAccessViolation, 10);
        return;
    }
    if (g_kernel255_mode == Kernel255Mode::kBadCleanup) {
        ctx.eax = kKernel255Result;
        ctx.esp += 4;
        return;
    }
    if (g_kernel255_mode == Kernel255Mode::kBadReturnAddress) {
        kernel::Return(ctx, kKernel255Result, 10);
        const uint32_t invalid_return = 0xDEADBEEF;
        mem::Write(ctx.esp - 44, &invalid_return, sizeof(invalid_return));
        return;
    }
    if (g_kernel255_mode == Kernel255Mode::kPrepareThread) {
        kernel::ThreadRef prepared;
        const uint32_t status = kernel::PrepareSystemThreadEx(
            g_kernel255_args[0], g_kernel255_args[2] ? g_kernel255_args[2] : 0x10000, g_kernel255_args[3],
            g_kernel255_args[4], g_kernel255_args[5], g_kernel255_args[6], g_kernel255_args[7] & 0xFF,
            g_kernel255_args[9], prepared);
        if (status == kernel::kSuccess) {
            if (g_prepared_thread_count < g_prepared_threads.size())
                g_prepared_threads[g_prepared_thread_count++] = std::move(prepared);
            else {
                kernel::CancelPreparedThread(prepared);
                kernel::Return(ctx, kernel::kNoMemory, 10);
                return;
            }
        }
        kernel::Return(ctx, status, 10);
        return;
    }
    kernel::Return(ctx, kKernel255Result, 10);
}

__attribute__((noinline)) void LocalCopy(void* destination, const void* source, size_t size) {
    std::memcpy(destination, source, size);
}

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

void SyntheticGuestCallback(X86Context& ctx, uint8_t*) {
    ++g_callback_fake_calls;
    g_callback_fake_stack_ok.store(g_callback_fake_stack_words <= g_callback_fake_stack.size() &&
                                   mem::Read(ctx.esp, g_callback_fake_stack.data(),
                                             g_callback_fake_stack_words * sizeof(uint32_t)) == mem::AccessStatus::kOk);
    std::unique_lock lock(g_callback_fake_lock);
    if (g_callback_fake_pause) {
        g_callback_fake_entered = true;
        g_callback_fake_changed.notify_all();
        g_callback_fake_changed.wait(lock, [] { return g_callback_fake_release; });
    }
    ctx.eax = 0xAABBCCDD;
}

void CallbackPageMutationWaiting() {
    std::lock_guard lock(g_callback_fake_lock);
    ++g_callback_page_mutation_waits;
    g_callback_fake_changed.notify_all();
}

void CheckGuestCallbackContract() {
    constexpr uint32_t code_page = 0x90000;
    constexpr uint32_t target = code_page + 0x10;
    constexpr uint32_t stack_size = 0x4000;
    const uint32_t stack_top = mem::AllocStack(stack_size);
    const uint32_t stack_base = stack_top ? stack_top - stack_size : 0;
    Check(stack_top && mem::Commit(code_page, mem::kPage) &&
              mem::Protect(code_page, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk,
          "synthetic callback code and stack pages prepare");
    if (!stack_top || !mem::IsCommitted(code_page)) return;

    static const X86FuncMapping functions[] = {{target, SyntheticGuestCallback}};
    static const uint32_t continuations[] = {target + 8};
    const dispatch::Metadata metadata{code_page, code_page + mem::kPage, target, functions, continuations};
    std::string error;
    Check(dispatch::Init(metadata, error), "synthetic callback dispatch map initializes");
    Check(kernel::RestartKernelWorkers(), "callback test opens worker dispatch gate");

    kernel::GuestCallbackTicket ticket;
    Check(kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kFixture, ticket) ==
              kernel::GuestCallbackStatus::kReady,
          "mapped executable generated callback registers");
    kernel::GuestCallbackTicket rejected;
    Check(kernel::PrepareGuestCallback(target + 4, kernel::GuestCallbackClass::kFixture, rejected) ==
                  kernel::GuestCallbackStatus::kRejectedTarget &&
              kernel::PrepareGuestCallback(target + 8, kernel::GuestCallbackClass::kFixture, rejected) ==
                  kernel::GuestCallbackStatus::kRejectedTarget,
          "unmapped entry and continuation targets reject");

    X86Context ctx{};
    ctx.esp = stack_top - 16;
    constexpr std::array<uint32_t, 2> arguments{0x12345678, 0x87654321};
    g_callback_fake_stack_words = arguments.size() + 1;
    uint32_t result = 0;
    Check(kernel::CallGuestCallbackForTest(ctx, ticket, target + 4, arguments, stack_base, stack_size, result) ==
              kernel::GuestCallbackStatus::kTargetChanged,
          "queued target mutation rejects before invocation");
    Check(kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kInvoked &&
              result == 0xAABBCCDD && g_callback_fake_stack_ok.load() &&
              g_callback_fake_stack[0] == 0xFFFFFFF0 && g_callback_fake_stack[1] == arguments[0] &&
              g_callback_fake_stack[2] == arguments[1],
          "synthetic callback writes checked sentinel and arguments then invokes host substitute");
    auto invalid_class_ticket = ticket;
    invalid_class_ticket.callback_class = static_cast<kernel::GuestCallbackClass>(0xFF);
    const uint32_t calls_before_invalid_class = g_callback_fake_calls.load();
    Check(kernel::CallGuestCallbackForTest(ctx, invalid_class_ticket, target, arguments, stack_base, stack_size,
                                           result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              g_callback_fake_calls.load() == calls_before_invalid_class,
          "forged callback class rejects without indexing outside the contract table");

    constexpr uint32_t info_page = 0x05000000;
    constexpr uint32_t info = info_page + 0x800;
    const std::array<uint32_t, 3> info_words{7, 2, 1};
    kernel::GuestCallbackTicket vblank_ticket;
    Check(mem::Commit(info_page, mem::kPage) &&
              mem::Write(info, info_words.data(), sizeof(info_words)) == mem::AccessStatus::kOk &&
              kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kVBlank, vblank_ticket) ==
                  kernel::GuestCallbackStatus::kReady,
          "VBlank contract registers a mapped callback and bounded info record");
    const std::array<uint32_t, 1> info_argument{info};
    g_callback_fake_stack_words = 2;
    Check(kernel::CallGuestCallbackForTest(ctx, vblank_ticket, target, info_argument, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kInvoked &&
              g_callback_fake_stack[1] == info,
          "VBlank callback accepts only its readable 12-byte info span");
    const uint32_t calls_before_bad_info = g_callback_fake_calls.load();
    Check(mem::Protect(info_page, mem::kPage, mem::kWrite) == mem::AccessStatus::kOk &&
              kernel::CallGuestCallbackForTest(ctx, vblank_ticket, target, info_argument, stack_base, stack_size,
                                               result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              mem::Protect(info_page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk &&
              mem::Decommit(info_page, mem::kPage) &&
              kernel::CallGuestCallbackForTest(ctx, vblank_ticket, target, info_argument, stack_base, stack_size,
                                               result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              mem::Commit(info_page, mem::kPage) &&
              mem::Write(info, info_words.data(), sizeof(info_words)) == mem::AccessStatus::kOk &&
              g_callback_fake_calls.load() == calls_before_bad_info,
          "VBlank callback rejects unreadable or decommitted bounded info without dispatch");

    kernel::GuestCallbackTicket system_ticket, apc_ticket, timer_apc_ticket;
    const std::array<uint32_t, 2> system_arguments{target, 0};
    const std::array<uint32_t, 1> wrong_system_function{target + 8};
    const std::array<uint32_t, 3> opaque_apc_arguments{0xDEADCAFE, 0xFFFFFFFF, 0x80000000};
    const std::array<uint32_t, 3> timer_apc_arguments{0, 0xF1234567, 0xFFFFFFFF};
    Check(kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kSystemRoutine, system_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              kernel::CallGuestCallbackForTest(ctx, system_ticket, target, system_arguments, stack_base, stack_size,
                                               result) == kernel::GuestCallbackStatus::kInvoked &&
              kernel::CallGuestCallbackForTest(ctx, system_ticket, target, wrong_system_function, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kApc, apc_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              kernel::CallGuestCallbackForTest(ctx, apc_ticket, target, opaque_apc_arguments, stack_base, stack_size,
                                               result) == kernel::GuestCallbackStatus::kInvoked &&
              kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kTimerApc, timer_apc_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              kernel::CallGuestCallbackForTest(ctx, timer_apc_ticket, target, timer_apc_arguments, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kInvoked,
          "callback contracts validate guest functions and leave opaque APC values and timer scalars uninterpreted");
    Check(kernel::CallGuestCallbackForTest(ctx, apc_ticket, target, info_argument, stack_base, stack_size, result) ==
              kernel::GuestCallbackStatus::kArgumentRejected,
          "APC contract rejects an argument frame with the wrong arity");

    kernel::GuestCallbackTicket file_apc_ticket, dpc_ticket, timer_dpc_ticket, synchronize_ticket;
    const std::array<uint32_t, 3> file_apc_arguments{0xDEADCAFE, info, 0xFFFFFFFF};
    g_callback_fake_stack_words = 4;
    Check(kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kFileApc, file_apc_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              kernel::CallGuestCallbackForTest(ctx, file_apc_ticket, target, file_apc_arguments, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kInvoked,
          "file APC contract accepts its optional context, bounded IOSB, and reserved scalar");
    const uint32_t calls_before_bad_iosb = g_callback_fake_calls.load();
    Check(mem::Protect(info_page, mem::kPage, mem::kWrite) == mem::AccessStatus::kOk &&
              kernel::CallGuestCallbackForTest(ctx, file_apc_ticket, target, file_apc_arguments, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              mem::Protect(info_page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk &&
              mem::Decommit(info_page, mem::kPage) &&
              kernel::CallGuestCallbackForTest(ctx, file_apc_ticket, target, file_apc_arguments, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              mem::Commit(info_page, mem::kPage) &&
              mem::Write(info, info_words.data(), sizeof(info_words)) == mem::AccessStatus::kOk &&
              g_callback_fake_calls.load() == calls_before_bad_iosb,
          "file APC contract rejects IOSB protection changes and decommit before dispatch");

    constexpr uint32_t dpc_page = 0x05020000;
    constexpr uint32_t dpc = dpc_page + 0x80;
    const std::array<uint8_t, 0x1C> dpc_bytes{};
    const std::array<uint32_t, 4> dpc_arguments{dpc, 0xDEADCAFE, 0xFFFFFFFF, 0x80000000};
    const std::array<uint32_t, 4> timer_dpc_arguments{dpc, 0xF1234567, 0xFFFFFFFF, 0x80000000};
    g_callback_fake_stack_words = 5;
    Check(mem::Commit(dpc_page, mem::kPage) &&
              mem::Write(dpc, dpc_bytes.data(), dpc_bytes.size()) == mem::AccessStatus::kOk &&
              kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kDpc, dpc_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              kernel::CallGuestCallbackForTest(ctx, dpc_ticket, target, dpc_arguments, stack_base, stack_size,
                                               result) == kernel::GuestCallbackStatus::kInvoked &&
              kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kTimerDpc, timer_dpc_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              kernel::CallGuestCallbackForTest(ctx, timer_dpc_ticket, target, timer_dpc_arguments, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kInvoked,
          "DPC contracts accept bounded KDPC spans, unresolved contexts, and class-specific dword values");
    const uint32_t calls_before_bad_dpc = g_callback_fake_calls.load();
    Check(mem::Protect(dpc_page, mem::kPage, mem::kRead) == mem::AccessStatus::kOk &&
              kernel::CallGuestCallbackForTest(ctx, dpc_ticket, target, dpc_arguments, stack_base, stack_size,
                                               result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              mem::Protect(dpc_page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk &&
              mem::Decommit(dpc_page, mem::kPage) &&
              kernel::CallGuestCallbackForTest(ctx, timer_dpc_ticket, target, timer_dpc_arguments, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kArgumentRejected &&
              mem::Commit(dpc_page, mem::kPage) && g_callback_fake_calls.load() == calls_before_bad_dpc,
          "DPC contracts reject protection changes and decommitted KDPC records");

    const std::array<uint32_t, 1> synchronize_arguments{0xFFFFFFFF};
    g_callback_fake_stack_words = 2;
    Check(kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kSynchronizeExecution,
                                       synchronize_ticket) == kernel::GuestCallbackStatus::kReady &&
              kernel::CallGuestCallbackForTest(ctx, synchronize_ticket, target, synchronize_arguments, stack_base,
                                               stack_size, result) == kernel::GuestCallbackStatus::kInvoked,
          "synchronize-execution contract preserves its unresolved context value");

    constexpr uint32_t queued_iosb_page = 0x05010000;
    constexpr uint32_t queued_iosb = queued_iosb_page + 0x80;
    Check(mem::Commit(queued_iosb_page, mem::kPage) &&
              mem::Write(queued_iosb, info_words.data(), 8) == mem::AccessStatus::kOk,
          "file APC IOSB is committed before queueing");
    kernel::Thread file_apc_thread;
    file_apc_thread.ctx = &ctx;
    file_apc_thread.stack_top = stack_top;
    file_apc_thread.stack_size = stack_size;
    Check(kernel::QueueApc(file_apc_thread, target, 0, queued_iosb, 0, kernel::GuestCallbackClass::kFileApc) &&
              file_apc_thread.apcs.size() == 1,
          "file APC queue stores a bounded IOSB address without retaining a page lease");
    std::atomic<bool> iosb_decommitted{false};
    std::thread iosb_mutation([&] { iosb_decommitted = mem::Decommit(queued_iosb_page, mem::kPage); });
    const auto iosb_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!iosb_decommitted.load() && std::chrono::steady_clock::now() < iosb_deadline)
        std::this_thread::yield();
    const bool iosb_mutation_completed_while_queued = iosb_decommitted.load();
    if (!iosb_mutation_completed_while_queued) (void)kernel::RunApcs(file_apc_thread);
    iosb_mutation.join();
    const uint32_t calls_before_stale_file_apc = g_callback_fake_calls.load();
    const bool stale_file_apc_rejected = !kernel::RunApcs(file_apc_thread) &&
                                         g_callback_fake_calls.load() == calls_before_stale_file_apc;
    Check(iosb_mutation_completed_while_queued && stale_file_apc_rejected,
          "queued file APC does not block IOSB decommit and rejects the stale span at dispatch");

    Check(mem::Protect(code_page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk &&
              kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kCodePageDenied &&
              mem::Protect(code_page, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk,
          "queued callback rejects execute-protection change");
    Check(mem::Decommit(code_page, mem::kPage) &&
              kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kCodePageDenied &&
              mem::Commit(code_page, mem::kPage) &&
              mem::Protect(code_page, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk,
          "queued callback rejects target decommit before dispatch");

    const uint32_t calls_before_failures = g_callback_fake_calls.load();
    std::array<uint32_t, 17> too_many{};
    Check(kernel::CallGuestCallbackForTest(ctx, ticket, target, too_many, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kTooManyArguments &&
              kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, 8, result) ==
                  kernel::GuestCallbackStatus::kInvalidStack,
          "oversized argument list and invalid stack reject");
    const uint32_t stack_page = (ctx.esp - uint32_t((arguments.size() + 1) * sizeof(uint32_t))) & ~(mem::kPage - 1);
    Check(mem::Protect(stack_page, mem::kPage, mem::kRead) == mem::AccessStatus::kOk &&
              kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kStackWriteDenied &&
              mem::Protect(stack_page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk &&
              g_callback_fake_calls.load() == calls_before_failures,
          "stack write denial rejects without invoking callback");

    const uint64_t old_generation = ticket.dispatch_generation;
    Check(dispatch::Init(metadata, error), "callback mapping can be replaced after queueing");
    Check(kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kStaleDispatch &&
              ticket.dispatch_generation == old_generation,
          "queued callback ticket rejects a reset dispatch generation");
    Check(kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kDpc, ticket) ==
              kernel::GuestCallbackStatus::kReady,
          "callback ticket refreshes against replacement dispatch map");

    constexpr uint32_t timer_dpc = 0xA0000;
    constexpr uint32_t timer_dpc_context = 0xA1234;
    std::array<uint8_t, 0x1C> timer_dpc_bytes{};
    std::memcpy(timer_dpc_bytes.data() + 0x0C, &target, sizeof(target));
    std::memcpy(timer_dpc_bytes.data() + 0x10, &timer_dpc_context, sizeof(timer_dpc_context));
    const bool timer_dpc_ready = mem::Commit(timer_dpc, mem::kPage) &&
                                 mem::Write(timer_dpc, timer_dpc_bytes.data(), timer_dpc_bytes.size()) ==
                                     mem::AccessStatus::kOk;
    const kernel::GuestCallbackTicket stale_timer_ticket = ticket;
    Check(timer_dpc_ready && dispatch::Init(metadata, error),
          "timer DPC fixture is ready before dispatch-generation replacement");
    const bool stale_timer_queued = timer_dpc_ready &&
                                    kernel::QueueDpc(timer_dpc, 1, 2, &stale_timer_ticket, timer_dpc_context);
    uint8_t timer_dpc_inserted = 1;
    const bool timer_dpc_state_read =
        mem::Read(timer_dpc + 2, &timer_dpc_inserted, sizeof(timer_dpc_inserted)) == mem::AccessStatus::kOk;
    Check(!stale_timer_queued && timer_dpc_state_read && timer_dpc_inserted == 0,
          "timer DPC rejects a stale dispatch ticket before queueing");
    Check(kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kFixture, ticket) ==
              kernel::GuestCallbackStatus::kReady,
          "callback ticket refreshes after stale timer DPC rejection");

    {
        std::lock_guard lock(g_callback_fake_lock);
        g_callback_fake_pause = true;
        g_callback_fake_entered = false;
        g_callback_fake_release = false;
        g_callback_page_mutation_waits = 0;
    }
    mem::SetPageMutationWaitHookForTest(CallbackPageMutationWaiting);
    std::atomic<kernel::GuestCallbackStatus> active_status{kernel::GuestCallbackStatus::kCancelled};
    const uint64_t active_generation = vblank_ticket.dispatch_generation;
    Check(dispatch::Init(metadata, error) &&
              kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kVBlank, vblank_ticket) ==
                  kernel::GuestCallbackStatus::kReady &&
              vblank_ticket.dispatch_generation != active_generation,
          "bounded callback ticket refreshes for lease-concurrency test");
    g_callback_fake_stack_words = 2;
    std::thread active([&] {
        uint32_t active_result = 0;
        active_status = kernel::CallGuestCallbackForTest(ctx, vblank_ticket, target, info_argument, stack_base,
                                                         stack_size, active_result);
    });
    bool entered = false;
    {
        std::unique_lock lock(g_callback_fake_lock);
        entered = g_callback_fake_changed.wait_for(lock, std::chrono::seconds(3), [] { return g_callback_fake_entered; });
    }
    std::atomic<bool> reset_started{false}, reset_done{false};
    std::thread resetter([&] {
        reset_started = true;
        dispatch::Reset();
        reset_done = true;
    });
    std::thread stack_mutator([&] {
        mem::Protect(stack_base, mem::kPage, mem::kRead);
    });
    std::thread info_mutator([&] {
        mem::Protect(info_page, mem::kPage, mem::kNoAccess);
    });
    std::thread code_mutator([&] {
        mem::Decommit(code_page, mem::kPage);
    });
    while (!reset_started.load()) std::this_thread::yield();
    bool mutations_waited = false;
    {
        std::unique_lock lock(g_callback_fake_lock);
        mutations_waited = g_callback_fake_changed.wait_for(lock, std::chrono::seconds(3), [] {
            return g_callback_page_mutation_waits >= 3;
        });
    }
    Check(entered && mutations_waited && dispatch::ResetWouldBlockForTest() && !reset_done.load(),
          "active callback leases block code, stack, and bounded data mutation plus dispatch reset");
    {
        std::lock_guard lock(g_callback_fake_lock);
        g_callback_fake_release = true;
        g_callback_fake_pause = false;
    }
    g_callback_fake_changed.notify_all();
    active.join();
    resetter.join();
    stack_mutator.join();
    info_mutator.join();
    code_mutator.join();
    mem::SetPageMutationWaitHookForTest(nullptr);
    Check(reset_done.load() && active_status.load() == kernel::GuestCallbackStatus::kInvoked &&
              mem::Commit(code_page, mem::kPage) &&
              mem::Protect(code_page, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk &&
              mem::Protect(stack_base, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "callback return releases code, stack, and bounded data leases before page mutations finish");
    Check(mem::Protect(info_page, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk &&
              mem::Decommit(info_page, mem::kPage),
          "bounded callback fixture restores and releases its owned info page");
    Check(dispatch::Init(metadata, error) && kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kFixture, ticket) ==
                                                   kernel::GuestCallbackStatus::kReady,
          "dispatch map restores and fresh callback ticket registers");

    std::barrier start(2);
    std::atomic<kernel::GuestCallbackStatus> registration_status{kernel::GuestCallbackStatus::kCancelled};
    std::thread registrar([&] {
        start.arrive_and_wait();
        kernel::GuestCallbackTicket concurrent_ticket;
        registration_status = kernel::PrepareGuestCallback(target, kernel::GuestCallbackClass::kFixture,
                                                           concurrent_ticket);
    });
    start.arrive_and_wait();
    Check(kernel::StopKernelWorkers(), "shutdown closes callback admission gate");
    registrar.join();
    const uint32_t before_shutdown_dispatch = g_callback_fake_calls.load();
    Check((registration_status.load() == kernel::GuestCallbackStatus::kReady ||
           registration_status.load() == kernel::GuestCallbackStatus::kCancelled) &&
              kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size, result) ==
                  kernel::GuestCallbackStatus::kCancelled &&
              g_callback_fake_calls.load() == before_shutdown_dispatch,
          "concurrent registration cannot dispatch after shutdown");
    for (int cycle = 0; cycle < 3; ++cycle) {
        Check(kernel::RestartKernelWorkers() && kernel::StopKernelWorkers(),
              "callback worker gate completes repeated setup and teardown");
    }
    Check(kernel::RestartKernelWorkers(), "final callback shutdown test opens worker gate");

    std::atomic<bool> checked_boundary_done{false};
    std::atomic<kernel::GuestCallbackStatus> checked_boundary_status{kernel::GuestCallbackStatus::kCancelled};
    const uint32_t calls_before_disabled_boundary = g_callback_fake_calls.load();
    Check(kernel::StartManagedHostThread("callback-execution-gate-test", [&](X86Context& worker_ctx,
                                                                              kernel::WorkerStopToken) {
              worker_ctx.esp = stack_top - 16;
              uint32_t ignored = 0;
              checked_boundary_status =
                  kernel::CallGuestCallback(worker_ctx, ticket, target, arguments, stack_base, stack_size, ignored);
              checked_boundary_done = true;
          }), "managed synthetic worker reaches production callback gate");
    const auto boundary_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!checked_boundary_done.load() && std::chrono::steady_clock::now() < boundary_deadline)
        std::this_thread::yield();
    Check(checked_boundary_done.load() &&
              checked_boundary_status.load() == kernel::GuestCallbackStatus::kExecutionDisabled &&
              g_callback_fake_calls.load() == calls_before_disabled_boundary,
          "Apple production callback gate validates then rejects execution without invoking host substitute");
    Check(kernel::StopKernelWorkers() && kernel::RestartKernelWorkers(),
          "callback gate worker joins before the next lifecycle phase");

    {
        std::lock_guard lock(g_callback_fake_lock);
        g_callback_fake_pause = true;
        g_callback_fake_entered = false;
        g_callback_fake_release = false;
    }
    std::thread final_callback([&] {
        active_status = kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size, result);
    });
    {
        std::unique_lock lock(g_callback_fake_lock);
        entered = g_callback_fake_changed.wait_for(lock, std::chrono::seconds(3), [] { return g_callback_fake_entered; });
    }
    std::atomic<bool> memory_shutdown_done{false};
    std::thread memory_shutdown([&] {
        mem::Shutdown();
        memory_shutdown_done = true;
    });
    const auto shutdown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (kernel::KernelWorkersAccepting() && std::chrono::steady_clock::now() < shutdown_deadline)
        std::this_thread::yield();
    Check(entered && !kernel::KernelWorkersAccepting() && !memory_shutdown_done.load(),
          "memory teardown waits for active callback lifetime");
    {
        std::lock_guard lock(g_callback_fake_lock);
        g_callback_fake_release = true;
        g_callback_fake_pause = false;
    }
    g_callback_fake_changed.notify_all();
    final_callback.join();
    memory_shutdown.join();
    Check(memory_shutdown_done.load() && active_status.load() == kernel::GuestCallbackStatus::kInvoked,
          "memory teardown starts only after active synthetic callback exits");
}

void CheckGuestThreadLifecycle() {
    constexpr uint32_t code_page = 0x90000;
    constexpr uint32_t target = code_page + 0x10;
    constexpr uint32_t output_page = 0xA5000;
    static const X86FuncMapping functions[] = {{target, SyntheticGuestCallback}};
    const dispatch::Metadata metadata{code_page, code_page + mem::kPage, target, functions, {}};
    std::string error;
    auto initialize = [&] {
        const bool ready = mem::Init() && mem::Commit(code_page, mem::kPage) &&
                           mem::Protect(code_page, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk &&
                           mem::Commit(output_page, mem::kPage) && dispatch::Init(metadata, error);
        Check(ready, "thread lifecycle fixture memory and callback ticket mapping initialize");
        return ready;
    };
    auto prepare = [&](kernel::ThreadRef& thread, uint32_t handle_out = output_page,
                       uint32_t id_out = output_page + 4) {
        const uint32_t status = kernel::PrepareSystemThreadEx(handle_out, 0x10000, 0x20, id_out, target, 0, false, 0,
                                                              thread);
        Check(status == kernel::kSuccess && thread, "production thread preparation retains one synthetic owner");
        return status == kernel::kSuccess && bool(thread);
    };
    if (!initialize()) return;
    const mem::Stats baseline = mem::Query();

    kernel::ThreadRef registration_failure;
    if (!prepare(registration_failure)) return;
    kernel::SetThreadPreparationFailureForTest(kernel::ThreadPreparationFailPoint::kBeforeThreadRegistration);
    const bool registration_started = kernel::StartThread(registration_failure);
    Check(!registration_started && registration_failure->state.load() == kernel::ThreadState::kPrepared &&
              !registration_failure->start_routine_ticket.target && !registration_failure->system_routine_ticket.target &&
              kernel::Threads().empty() && kernel::KernelWorkerCountForTest() == 0,
          "failed thread registration restores prepared state and clears both callback tickets");
    const uint32_t registration_stack = registration_failure->stack_top;
    Check(kernel::CancelPreparedThread(registration_failure) &&
              !kernel::CancelPreparedThread(registration_failure) &&
              mem::Query().system_used == baseline.system_used && !mem::IsCommitted(registration_stack - 1),
          "prepared cancellation is exactly once and releases its stack and system allocations");

    kernel::ThreadRef launch_failure;
    if (!prepare(launch_failure)) return;
    kernel::FailNextKernelWorkerStartForTest();
    const bool launch_started = kernel::StartThread(launch_failure);
    Check(!launch_started && launch_failure->state.load() == kernel::ThreadState::kPrepared &&
              !launch_failure->start_routine_ticket.target && !launch_failure->system_routine_ticket.target &&
              kernel::Threads().empty() && kernel::KernelWorkerCountForTest() == 0,
          "failed worker launch rolls registration back without losing prepared ownership");
    Check(kernel::CancelPreparedThread(launch_failure) && mem::Query().system_used == baseline.system_used,
          "launch rollback remains cancellable and returns allocations to baseline");

#if defined(__APPLE__)
    std::size_t managed_stack_size = 0;
    const bool stack_started = kernel::StartManagedHostThread(
        "managed-stack-size-test", [&](X86Context&, kernel::WorkerStopToken) {
            managed_stack_size = pthread_get_stacksize_np(pthread_self());
        }, 64u << 20);
    const bool stack_joined = kernel::StopKernelWorkers() && kernel::RestartKernelWorkers();
    Check(stack_started && stack_joined && managed_stack_size >= (64u << 20) &&
              kernel::KernelWorkerCountForTest() == 0 && mem::Query().system_used == baseline.system_used,
          "managed lifecycle preserves the reviewed 64 MiB stack for deep guest call chains");
#endif

    std::mutex termination_lock;
    std::condition_variable termination_changed;
    kernel::ThreadRef terminated_thread;
    bool termination_entered = false;
    bool termination_stop_requested = false;
    std::atomic<bool> automatic_cleanup_ran{false};
    std::atomic<bool> termination_stop_callback_ran{false};
    struct CleanupProof {
        std::atomic<bool>& ran;
        ~CleanupProof() { ran.store(true); }
    };
    const bool termination_started = kernel::StartManagedHostThread(
        "controlled-thread-exit-test", [&](X86Context&, kernel::WorkerStopToken stop) {
            {
                std::lock_guard lock(termination_lock);
                terminated_thread = kernel::CurrentRef();
                termination_entered = true;
            }
            termination_changed.notify_all();
            kernel::WorkerStopCallback stop_callback(stop, [&] {
                {
                    std::lock_guard lock(termination_lock);
                    termination_stop_requested = true;
                }
                termination_stop_callback_ran.store(true);
                termination_changed.notify_all();
            });
            if (!stop_callback.Ready()) return;
            CleanupProof proof{automatic_cleanup_ran};
            {
                std::unique_lock lock(termination_lock);
                termination_changed.wait(lock, [&] { return termination_stop_requested; });
            }
            kernel::ExitThread(0x13579BDF);
        });
    bool termination_ready = false;
    {
        std::unique_lock lock(termination_lock);
        termination_ready = termination_changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return termination_entered;
        });
    }
    const bool termination_joined = kernel::StopKernelWorkers() && kernel::RestartKernelWorkers();
    const uint32_t termination_wait = termination_ready
                                          ? kernel::Wait({std::static_pointer_cast<kernel::Object>(terminated_thread)},
                                                         false, -100'000'000, false)
                                          : kernel::kTimeout;
    Check(termination_started && termination_ready && termination_stop_callback_ran.load() &&
              termination_wait == kernel::kWait0 && termination_joined && automatic_cleanup_ran.load() &&
              terminated_thread &&
              terminated_thread->state.load() == kernel::ThreadState::kExited &&
              terminated_thread->exit_code.load() == 0x13579BDF && terminated_thread->resources_released &&
              !terminated_thread->stack_top && !terminated_thread->kpcr && !terminated_thread->ethread &&
              mem::Query().system_used == baseline.system_used && kernel::KernelWorkerCountForTest() == 0,
          "managed ExitThread unwinds C++ scopes, preserves exit code, finalizes resources, and joins");

    kernel::ThreadRef duplicate_exit_thread;
    bool second_exit_caught = false;
    const bool duplicate_exit_started = kernel::StartManagedHostThread(
        "duplicate-thread-exit-test", [&](X86Context&, kernel::WorkerStopToken) {
            duplicate_exit_thread = kernel::CurrentRef();
            try {
                kernel::ExitThread(0x2468ACE0);
            } catch (...) {
            }
            try {
                kernel::ExitThread(0xFFFFFFFF);
            } catch (...) {
                second_exit_caught = true;
            }
        });
    const bool duplicate_exit_joined = kernel::StopKernelWorkers() && kernel::RestartKernelWorkers();
    Check(duplicate_exit_started && duplicate_exit_joined && second_exit_caught && duplicate_exit_thread &&
              duplicate_exit_thread->state.load() == kernel::ThreadState::kExited &&
              duplicate_exit_thread->exit_code.load() == 0x2468ACE0 && duplicate_exit_thread->resources_released &&
              kernel::KernelWorkerCountForTest() == 0 && mem::Query().system_used == baseline.system_used,
          "duplicate managed ExitThread preserves first status and releases thread resources once");

#if defined(__APPLE__)
    kernel::ThreadRef rejected_callback;
    const uint32_t callbacks_before_rejection = g_callback_fake_calls.load();
    if (!prepare(rejected_callback)) return;
    uint32_t rejected_handle = 0;
    mem::Read(output_page, &rejected_handle, sizeof(rejected_handle));
    const uint32_t rejected_ethread = rejected_callback->ethread;
    const uint32_t rejected_kpcr = rejected_callback->kpcr;
    const uint32_t rejected_stack = rejected_callback->stack_top;
    const bool rejected_started = kernel::StartThread(rejected_callback);
    const uint32_t rejected_wait = rejected_started
                                       ? kernel::Wait({std::static_pointer_cast<kernel::Object>(rejected_callback)},
                                                      false, -100'000'000, false)
                                       : kernel::kTimeout;
    const bool rejected_joined = kernel::StopKernelWorkers() && kernel::RestartKernelWorkers();
    const auto retained = kernel::FromHandle(rejected_handle);
    Check(rejected_started && rejected_wait == kernel::kWait0 && rejected_joined &&
              rejected_callback->state.load() == kernel::ThreadState::kCancelled &&
              retained.get() == rejected_callback.get() && rejected_callback->exit_code.load() == 0 &&
              rejected_callback->resources_released && !rejected_callback->ethread && !rejected_callback->kpcr &&
              !rejected_callback->stack_top && !rejected_callback->start_routine_ticket.target &&
              !rejected_callback->system_routine_ticket.target &&
              !kernel::IsPointerBoundForTest(rejected_ethread, rejected_callback.get()) &&
              !mem::AllocationSize(rejected_ethread) && !mem::AllocationSize(rejected_kpcr) &&
              !mem::IsCommitted(rejected_stack - 1) && kernel::KernelWorkerCountForTest() == 0 &&
              mem::Query().system_used == baseline.system_used &&
              g_callback_fake_calls.load() == callbacks_before_rejection,
          "Apple production callback rejection joins, frees guest blocks, and retains the signaled handle object");
    kernel::CloseHandle(rejected_handle);

    kernel::ThreadRef racing_thread;
    if (!prepare(racing_thread, 0, 0)) return;
    std::barrier race_start(3);
    bool race_started = false;
    bool race_cancelled = false;
    std::thread starter([&] {
        race_start.arrive_and_wait();
        race_started = kernel::StartThread(racing_thread);
    });
    std::thread canceller([&] {
        race_start.arrive_and_wait();
        race_cancelled = kernel::CancelPreparedThread(racing_thread);
    });
    race_start.arrive_and_wait();
    starter.join();
    canceller.join();
    bool race_joined = true;
    if (race_started) {
        const uint32_t wait = kernel::Wait({std::static_pointer_cast<kernel::Object>(racing_thread)}, false,
                                           -100'000'000, false);
        race_joined = wait == kernel::kWait0 && kernel::StopKernelWorkers() && kernel::RestartKernelWorkers();
    }
    Check(race_started != race_cancelled && race_joined && racing_thread->resources_released &&
              racing_thread->state.load() != kernel::ThreadState::kActive &&
              kernel::KernelWorkerCountForTest() == 0 &&
              g_callback_fake_calls.load() == callbacks_before_rejection,
          "StartThread and prepared cancellation serialize with one winner and no duplicate resource release");
#endif

    std::mutex teardown_lock;
    std::condition_variable teardown_changed;
    kernel::ThreadRef completing_thread;
    bool worker_entered = false;
    bool shutdown_requested = false;
    bool release_worker = false;
    bool shutdown_done = false;
    const bool completion_started = kernel::StartManagedHostThread(
        "completion-teardown-race-test", [&](X86Context&, kernel::WorkerStopToken stop) {
            {
                std::lock_guard lock(teardown_lock);
                completing_thread = kernel::CurrentRef();
                worker_entered = true;
            }
            teardown_changed.notify_all();
            kernel::WorkerStopCallback wake(stop, [&] {
                {
                    std::lock_guard lock(teardown_lock);
                    shutdown_requested = true;
                }
                teardown_changed.notify_all();
            });
            std::unique_lock lock(teardown_lock);
            teardown_changed.wait(lock, [&] { return release_worker; });
        });
    bool entered_for_teardown = false;
    {
        std::unique_lock lock(teardown_lock);
        entered_for_teardown = teardown_changed.wait_for(lock, std::chrono::seconds(5), [&] { return worker_entered; });
    }
    std::thread shutdown([&] {
        mem::Shutdown();
        {
            std::lock_guard lock(teardown_lock);
            shutdown_done = true;
        }
        teardown_changed.notify_all();
    });
    bool stop_seen = false;
    bool teardown_waited_for_worker = false;
    {
        std::unique_lock lock(teardown_lock);
        stop_seen = teardown_changed.wait_for(lock, std::chrono::seconds(5), [&] { return shutdown_requested; });
        teardown_waited_for_worker = stop_seen && !shutdown_done && mem::IsInitialized();
        release_worker = true;
    }
    teardown_changed.notify_all();
    shutdown.join();
    Check(completion_started && entered_for_teardown && stop_seen && teardown_waited_for_worker && shutdown_done &&
              completing_thread &&
              completing_thread->state.load() == kernel::ThreadState::kExited &&
              completing_thread->resources_released && !completing_thread->stack_top &&
              kernel::KernelWorkerCountForTest() == 0 && !mem::IsInitialized(),
          "host teardown waits for worker completion before releasing guest memory");

    if (!initialize()) return;
    kernel::ThreadRef stale_prepared;
    if (!prepare(stale_prepared)) return;
    const uint64_t old_generation = stale_prepared->memory_generation;
    const uint32_t old_ethread = stale_prepared->ethread;
    const uint32_t old_kpcr = stale_prepared->kpcr;
    const uint32_t old_stack = stale_prepared->stack_top;
    const uint32_t old_handle = stale_prepared->published_handle;
    mem::Shutdown();
    Check(stale_prepared->state.load() == kernel::ThreadState::kCancelled &&
              stale_prepared->resources_released && !stale_prepared->ethread && !stale_prepared->kpcr &&
              !stale_prepared->stack_top && !stale_prepared->start_routine_ticket.target &&
              !kernel::FromHandle(old_handle) && kernel::KernelWorkerCountForTest() == 0,
          "memory shutdown finalizes a prepared owner before allocator reset");

    if (!initialize()) return;
    kernel::ThreadRef reused_prepared;
    if (!prepare(reused_prepared)) return;
    uint32_t reused_handle = 0, reused_id = 0;
    mem::Read(output_page, &reused_handle, sizeof(reused_handle));
    mem::Read(output_page + 4, &reused_id, sizeof(reused_id));
    Check(reused_prepared->memory_generation != old_generation && reused_prepared->ethread == old_ethread &&
              reused_prepared->kpcr == old_kpcr && reused_prepared->stack_top == old_stack &&
              !kernel::CancelPreparedThread(stale_prepared) &&
              kernel::FromHandle(reused_handle).get() == reused_prepared.get() &&
              kernel::FromPointer(reused_prepared->ethread).get() == reused_prepared.get() &&
              reused_id == reused_prepared->id &&
              mem::AllocationSize(old_ethread) != 0 && mem::AllocationSize(old_kpcr) != 0 &&
              mem::IsCommitted(old_stack - 1),
          "stale ThreadRef cannot release or overwrite allocations reused by a newer memory generation");
    Check(kernel::CancelPreparedThread(reused_prepared) && mem::Query().system_used ==
                                                           (baseline.system_used),
          "reused prepared thread cancellation restores current-generation allocations");

#if defined(__APPLE__)
    kernel::HoldKernelWorkerActivationForTest(true);
    kernel::ThreadRef shutdown_prepared;
    if (!prepare(shutdown_prepared)) return;
    uint32_t shutdown_handle = 0;
    mem::Read(output_page, &shutdown_handle, sizeof(shutdown_handle));
    const uint32_t callbacks_before_shutdown = g_callback_fake_calls.load();
    const uint32_t active_ethread = shutdown_prepared->ethread;
    const uint32_t active_kpcr = shutdown_prepared->kpcr;
    const uint32_t active_stack = shutdown_prepared->stack_top;
    const bool shutdown_started = kernel::StartThread(shutdown_prepared);
    const bool closed_while_active = kernel::CloseHandle(shutdown_handle);
    const bool guest_resources_retained = shutdown_started && closed_while_active &&
                                          shutdown_prepared->state.load() == kernel::ThreadState::kActive &&
                                          !shutdown_prepared->resources_released &&
                                          kernel::KernelWorkerCountForTest() == 1 &&
                                          mem::AllocationSize(active_ethread) != 0 &&
                                          mem::AllocationSize(active_kpcr) != 0 &&
                                          mem::IsCommitted(active_stack - 1);
    mem::Shutdown();
    kernel::HoldKernelWorkerActivationForTest(false);
    Check(shutdown_started && closed_while_active && guest_resources_retained &&
              shutdown_prepared->state.load() == kernel::ThreadState::kCancelled &&
              shutdown_prepared->resources_released && !shutdown_prepared->ethread &&
              !shutdown_prepared->stack_top && kernel::KernelWorkerCountForTest() == 0 &&
              g_callback_fake_calls.load() == callbacks_before_shutdown,
          "shutdown cancels one activation-held worker before dispatch and joins it after handle close");
#else
    mem::Shutdown();
#endif
}

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE_CLI
void RunReleaseBenchmarks(uint32_t source, uint32_t destination) {
    constexpr int scalar_rounds = 200000;
    X86Context ctx{};
    ctx.ebx = source;
    ctx.edi = destination;
    auto ns_per_call = [&](X86Func* function) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < scalar_rounds; ++i) {
            ctx.esp = 0x1000;
            function(ctx, nullptr);
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
               scalar_rounds;
    };
    const double empty_ns = ns_per_call(sub_0001103F);
    const double scalar_ns = ns_per_call(sub_00011045);
    std::printf("release fixture benchmark: no-memory generated call %.1f ns; generated checked scalar load+store %.1f ns "
                "(delta %.1f ns/call; %.2f M checked ops/s)\n",
                empty_ns, scalar_ns, scalar_ns - empty_ns, 2.0 * scalar_rounds / (scalar_ns * scalar_rounds) * 1e9 / 1e6);

    constexpr uint32_t bulk_source = 0x40000, bulk_destination = 0x50000;
    constexpr int bulk_rounds = 10000;
    constexpr size_t bulk_size = 0x1000;
    if (!mem::Commit(bulk_source, 2 * mem::kPage) || !mem::Commit(bulk_destination, 2 * mem::kPage)) {
        Check(false, "release bulk benchmark pages commit");
        return;
    }
    std::array<uint8_t, bulk_size> local_source{}, local_destination{};
    local_source.fill(0x5A);
    bool checked_ok = mem::Write(bulk_source, local_source.data(), bulk_size) == mem::AccessStatus::kOk;
    volatile uint64_t local_checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < bulk_rounds; ++i) {
        LocalCopy(local_destination.data(), local_source.data(), bulk_size);
        local_checksum += local_destination[0];
    }
    const double local_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < bulk_rounds; ++i) {
        checked_ok &= mem::Copy(bulk_destination, bulk_source, bulk_size) == mem::AccessStatus::kOk;
    }
    const double checked_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double bytes = double(bulk_rounds) * bulk_size;
    std::printf("release bulk benchmark: checked mem::Copy %.2f GiB/s; local-buffer memcpy proxy %.2f GiB/s "
                "(proxy is not direct guest memory)\n",
                bytes / checked_seconds / (1024.0 * 1024.0 * 1024.0),
                bytes / local_seconds / (1024.0 * 1024.0 * 1024.0));
    Check(checked_ok && local_checksum == uint64_t(bulk_rounds) * local_source[0] &&
              mem::Read(bulk_destination, local_destination.data(), bulk_size) == mem::AccessStatus::kOk &&
              local_destination == local_source,
          "release checked bulk benchmark transfers all requested bytes");
}
#endif

bool ThrowsMemoryFault(X86Func* function, X86Context& ctx) {
    faults = 0;
    try {
        function(ctx, nullptr);
    } catch (const FixtureStop&) {
        return faults == 1;
    }
    return false;
}

bool ThrowsTrap(X86Func* function, X86Context& ctx) {
    traps = 0;
    try {
        function(ctx, nullptr);
    } catch (const FixtureStop&) {
        return traps == 1;
    }
    return false;
}

struct SubFlags {
    uint32_t cf, pf, af, zf, sf, of;
};

uint64_t WidthMask(unsigned width) { return width == 32 ? UINT32_MAX : (uint64_t(1) << width) - 1; }

int64_t SignedValue(uint32_t value, unsigned width) {
    const uint64_t mask = WidthMask(width), sign = uint64_t(1) << (width - 1);
    value &= uint32_t(mask);
    return value & sign ? int64_t(value) - int64_t(uint64_t(1) << width) : value;
}

SubFlags ReferenceSubFlags(unsigned width, uint32_t left, uint32_t right) {
    const uint64_t mask = WidthMask(width);
    const uint64_t a = left & mask, b = right & mask, result = (a - b) & mask;
    const int64_t difference = SignedValue(uint32_t(a), width) - SignedValue(uint32_t(b), width);
    const int64_t minimum = -(int64_t(1) << (width - 1));
    const int64_t maximum = (int64_t(1) << (width - 1)) - 1;
    return {uint32_t(a < b), uint32_t(std::popcount(uint8_t(result)) % 2 == 0), uint32_t((a & 0xF) < (b & 0xF)),
            uint32_t(result == 0), uint32_t((result >> (width - 1)) & 1), uint32_t(difference < minimum || difference > maximum)};
}

bool Runs(X86Func* function, X86Context& ctx) {
    try {
        function(ctx, nullptr);
        return true;
    } catch (...) {
        return false;
    }
}

bool MatchesFlags(const X86Context& ctx, const SubFlags& expected) {
    return ctx.cf == expected.cf && ctx.pf == expected.pf && ctx.af == expected.af && ctx.zf == expected.zf &&
           ctx.sf == expected.sf && ctx.of == expected.of;
}

bool SeedCmpFlags(X86Context& ctx) {
    ctx.cf = 1;
    ctx.pf = 0;
    ctx.af = 1;
    ctx.zf = 0;
    ctx.sf = 1;
    ctx.of = 0;
    return true;
}

void CheckCmpTests(uint32_t destination) {
    struct Case {
        X86Func* function;
        unsigned width;
        uint32_t left, right;
        const char* name;
    };
    const Case cases[] = {
        {sub_00011054, 8, 0x80, 1, "8-bit negative CMP flags match independent subtraction reference"},
        {sub_00011054, 8, 0x7F, 0xFF, "8-bit signed overflow and parity flags match reference"},
        {sub_00011054, 8, 0, 1, "8-bit carry and half-carry flags match reference"},
        {sub_00011054, 8, 0x55, 0x55, "8-bit zero and parity flags match reference"},
        {sub_00011057, 16, 0x8000, 1, "16-bit negative CMP flags match independent subtraction reference"},
        {sub_00011057, 16, 0x7FFF, 0xFFFF, "16-bit signed overflow flags match reference"},
        {sub_00011057, 16, 0x10, 1, "16-bit half-carry flags match reference"},
        {sub_00011051, 32, 0x80000000, 1, "32-bit negative CMP flags match independent subtraction reference"},
        {sub_00011051, 32, 0x7FFFFFFF, 0xFFFFFFFF, "32-bit signed overflow flags match reference"},
        {sub_00011051, 32, 0, 1, "32-bit carry and half-carry flags match reference"},
        {sub_00011051, 32, 0x101, 1, "32-bit parity uses low result byte"},
        {sub_00011051, 32, 9, 9, "32-bit zero comparison flags match reference"},
    };
    for (const Case& test : cases) {
        X86Context ctx{};
        ctx.eax = test.width == 32 ? test.left : test.left | 0xA5A50000;
        ctx.ebx = test.width == 32 ? test.right : test.right | 0x5A5A0000;
        const uint32_t eax_before = ctx.eax, ebx_before = ctx.ebx;
        SeedCmpFlags(ctx);
        const SubFlags expected = ReferenceSubFlags(test.width, test.left, test.right);
        Check(Runs(test.function, ctx) && ctx.eax == eax_before && ctx.ebx == ebx_before && MatchesFlags(ctx, expected), test.name);
    }

    X86Context ctx{};
    ctx.eax = 0x7FFFFFFF;
    const SubFlags immediate32 = ReferenceSubFlags(32, ctx.eax, 0x80000000);
    SeedCmpFlags(ctx);
    Check(Runs(sub_0001105B, ctx) && ctx.eax == 0x7FFFFFFF && MatchesFlags(ctx, immediate32),
          "32-bit immediate CMP uses destination operand width");
    ctx = X86Context{};
    ctx.eax = UINT32_MAX;
    const SubFlags signed_imm8 = ReferenceSubFlags(32, ctx.eax, UINT32_MAX);
    Check(Runs(sub_00011061, ctx) && ctx.eax == UINT32_MAX && MatchesFlags(ctx, signed_imm8),
          "sign-extended imm8 CMP compares against -1 at 32-bit width");

    constexpr uint32_t unaligned = 0x30001;
    uint32_t memory_value = 0x44332280;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "CMP unaligned memory fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0x4433227F;
    ctx.esi = unaligned;
    const SubFlags memory_flags = ReferenceSubFlags(32, ctx.eax, memory_value);
    Check(Runs(sub_00011068, ctx) && ctx.eax == 0x4433227F && MatchesFlags(ctx, memory_flags),
          "CMP register-memory uses checked unaligned dword read and exact arithmetic flags");

    const uint32_t memory_dest = destination + 0x100;
    memory_value = 0xABCDEF01;
    Check(mem::Write(memory_dest, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "CMP memory destination fixture initializes");
    ctx = X86Context{};
    ctx.eax = memory_value - 1;
    ctx.esi = memory_dest;
    const SubFlags destination_flags = ReferenceSubFlags(32, memory_value, ctx.eax);
    uint32_t register_left = ctx.eax;
    Check(Runs(sub_00011065, ctx) && ctx.eax == register_left &&
              mem::Read(memory_dest, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk &&
              memory_value == 0xABCDEF01 && MatchesFlags(ctx, destination_flags),
          "CMP memory-register performs read-only operand access without destination write");

    uint8_t byte_value = 0x7F;
    Check(mem::Write(unaligned, &byte_value, sizeof(byte_value)) == mem::AccessStatus::kOk,
          "byte CMP fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0xA5A50080;
    ctx.esi = unaligned;
    Check(Runs(sub_0001106B, ctx) && ctx.eax == 0xA5A50080 && MatchesFlags(ctx, ReferenceSubFlags(8, 0x7F, 0x80)),
          "CMP byte memory operand uses 8-bit flags and preserves upper register bits");

    uint16_t word_value = 0x8001;
    Check(mem::Write(unaligned, &word_value, sizeof(word_value)) == mem::AccessStatus::kOk,
          "word CMP fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0x12348000;
    ctx.esi = unaligned;
    Check(Runs(sub_0001106E, ctx) && ctx.eax == 0x12348000 && MatchesFlags(ctx, ReferenceSubFlags(16, 0x8001, 0x8000)),
          "CMP word memory operand uses 16-bit flags and preserves upper register bits");

    const uint32_t scaled = 0x30204;
    memory_value = 0x11223344;
    Check(mem::Write(scaled, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "scaled CMP address fixture initializes");
    ctx = X86Context{};
    ctx.ebx = 0x30200;
    ctx.esi = 2;
    ctx.eax = memory_value + 1;
    Check(Runs(sub_00011072, ctx) && MatchesFlags(ctx, ReferenceSubFlags(32, memory_value, memory_value + 1)),
          "CMP SIB address applies scale, base, and signed displacement");

    memory_value = UINT32_MAX;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "memory-immediate CMP fixture initializes");
    ctx = X86Context{};
    ctx.esi = unaligned;
    Check(Runs(sub_00011077, ctx) && MatchesFlags(ctx, ReferenceSubFlags(32, UINT32_MAX, UINT32_MAX)),
          "memory CMP sign-extends imm8 without writing operand");

    memory_value = 3;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "CMP branch fixture initializes");
    ctx = X86Context{};
    ctx.eax = 4;
    ctx.esi = unaligned;
    Check(Runs(sub_0001107B, ctx) && ctx.eax == 2,
          "CMP feeds correct unsigned not-below conditional branch");
    ctx = X86Context{};
    ctx.eax = 2;
    ctx.esi = unaligned;
    Check(Runs(sub_0001107B, ctx) && ctx.eax == 1,
          "CMP carry feeds opposite unsigned conditional branch");
    memory_value = 0;
    mem::Write(unaligned, &memory_value, sizeof(memory_value));
    ctx = X86Context{};
    ctx.esi = unaligned;
    Check(Runs(sub_0001108B, ctx) && ctx.eax == 2 && ctx.pf == 1,
          "CMP parity flag drives subsequent JP branch");
    ctx = X86Context{};
    ctx.eax = 1;
    ctx.esi = unaligned;
    Check(Runs(sub_0001108B, ctx) && ctx.eax == 1 && ctx.pf == 0,
          "CMP odd-result parity skips subsequent JP branch");

    memory_value = 2;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "signed CMP branch fixture initializes");
    ctx = X86Context{};
    ctx.eax = 1;
    ctx.esi = unaligned;
    Check(Runs(sub_0001109B, ctx) && ctx.eax == 2,
          "CMP signed less-than branch uses SF and OF for negative result");
    memory_value = 0x80000000;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "signed-overflow CMP branch fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0x7FFFFFFF;
    ctx.esi = unaligned;
    Check(Runs(sub_0001109B, ctx) && ctx.eax == 1,
          "CMP signed less-than branch rejects negative wrapped result when OF is set");

    const auto check_fault_unchanged = [&](uint32_t base, uint32_t index, uint32_t address, uint32_t pc,
                                           mem::AccessStatus status, const char* name, X86Func* function) {
        X86Context fault_ctx{};
        fault_ctx.eax = 0xDEADBEEF;
        fault_ctx.ebx = base;
        fault_ctx.esi = index;
        fault_ctx.esp = 0x12345678;
        fault_ctx.cf = 1; fault_ctx.pf = 0; fault_ctx.af = 1; fault_ctx.zf = 0; fault_ctx.sf = 1; fault_ctx.of = 0;
        const X86Context before = fault_ctx;
        Check(ThrowsMemoryFault(function, fault_ctx) && last_fault.instruction == pc && last_fault.address == address &&
                  last_fault.status == status && !last_fault.write && fault_ctx.eax == before.eax &&
                  fault_ctx.ebx == before.ebx && fault_ctx.esi == before.esi && fault_ctx.esp == before.esp &&
                  fault_ctx.cf == before.cf && fault_ctx.pf == before.pf && fault_ctx.af == before.af &&
                  fault_ctx.zf == before.zf && fault_ctx.sf == before.sf && fault_ctx.of == before.of,
              name);
    };
    check_fault_unchanged(0, 0x24000, 0x24000, 0x11068, mem::AccessStatus::kUncommitted,
                          "CMP uncommitted read preserves all registers and arithmetic flags", sub_00011068);
    check_fault_unchanged(0, 0x21FFF, 0x22000, 0x11068, mem::AccessStatus::kUncommitted,
                          "CMP cross-page read reports first uncommitted byte and fault PC", sub_00011068);
    Check(mem::Commit(0x40000, mem::kPage) && mem::Protect(0x40000, mem::kPage, mem::kWrite) == mem::AccessStatus::kOk,
          "CMP denied-read fixture page becomes write-only");
    check_fault_unchanged(0, 0x40000, 0x40000, 0x11068, mem::AccessStatus::kPermissionDenied,
                          "CMP permission denial preserves registers and flags", sub_00011068);
    check_fault_unchanged(3, 0, UINT32_MAX, 0x11072, mem::AccessStatus::kInvalidAddress,
                          "CMP SIB address overflow reports wrapped guest address", sub_00011072);

    X86Context denied{};
    denied.esi = 0x40000;
    Check(ThrowsMemoryFault(sub_00011068, denied) && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.instruction == 0x11068 && last_fault.address == 0x40000 && !last_fault.write,
          "CMP reports exact memory-read permission fault");
    Check(mem::Protect(0x40000, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "CMP permission test restores guest page");
}

struct AndFlags {
    uint32_t cf, zf, sf, of, pf;
};

AndFlags ReferenceAndFlags(uint32_t left, uint32_t right, unsigned width) {
    const uint32_t mask = width == 32 ? UINT32_MAX : (1u << width) - 1;
    const uint32_t result = (left & right) & mask;
    return {0, uint32_t(result == 0), (result >> (width - 1)) & 1, 0,
            uint32_t(std::popcount(result & 0xFF) % 2 == 0)};
}

bool MatchesAndFlags(const X86Context& ctx, const AndFlags& flags) {
    return ctx.cf == flags.cf && ctx.zf == flags.zf && ctx.sf == flags.sf && ctx.of == flags.of &&
           ctx.pf == flags.pf;
}

bool SameCoreState(const X86Context& left, const X86Context& right) {
    return left.eax == right.eax && left.ecx == right.ecx && left.edx == right.edx && left.ebx == right.ebx &&
           left.esp == right.esp && left.ebp == right.ebp && left.esi == right.esi && left.edi == right.edi &&
           left.cf == right.cf && left.zf == right.zf && left.sf == right.sf && left.of == right.of &&
           left.pf == right.pf && left.af == right.af && left.df == right.df && left.xflags == right.xflags;
}

void SeedFlags(X86Context& ctx);

void CheckCallTests(uint32_t destination) {
    constexpr uint32_t stack = 0x74000, top = stack + mem::kPage;
    Check(mem::Commit(stack, mem::kPage), "CALL fixture stack page commits");

    X86Context ctx{};
    ctx.esp = top;
    SeedFlags(ctx);
    Check(Runs(sub_000110F5, ctx) && ctx.eax == 0x12345678 && ctx.ebx == 0x55667788 && ctx.esp == top + 4 &&
              ctx.last_icall == 0x110EF && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
              ctx.af == 1,
          "direct CALL dispatches generated target and returns to guest continuation");
    uint32_t return_address = 0;
    Check(mem::Read(top - 4, &return_address, sizeof(return_address)) == mem::AccessStatus::kOk &&
              return_address == 0x110FA,
          "direct CALL stores exact guest return address at ESP minus four");

    ctx = X86Context{};
    ctx.esp = top;
    Check(Runs(sub_0001110D, ctx) && ctx.eax == 3 && ctx.ebx == 0x44332211 && ctx.esp == top + 4 && ctx.cf == 0 &&
              ctx.zf == 0 && ctx.sf == 0 && ctx.of == 0 && ctx.pf == 1 && ctx.af == 0,
          "nested direct CALLs return in LIFO order through generated continuations");
    uint32_t returns[2]{};
    Check(mem::Read(top - 8, returns, sizeof(returns)) == mem::AccessStatus::kOk && returns[0] == 0x11109 &&
              returns[1] == 0x11117,
          "nested CALLs preserve inner and outer guest return addresses");

    ctx = X86Context{};
    ctx.esp = top;
    ctx.edi = destination;
    SeedFlags(ctx);
    Check(Runs(sub_00011120, ctx) && ctx.esp == top + 12,
          "checked RET imm16 applies guest callee stack cleanup");
    uint32_t post_call_esp = 0;
    Check(mem::Read(destination, &post_call_esp, sizeof(post_call_esp)) == mem::AccessStatus::kOk &&
              post_call_esp == top + 8 && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
              ctx.af == 1,
          "RET imm16 continuation observes ESP after return address and immediate cleanup");

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_00011130, ctx) && last_trap_pc == 0x1112F &&
              last_trap_reason.find("return address") != std::string::npos && ctx.esp == top - 4,
          "checked RET traps corrupted guest return address before changing ESP");

    ctx = X86Context{};
    ctx.esp = 0x78000;
    const X86Context before_write_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110F5, ctx) && last_fault.instruction == 0x110F5 &&
              last_fault.address == 0x77FFC && last_fault.status == mem::AccessStatus::kUncommitted && last_fault.write &&
              SameCoreState(ctx, before_write_fault),
          "CALL stack write fault reports CALL PC and leaves ESP unchanged");

    constexpr uint32_t read_only_stack = 0x76000;
    Check(mem::Commit(read_only_stack, mem::kPage) &&
              mem::Protect(read_only_stack, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "CALL denied-write fixture page becomes read-only");
    ctx = X86Context{};
    ctx.esp = read_only_stack + mem::kPage;
    const X86Context before_denied_write = ctx;
    Check(ThrowsMemoryFault(sub_000110F5, ctx) && last_fault.instruction == 0x110F5 &&
              last_fault.address == ctx.esp - 4 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.write && SameCoreState(ctx, before_denied_write),
          "CALL denied stack write preserves ESP and reports permission failure");
    Check(mem::Protect(read_only_stack, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "CALL denied-write fixture page becomes writable again");

    ctx = X86Context{};
    ctx.esp = top;
    ctx.ebx = 0x78000;
    Check(ThrowsMemoryFault(sub_0001114E, ctx) && last_fault.instruction == 0x1114D &&
              last_fault.address == 0x78000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && ctx.esp == 0x78000 && ctx.checked_call == nullptr,
          "RET stack read fault reports RET PC and address without advancing ESP");

    ctx = X86Context{};
    ctx.esp = top;
    const uint32_t no_args = 0;
    const uint32_t host_result = kernel::CallGuest(ctx, 0x110EF, &no_args, 0);
    Check(host_result == 0x12345678 && ctx.esp == top && ctx.checked_call == nullptr &&
              mem::Read(top - 4, &return_address, sizeof(return_address)) == mem::AccessStatus::kOk &&
              return_address == 0xFFFFFFF0,
          "host CallGuest validates its explicit sentinel return without a guest continuation");

    X86CheckedCallFrame outer_frame{nullptr, 0xCAFEBABE, false};
    ctx = X86Context{};
    ctx.esp = top;
    ctx.checked_call = &outer_frame;
    const uint32_t nested_host_result = kernel::CallGuest(ctx, 0x110EF, &no_args, 0);
    Check(nested_host_result == 0x12345678 && ctx.esp == top && ctx.checked_call == &outer_frame &&
              !outer_frame.returned,
          "host CallGuest sentinel frame shadows and restores an enclosing checked CALL frame");

    ctx = X86Context{};
    ctx.esp = top;
    ctx.checked_call = &outer_frame;
    traps = 0;
    try {
        kernel::CallGuest(ctx, 0x11128, &no_args, 0);
    } catch (const FixtureStop&) {
    }
    Check(traps == 1 && last_trap_pc == 0x1112F &&
              last_trap_reason.find("expected FFFFFFF0, found DEADBEEF") != std::string::npos &&
              ctx.esp == top - 4 && ctx.checked_call == &outer_frame && !outer_frame.returned,
          "host CallGuest rejects a corrupted sentinel and restores the enclosing checked frame");
    ctx.checked_call = nullptr;

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_0001113C, ctx) && last_trap_pc == 0x1113C &&
              last_trap_reason.find("unimplemented: call") != std::string::npos && ctx.esp == top,
          "indirect CALL remains trapped without changing guest stack");

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_0001113F, ctx) && last_trap_pc == 0x1113F &&
              last_trap_reason.find("unimplemented: call") != std::string::npos && ctx.esp == top,
          "direct CALL to a target absent from generated metadata remains trapped");

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_00011145, ctx) && last_trap_pc == 0x11145 &&
              last_trap_reason.find("unimplemented: call") != std::string::npos && ctx.esp == top,
          "CALL $+5 remains trapped because fallthrough is not an audited target");

    ctx = X86Context{};
    ctx.esp = top;
    traps = 0;
    try {
        x86_checked_call(ctx, nullptr, 0x11001, 0x22222222, 0x1113F);
    } catch (const FixtureStop&) {
    }
    Check(traps == 1 && last_trap_pc == 0x1113F && ctx.esp == top &&
              last_trap_reason.find("unmapped") != std::string::npos,
          "runtime checked CALL rejects a target missing from the dispatch table before stack write");

    const uint32_t continuation = 0x11136;
    const dispatch::Metadata metadata{X86CodeBase, X86CodeEnd, X86EntryPoint,
                                      std::span<const X86FuncMapping>(X86FuncMappings, X86FuncMappingCount),
                                      std::span<const uint32_t>(&continuation, 1)};
    std::string error;
    Check(dispatch::Init(metadata, error), "dispatch table accepts generated fixture continuation metadata");

    X86CheckedCallFrame resumed_frame{nullptr, 0x22222222, false};
    ctx = X86Context{};
    ctx.esp = top - 4;
    ctx.checked_call = &resumed_frame;
    return_address = resumed_frame.return_address;
    Check(mem::Write(ctx.esp, &return_address, sizeof(return_address)) == mem::AccessStatus::kOk,
          "valid-resume stack return address initializes");
    x86_resume(ctx, nullptr, X86Resume{continuation, ctx.esp});
    Check(ctx.ecx == 0x12345678 && ctx.esp == top && ctx.checked_call == nullptr && resumed_frame.returned,
          "valid X86Resume continuation dispatches generated code and validates its RET");

    ctx = X86Context{};
    ctx.esp = top;
    traps = 0;
    bool continuation_rejected = false;
    try {
        x86_checked_call(ctx, nullptr, continuation, 0x22222222, 0x11145);
    } catch (const FixtureStop&) {
        continuation_rejected = true;
    } catch (...) {
    }
    Check(continuation_rejected && traps == 1 && ctx.esp == top && ctx.checked_call == nullptr &&
              last_trap_reason.find("continuation") != std::string::npos,
          "direct CALL rejects a generated continuation before changing the guest stack");

    Check(dispatch::Init(error), "dispatch table restores generated fixture metadata after continuation test");
}

void SeedFlags(X86Context& ctx) {
    ctx.cf = 1;
    ctx.zf = 0;
    ctx.sf = 1;
    ctx.of = 1;
    ctx.pf = 0;
    ctx.af = 1;
}

void CheckIndirectCallTests() {
    constexpr uint32_t slots = 0x12000, stack = 0x74000, top = stack + mem::kPage;
    const uint32_t targets[] = {0x111A5, 0x111AC, 0x111B6};
    Check(mem::Commit(slots, mem::kPage) && mem::Write(slots, targets, sizeof(targets)) == mem::AccessStatus::kOk,
          "indirect CALL target slots initialize in committed guest memory");
    Check(mem::Protect(slots, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "indirect CALL target slots become read-only");

    X86Context ctx{};
    ctx.esp = top;
    ctx.eax = 40;
    SeedFlags(ctx);
    Check(Runs(sub_0001119E, ctx) && ctx.eax == 41 && ctx.esp == top + 4 && ctx.last_icall == 0x111B2,
          "generated indirect and nested direct CALLs dispatch mapped functions and return");
    uint32_t returns[3]{};
    Check(mem::Read(top - sizeof(returns), returns, sizeof(returns)) == mem::AccessStatus::kOk &&
              returns[0] == 0x111B1 && returns[1] == 0x111AB && returns[2] == 0x111A4,
          "nested indirect and direct CALLs place exact return addresses in LIFO stack order");

    ctx = X86Context{};
    ctx.esp = top - 8;
    Check(Runs(sub_000111B9, ctx) && ctx.esp == top + 4 &&
              mem::Read(top - 12, &returns[0], sizeof(returns[0])) == mem::AccessStatus::kOk &&
              returns[0] == 0x111BF,
          "indirect callee RET imm16 validates return address and applies eight-byte cleanup");
    Check(mem::Protect(slots, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "indirect CALL target slots become writable again");

    auto reject_target = [&](uint32_t target, const char* class_name, const char* description) {
        Check(mem::Write(slots, &target, sizeof(target)) == mem::AccessStatus::kOk,
              "indirect CALL rejection target initializes");
        X86Context failed{};
        failed.esp = top;
        failed.eax = 0x12345678;
        failed.last_icall = 0x87654321;
        SeedFlags(failed);
        const X86Context before = failed;
        traps = 0;
        try {
            sub_0001119E(failed, nullptr);
        } catch (const FixtureStop&) {
        }
        Check(traps == 1 && last_trap_pc == 0x1119E &&
                  last_trap_reason.find(class_name) != std::string::npos && SameCoreState(failed, before) &&
                  failed.last_icall == before.last_icall && failed.checked_call == before.checked_call,
              description);
    };

    reject_target(0, "null", "checked indirect CALL rejects null target before stack mutation");
    reject_target(0x11001, "unmapped", "checked indirect CALL rejects generated-code gap before stack mutation");
    reject_target(0x13000, "unknown", "checked indirect CALL rejects unknown target before stack mutation");
    reject_target(dispatch::KernelThunkAddress(255), "kernel", "checked indirect CALL rejects kernel export thunk");
    reject_target(dispatch::HostThunk(sub_000111B2), "host", "checked indirect CALL rejects registered host thunk");

    const uint32_t continuation = 0x111B2;
    const dispatch::Metadata continuation_metadata{
        X86CodeBase, X86CodeEnd, X86EntryPoint,
        std::span<const X86FuncMapping>(X86FuncMappings, X86FuncMappingCount),
        std::span<const uint32_t>(&continuation, 1)};
    std::string error;
    Check(dispatch::Init(continuation_metadata, error), "indirect continuation rejection metadata initializes");
    reject_target(continuation, "continuation", "checked indirect CALL rejects generated continuation");
    Check(dispatch::Init(error), "generated dispatch metadata restored after indirect continuation test");

    Check(mem::Write(slots, &targets[0], sizeof(targets[0])) == mem::AccessStatus::kOk,
          "indirect CALL valid target restored after classification tests");
    uint32_t sentinel = 0;
    Check(mem::Write(top - 4, &sentinel, sizeof(sentinel)) == mem::AccessStatus::kOk,
          "indirect CALL corrupted-return sentinel initializes");
    const uint32_t corruptor = 0x11128;
    Check(mem::Write(slots, &corruptor, sizeof(corruptor)) == mem::AccessStatus::kOk,
          "indirect CALL corrupted-return target initializes");
    ctx = X86Context{};
    ctx.esp = top;
    traps = 0;
    try {
        sub_0001119E(ctx, nullptr);
    } catch (const FixtureStop&) {
    }
    Check(traps == 1 && last_trap_pc == 0x1112F &&
              last_trap_reason.find("expected 000111A4, found DEADBEEF") != std::string::npos &&
              ctx.esp == top - 4 && ctx.checked_call == nullptr,
          "indirect callee RET rejects a corrupted guest return address");

    const uint32_t valid_target = 0x111B2;
    Check(mem::Write(slots, &valid_target, sizeof(valid_target)) == mem::AccessStatus::kOk,
          "indirect CALL valid target restored before memory-fault tests");

    ctx = X86Context{};
    ctx.esp = top;
    ctx.last_icall = 0x87654321;
    const X86Context before_source_fault = ctx;
    Check(ThrowsMemoryFault(sub_000111C0, ctx) && last_fault.instruction == 0x111C0 &&
              last_fault.address == 0x14000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_source_fault) &&
              ctx.last_icall == before_source_fault.last_icall && ctx.checked_call == before_source_fault.checked_call,
          "indirect CALL source read fault reports exact PC/address/status and preserves guest state");

    Check(mem::Protect(slots, mem::kPage, mem::kWrite) == mem::AccessStatus::kOk,
          "indirect CALL source page becomes write-only");
    ctx = X86Context{};
    ctx.esp = top;
    const X86Context before_permission_fault = ctx;
    Check(ThrowsMemoryFault(sub_0001119E, ctx) && last_fault.instruction == 0x1119E &&
              last_fault.address == slots && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              !last_fault.write && SameCoreState(ctx, before_permission_fault) &&
              ctx.last_icall == before_permission_fault.last_icall &&
              ctx.checked_call == before_permission_fault.checked_call,
          "indirect CALL write-only source fault is a checked read and preserves state");
    Check(mem::Protect(slots, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "indirect CALL source page becomes readable again");

    ctx = X86Context{};
    ctx.esp = 0x78000;
    ctx.last_icall = 0x87654321;
    const X86Context before_stack_fault = ctx;
    Check(ThrowsMemoryFault(sub_0001119E, ctx) && last_fault.instruction == 0x1119E &&
              last_fault.address == 0x77FFC && last_fault.status == mem::AccessStatus::kUncommitted &&
              last_fault.write && SameCoreState(ctx, before_stack_fault) &&
              ctx.last_icall == before_stack_fault.last_icall && ctx.checked_call == before_stack_fault.checked_call,
          "indirect CALL stack write fault preserves ESP, dispatch state, and return metadata");

    constexpr uint32_t read_only_stack = 0x76000;
    Check(mem::Commit(read_only_stack, mem::kPage) &&
              mem::Protect(read_only_stack, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "indirect CALL stack-fault page becomes read-only");
    ctx = X86Context{};
    ctx.esp = read_only_stack + mem::kPage;
    const X86Context before_read_only_stack = ctx;
    Check(ThrowsMemoryFault(sub_0001119E, ctx) && last_fault.instruction == 0x1119E &&
              last_fault.address == ctx.esp - 4 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.write && SameCoreState(ctx, before_read_only_stack),
          "indirect CALL read-only stack fault preserves ESP");
    Check(mem::Protect(read_only_stack, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "indirect CALL stack-fault page becomes writable again");

    constexpr uint32_t stack_prefix = 0x85FFE;
    Check(mem::Commit(0x85000, mem::kPage), "indirect CALL stack cross-page prefix commits");
    const uint16_t prefix = 0xBEEF;
    Check(mem::Write(stack_prefix, &prefix, sizeof(prefix)) == mem::AccessStatus::kOk,
          "indirect CALL stack cross-page sentinel initializes");
    ctx = X86Context{};
    ctx.esp = 0x86002;
    Check(ThrowsMemoryFault(sub_0001119E, ctx) && last_fault.instruction == 0x1119E &&
              last_fault.address == 0x86000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              last_fault.write && ctx.esp == 0x86002 &&
              mem::Read(stack_prefix, &sentinel, sizeof(prefix)) == mem::AccessStatus::kOk && sentinel == prefix,
          "indirect CALL cross-page stack fault writes no partial return address");

    ctx = X86Context{};
    ctx.esp = 2;
    Check(ThrowsMemoryFault(sub_0001119E, ctx) && last_fault.instruction == 0x1119E &&
              last_fault.address == UINT32_MAX - 1 && last_fault.status == mem::AccessStatus::kInvalidAddress &&
              last_fault.write && ctx.esp == 2,
          "indirect CALL stack underflow is rejected as an overflowing checked write");

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsMemoryFault(sub_000111CE, ctx) && last_fault.instruction == 0x111CE &&
              last_fault.address == UINT32_MAX - 1 && last_fault.status == mem::AccessStatus::kInvalidAddress &&
              !last_fault.write && ctx.esp == top,
          "indirect CALL target read rejects a 32-bit address overflow");

    constexpr uint32_t cross_page_slot = 0x15FFE;
    Check(mem::Commit(0x15000, 2 * mem::kPage), "indirect CALL cross-page source commits both guest pages");
    Check(mem::Write(cross_page_slot, &valid_target, sizeof(valid_target)) == mem::AccessStatus::kOk,
          "indirect CALL cross-page target initializes");
    ctx = X86Context{};
    ctx.esp = top;
    Check(Runs(sub_000111C7, ctx) && ctx.eax == 1 && ctx.esp == top + 4,
          "indirect CALL target dword reads correctly across guest pages");
    Check(mem::Decommit(0x16000, mem::kPage), "indirect CALL cross-page source tail decommits");
    ctx = X86Context{};
    ctx.esp = top;
    const X86Context before_cross_page_fault = ctx;
    Check(ThrowsMemoryFault(sub_000111C7, ctx) && last_fault.instruction == 0x111C7 &&
              last_fault.address == 0x16000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_cross_page_fault),
          "indirect CALL cross-page source fault reports full read span and preserves state");
    Check(mem::Commit(0x16000, mem::kPage), "indirect CALL cross-page source tail recommits");

    ctx = X86Context{};
    ctx.esp = top;
    const X86Context before_overflow_source = ctx;
    Check(ThrowsMemoryFault(sub_000111CE, ctx) && last_fault.instruction == 0x111CE &&
              last_fault.address == UINT32_MAX - 1 && last_fault.status == mem::AccessStatus::kInvalidAddress &&
              !last_fault.write && SameCoreState(ctx, before_overflow_source),
          "indirect CALL overflowing target read preserves guest state");

    Check(ThrowsTrap(sub_000111D5, ctx) && last_trap_pc == 0x111D5 &&
              last_trap_reason.find("unimplemented: call") != std::string::npos,
          "checked indirect CALL rejects non-absolute memory operand shape");

    Check(mem::Protect(slots, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk &&
              mem::Write(slots, &valid_target, sizeof(valid_target)) == mem::AccessStatus::kOk,
          "indirect CALL source restored before concurrent protection test");
    std::barrier start(2);
    std::atomic<mem::AccessStatus> protect_status{mem::AccessStatus::kHostError};
    std::thread protector([&] {
        start.arrive_and_wait();
        protect_status = mem::Protect(slots, mem::kPage, mem::kWrite);
    });
    ctx = X86Context{};
    ctx.esp = top;
    const X86Context before_race = ctx;
    start.arrive_and_wait();
    bool completed = false;
    faults = traps = 0;
    try {
        sub_0001119E(ctx, nullptr);
        completed = true;
    } catch (const FixtureStop&) {
    }
    protector.join();
    const bool race_fault = faults == 1 && traps == 0 && last_fault.instruction == 0x1119E &&
                            last_fault.address == slots && last_fault.status == mem::AccessStatus::kPermissionDenied &&
                            !last_fault.write && SameCoreState(ctx, before_race);
    Check(protect_status == mem::AccessStatus::kOk && (completed || race_fault),
          "concurrent indirect source protection yields complete CALL or precise checked read fault");
    Check(mem::Protect(slots, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "indirect CALL concurrent protection test restores source page");

    Check(mem::Write(slots, &valid_target, sizeof(valid_target)) == mem::AccessStatus::kOk,
          "indirect CALL valid target restored before uninitialized dispatch test");
    dispatch::Reset();
    ctx = X86Context{};
    ctx.esp = top;
    const X86Context before_uninitialized = ctx;
    traps = 0;
    try {
        sub_0001119E(ctx, nullptr);
    } catch (const FixtureStop&) {
    }
    Check(traps == 1 && last_trap_pc == 0x1119E &&
              last_trap_reason.find("uninitialized") != std::string::npos &&
              SameCoreState(ctx, before_uninitialized),
          "checked indirect CALL rejects valid-looking target while dispatch table is uninitialized");
    Check(dispatch::Init(error), "generated dispatch table restored after uninitialized-table test");

    std::barrier dispatch_start(2);
    std::atomic<bool> dispatch_results_valid{true};
    std::thread dispatch_mutator([&] {
        dispatch_start.arrive_and_wait();
        std::string init_error;
        for (int i = 0; i < 64; ++i) {
            dispatch::Reset();
            if (!dispatch::Init(init_error)) dispatch_results_valid = false;
        }
    });
    dispatch_start.arrive_and_wait();
    for (int i = 0; i < 512; ++i) {
        const dispatch::ResolvedTarget resolved = dispatch::ResolveTarget(valid_target);
        if ((resolved.kind == dispatch::TargetKind::kGeneratedFunction && resolved.function != sub_000111B2) ||
            (resolved.kind != dispatch::TargetKind::kGeneratedFunction &&
             resolved.kind != dispatch::TargetKind::kUninitialized))
            dispatch_results_valid = false;
    }
    dispatch_mutator.join();
    Check(dispatch_results_valid,
          "concurrent dispatch reset yields stable generated target or explicit uninitialized result");
}

void CheckKernel255AbiAndPreparationTests() {
    constexpr uint32_t slot = 0x12000, stack_base = 0xA0000, top = 0xA1000, output = 0xA4000;
    Check(mem::Commit(slot, mem::kPage) && mem::Commit(stack_base, 2 * mem::kPage) &&
              mem::Commit(output, 4 * mem::kPage),
          "ordinal-255 fixture slot, guest stack, and output pages commit");
    const uint32_t sentinel = 0xAABBCCDD;
    std::array<uint32_t, 10> args{output, 0x44, 0x10000, 0x40, output + 4, 0x111B2, 0x12345678, 0, 0,
                                  0x111AC};
    auto invoke = [&](uint32_t target, uint32_t stack, const std::array<uint32_t, 10>& values, bool write_args = true) {
        Check(mem::Write(slot, &target, sizeof(target)) == mem::AccessStatus::kOk,
              "ordinal-255 fixture target patches the generated import slot");
        if (write_args) {
            std::array<uint32_t, 11> frame{};
            std::copy(values.begin(), values.end(), frame.begin());
            frame.back() = sentinel;
            Check(mem::Write(stack, frame.data(), sizeof(frame)) == mem::AccessStatus::kOk,
                  "ordinal-255 fixture stack arguments initialize");
        }
        X86Context ctx{};
        ctx.esp = stack;
        SeedFlags(ctx);
        traps = faults = 0;
        bool completed = false;
        try {
            sub_0001119E(ctx, nullptr);
            completed = true;
        } catch (const FixtureStop&) {
        }
        return std::pair{completed, ctx};
    };

    const uint32_t import_ordinal = 0x800000FF;
    const uint32_t calls_before_import = g_kernel255_calls;
    const auto [import_completed, import_ctx] = invoke(import_ordinal, top, args);
    Check(!import_completed && traps == 1 && last_trap_pc == 0x1119E &&
              last_trap_reason.find("unknown") != std::string::npos && import_ctx.esp == top &&
              g_kernel255_calls == calls_before_import,
          "generated CALL rejects the original ordinal-encoded import before patching");

    kernel::RegisterExport(255, "WrongExportName", Kernel255FixtureHandler);
    Check(!dispatch::SetKernel255FixtureBridge(true),
          "test bridge refuses ordinal 255 until its exact export name is registered");
    const uint32_t thunk = dispatch::KernelThunkAddress(255);
    const auto [unregistered_completed, unregistered_ctx] = invoke(thunk, top, args);
    Check(!unregistered_completed && traps == 1 && last_trap_reason.find("kernel") != std::string::npos &&
              unregistered_ctx.esp == top && g_kernel255_calls == calls_before_import,
          "checked CALL rejects an unregistered ordinal-255 export without stack mutation");

    kernel::RegisterExport(255, "PsCreateSystemThreadEx", Kernel255FixtureHandler);
    Check(dispatch::SetKernel255FixtureBridge(true),
          "test bridge authorizes only the registered PsCreateSystemThreadEx ordinal");
    g_kernel255_mode = Kernel255Mode::kAbiOnly;
    const auto [abi_completed, abi_ctx] = invoke(thunk, top, args);
    Check(abi_completed && abi_ctx.eax == kKernel255Result && abi_ctx.esp == top + 44 &&
              abi_ctx.cf == 1 && abi_ctx.zf == 0 && abi_ctx.sf == 1 && abi_ctx.of == 1 && abi_ctx.pf == 0 &&
              abi_ctx.af == 1 && abi_ctx.checked_call == nullptr && abi_ctx.last_icall == thunk &&
              g_kernel255_args == args,
          "generated kernel CALL preserves flags, returns EAX, validates frame, and applies ten-argument stdcall cleanup");

    const uint32_t calls_before_wrong_ordinal = g_kernel255_calls;
    const auto [wrong_ordinal_completed, wrong_ordinal_ctx] = invoke(dispatch::KernelThunkAddress(254), top, args);
    Check(!wrong_ordinal_completed && traps == 1 && wrong_ordinal_ctx.esp == top &&
              g_kernel255_calls == calls_before_wrong_ordinal,
          "checked bridge rejects the neighboring kernel ordinal");
    const auto [mutated_completed, mutated_ctx] = invoke(thunk + 1, top, args);
    Check(!mutated_completed && traps == 1 && mutated_ctx.esp == top &&
              g_kernel255_calls == calls_before_wrong_ordinal,
          "checked bridge rejects a misaligned mutated ordinal-255 thunk address");

    Check(mem::Write(slot, &thunk, sizeof(thunk)) == mem::AccessStatus::kOk,
          "exact ordinal-255 thunk restores before checked return-address test");
    X86Context invalid_return{};
    invalid_return.esp = top;
    SeedFlags(invalid_return);
    const X86Context invalid_return_before = invalid_return;
    traps = 0;
    try {
        x86_checked_indirect_call(invalid_return, nullptr, slot, 0x90000, 0x1119E);
    } catch (const FixtureStop&) {
    }
    Check(traps == 1 && last_trap_pc == 0x1119E && last_trap_reason.find("return address") != std::string::npos &&
              SameCoreState(invalid_return, invalid_return_before) && invalid_return.checked_call == nullptr &&
              g_kernel255_calls == calls_before_wrong_ordinal,
          "checked kernel CALL rejects return addresses outside the generated guest code span before dispatch");

    g_kernel255_mode = Kernel255Mode::kBadCleanup;
    const auto [bad_cleanup_completed, bad_cleanup_ctx] = invoke(thunk, top, args);
    Check(!bad_cleanup_completed && traps == 1 && last_trap_reason.find("stdcall") != std::string::npos &&
              bad_cleanup_ctx.checked_call == nullptr,
          "checked bridge rejects a test export with incorrect stdcall cleanup");
    g_kernel255_mode = Kernel255Mode::kBadReturnAddress;
    const auto [bad_return_completed, bad_return_ctx] = invoke(thunk, top, args);
    Check(!bad_return_completed && traps == 1 && last_trap_reason.find("return address was modified") != std::string::npos &&
              bad_return_ctx.checked_call == nullptr,
          "checked bridge rejects a modified checked CALL return address");

    const uint32_t uncommitted_top = 0xA2000;
    const auto [bad_stack_completed, bad_stack_ctx] = invoke(thunk, uncommitted_top, args, false);
    Check(!bad_stack_completed && faults == 1 && last_fault.instruction == 0x1119E &&
              last_fault.address == uncommitted_top && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && bad_stack_ctx.esp == uncommitted_top &&
              g_kernel255_calls == calls_before_wrong_ordinal + 2,
          "checked bridge validates all ten argument words before invoking the export");
    Check(mem::Protect(top, mem::kPage, mem::kNoAccess) == mem::AccessStatus::kOk,
          "ordinal-255 argument page becomes inaccessible");
    const auto [denied_stack_completed, denied_stack_ctx] = invoke(thunk, top, args, false);
    Check(!denied_stack_completed && faults == 1 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              denied_stack_ctx.esp == top,
          "checked bridge rejects a denied argument span before invoking the export");
    Check(mem::Protect(top, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "ordinal-255 argument page permissions restore");

    g_kernel255_mode = Kernel255Mode::kPrepareThread;
    g_prepared_thread_count = 0;
    const auto baseline = mem::Query();
    const uint32_t workers_before = kernel::ThreadWorkerLaunchCountForTest();
    std::array<uint32_t, 2> outputs{0xAAAAAAAA, 0xBBBBBBBB};
    Check(mem::Write(output, outputs.data(), sizeof(outputs)) == mem::AccessStatus::kOk,
          "thread preparation output sentinels initialize");
    const auto [prepared_completed, prepared_ctx] = invoke(thunk, top, args);
    uint32_t published_handle = 0, published_id = 0;
    mem::Read(output, &published_handle, sizeof(published_handle));
    mem::Read(output + 4, &published_id, sizeof(published_id));
    const auto prepared_thread = g_prepared_threads[0];
    Check(prepared_completed && prepared_ctx.eax == kernel::kSuccess && prepared_ctx.esp == top + 44 &&
              g_prepared_thread_count == 1 && prepared_thread &&
              prepared_thread->state.load() == kernel::ThreadState::kPrepared &&
              prepared_thread->start_routine == args[5] && prepared_thread->start_context == args[6] &&
              prepared_thread->system_routine == args[9] && published_handle != 0 &&
              published_id == prepared_thread->id && kernel::FromHandle(published_handle).get() == prepared_thread.get() &&
              kernel::IsPointerBoundForTest(prepared_thread->ethread, prepared_thread.get()) &&
              kernel::Threads().empty() && kernel::ThreadWorkerLaunchCountForTest() == workers_before,
          "ordinal-255 test service prepares and publishes a thread without activating a host worker or guest callback");
    X86Context prepared_context{};
    const uint32_t prepared_stack_low = prepared_thread ? prepared_thread->stack_top - prepared_thread->stack_size : 0;
    const bool prepared_context_ready =
        prepared_thread && kernel::InitializePreparedThreadContextForTest(prepared_thread, prepared_context);
    Check(prepared_context_ready &&
              prepared_thread->state.load() == kernel::ThreadState::kPrepared &&
              prepared_context.fs_base == prepared_thread->kpcr &&
              prepared_context.esp == ((prepared_thread->tls & ~15u) - 16) &&
              prepared_context.esp >= prepared_stack_low && prepared_context.esp < prepared_thread->stack_top &&
              prepared_context.fcw == 0x27F && prepared_context.mxcsr == 0x1F80 &&
              kernel::Threads().empty() && kernel::ThreadWorkerLaunchCountForTest() == workers_before,
          "production context initialization binds KPCR FS base and bounded TLS stack without starting a thread");
    Check(prepared_context_ready && Runs(sub_0001123F, prepared_context) &&
              prepared_context.eax == prepared_thread->ethread,
          "production-prepared FS base reads KPCR current ETHREAD through generated code");
    if (prepared_context_ready) prepared_context.eax = prepared_thread->ethread - prepared_thread->kpcr;
    prepared_context.ecx = 0;
    Check(prepared_context_ready && Runs(sub_00011246, prepared_context) &&
              prepared_context.eax == prepared_thread->tls,
          "production-prepared context reaches ETHREAD TLS through generated FS base/index addressing");
    if (prepared_thread) prepared_thread->ctx = nullptr;
    auto duplicate_output = prepared_thread;
    const uint32_t duplicate_status = kernel::PrepareSystemThreadEx(
        output, 0x10000, 0x20, output + 4, args[5], args[6], false, args[9], duplicate_output);
    std::array<uint32_t, 2> after_duplicate{};
    const bool duplicate_outputs_read = mem::Read(output, after_duplicate.data(), sizeof(after_duplicate)) ==
                                       mem::AccessStatus::kOk;
    Check(duplicate_status == kernel::kInvalidParameter && duplicate_output.get() == prepared_thread.get() &&
              kernel::FromHandle(published_handle).get() == prepared_thread.get() && duplicate_outputs_read &&
              after_duplicate[0] == published_handle && after_duplicate[1] == published_id,
          "preparation rejects reuse of an occupied output reference without replacing its live thread");
    const uint32_t prepared_stack = prepared_thread ? prepared_thread->stack_top : 0;
    const uint32_t prepared_ethread = prepared_thread ? prepared_thread->ethread : 0;
    Check(kernel::CancelPreparedThread(prepared_thread) &&
              prepared_thread->state.load() == kernel::ThreadState::kCancelled &&
              !kernel::FromHandle(published_handle) && kernel::ThreadWorkerLaunchCountForTest() == workers_before &&
              !kernel::IsPointerBoundForTest(prepared_ethread, prepared_thread.get()) &&
              !mem::IsCommitted(prepared_stack - 1) &&
              kernel::Threads().empty() && mem::Query().system_used == baseline.system_used &&
              mem::Read(output, outputs.data(), sizeof(outputs)) == mem::AccessStatus::kOk && outputs[0] == 0 &&
              outputs[1] == 0 && !kernel::CancelPreparedThread(prepared_thread) &&
              !kernel::StartThread(prepared_thread) && kernel::ThreadWorkerLaunchCountForTest() == workers_before,
          "prepared thread cancellation closes its handle, clears owned outputs, frees guest blocks, and cannot repeat");
    g_prepared_threads[0].reset();
    duplicate_output.reset();
    g_prepared_thread_count = 0;

    auto failure = [&](std::array<uint32_t, 10> failure_args, kernel::ThreadPreparationFailPoint point,
                       uint32_t expected_status, const char* description) {
        const uint32_t old_workers = kernel::ThreadWorkerLaunchCountForTest();
        const std::array<uint32_t, 2> sentinels{0xAAAAAAAA, 0xBBBBBBBB};
        mem::Write(output, sentinels.data(), sizeof(sentinels));
        kernel::SetThreadPreparationFailureForTest(point);
        const auto [completed, failed_ctx] = invoke(thunk, top, failure_args);
        std::array<uint32_t, 2> after{};
        mem::Read(output, after.data(), sizeof(after));
        Check(completed && failed_ctx.eax == expected_status && g_prepared_thread_count == 0 &&
                  after == sentinels && mem::Query().system_used == baseline.system_used &&
                  kernel::Threads().empty() && kernel::ThreadWorkerLaunchCountForTest() == old_workers,
              description);
    };

    auto invalid_stack = args;
    invalid_stack[2] = UINT32_MAX;
    failure(invalid_stack, kernel::ThreadPreparationFailPoint::kNone, kernel::kInvalidParameter,
            "oversized stack request fails before allocation or output publication");
    auto invalid_tls = args;
    invalid_tls[3] = UINT32_MAX;
    failure(invalid_tls, kernel::ThreadPreparationFailPoint::kNone, kernel::kInvalidParameter,
            "overflowing TLS request fails before allocation or output publication");
    for (const auto [point, label] : {
             std::pair{kernel::ThreadPreparationFailPoint::kAfterStack, "stack allocation rollback"},
             std::pair{kernel::ThreadPreparationFailPoint::kAfterKpcr, "KPCR allocation rollback"},
             std::pair{kernel::ThreadPreparationFailPoint::kAfterEthread, "ETHREAD allocation rollback"}}) {
        const std::string description = std::string("injected ") + label + " frees all prepared resources";
        failure(args, point, kernel::kNoMemory, description.c_str());
    }
    failure(args, kernel::ThreadPreparationFailPoint::kBeforePublish, kernel::kNoMemory,
            "injected output publication failure after allocation rolls back the prepared thread");

    auto invalid_output = args;
    invalid_output[4] = uncommitted_top;
    failure(invalid_output, kernel::ThreadPreparationFailPoint::kNone, kernel::kAccessViolation,
            "uncommitted output page failure cancels preparation without publishing a handle");

    const std::array<uint32_t, 2> write_only_sentinels{0xAAAAAAAA, 0xBBBBBBBB};
    mem::Write(output, write_only_sentinels.data(), sizeof(write_only_sentinels));
    const bool write_only_protected = mem::Protect(output, 4 * mem::kPage, mem::kWrite) == mem::AccessStatus::kOk;
    kernel::ThreadRef write_only_output;
    const uint32_t write_only_status = write_only_protected
                                           ? kernel::PrepareSystemThreadEx(output, 0x10000, 0x20, output + 4,
                                                                           args[5], args[6], false, args[9],
                                                                           write_only_output)
                                           : kernel::kNoMemory;
    const bool write_only_restored = mem::Protect(output, 4 * mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk;
    std::array<uint32_t, 2> write_only_after{};
    Check(write_only_protected && write_only_restored && write_only_status == kernel::kAccessViolation &&
              !write_only_output && mem::Read(output, write_only_after.data(), sizeof(write_only_after)) ==
                                        mem::AccessStatus::kOk &&
              write_only_after == write_only_sentinels && mem::Query().system_used == baseline.system_used &&
              kernel::Threads().empty() && kernel::ThreadWorkerLaunchCountForTest() == workers_before,
          "write-only output pages reject preparation before publication and rollback all allocations");

    const std::array<uint32_t, 10> duplicate_args_a{output, 0, 0x10000, 0x20, output + 4, 0x111B2, 0x111AC, 0, 0,
                                                    0x111AC};
    const std::array<uint32_t, 10> duplicate_args_b{output + 8, 0, 0x10000, 0x20, output + 12, 0x111B2,
                                                    0x22223333, 0, 0, 0x111AC};
    g_prepared_thread_count = 0;
    Check(mem::Write(output, outputs.data(), sizeof(outputs)) == mem::AccessStatus::kOk &&
              mem::Write(output + 8, outputs.data(), sizeof(outputs)) == mem::AccessStatus::kOk,
          "duplicate-preparation output regions initialize");
    const auto [first_completed, first_ctx] = invoke(thunk, top, duplicate_args_a);
    const auto [second_completed, second_ctx] = invoke(thunk, top, duplicate_args_b);
    const auto first_thread = g_prepared_threads[0];
    const auto second_thread = g_prepared_threads[1];
    const bool duplicate_prepared = first_completed && second_completed && first_ctx.eax == kernel::kSuccess &&
                                    second_ctx.eax == kernel::kSuccess && first_thread && second_thread &&
                                    first_thread->id != second_thread->id &&
                                    first_thread->stack_top != second_thread->stack_top &&
                                    kernel::ThreadWorkerLaunchCountForTest() == workers_before &&
                                    kernel::Threads().empty();
    const bool both_cancelled = kernel::CancelPreparedThread(first_thread) &&
                                kernel::CancelPreparedThread(second_thread);
    Check(duplicate_prepared && both_cancelled && mem::Query().system_used == baseline.system_used &&
              kernel::Threads().empty(),
          "separate prepared calls own distinct resources and both cancel without worker activation");
    g_prepared_threads[0].reset();
    g_prepared_threads[1].reset();
    g_prepared_thread_count = 0;
    g_kernel255_mode = Kernel255Mode::kAbiOnly;
    Check(dispatch::SetKernel255FixtureBridge(false), "ordinal-255 test-only bridge disables after fixture coverage");
}

void CheckPostCallInstructionTests() {
    X86Context ctx{};
    ctx.esp = 0x74000;
    ctx.ecx = 0x80000000;
    SeedFlags(ctx);
    Check(Runs(sub_000111D8, ctx) && ctx.cf == 0 && ctx.of == 0 && ctx.sf == 1 && ctx.zf == 0 &&
              ctx.pf == 1 && ctx.af == 1,
          "checked TEST ECX,ECX sets defined flags and preserves undefined AF");

    constexpr uint32_t top = 0x75000;
    const uint32_t saved_ebp = 0xAABBCCDD;
    Check(mem::Write(top - 4, &saved_ebp, sizeof(saved_ebp)) == mem::AccessStatus::kOk,
          "POP EBP fixture stack word initializes");
    ctx = X86Context{};
    ctx.esp = top - 4;
    ctx.ebp = 0x12345678;
    SeedFlags(ctx);
    Check(Runs(sub_000111DB, ctx) && ctx.ebp == saved_ebp && ctx.esp == top + 4 && ctx.cf == 1 && ctx.af == 1,
          "checked POP EBP reads before register and ESP updates");

    ctx = X86Context{};
    ctx.esp = 0x78000;
    ctx.ebp = 0x12345678;
    SeedFlags(ctx);
    const X86Context before_fault = ctx;
    Check(ThrowsMemoryFault(sub_000111DB, ctx) && last_fault.instruction == 0x111DB &&
              last_fault.address == 0x78000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_fault),
          "checked POP EBP read fault preserves EBP, ESP, and flags");

    ctx = X86Context{};
    ctx.esp = 0x79000;
    Check(Runs(sub_000111DD, ctx) && ctx.eax == 42,
          "checked short JMP follows its generated in-function target and skips fallthrough bytes");

    constexpr uint32_t pop_stack = 0x7B000;
    const uint32_t values[] = {0x11223344, 0x55667788, 0x99AABBCC};
    Check(mem::Write(pop_stack - sizeof(values), values, sizeof(values)) == mem::AccessStatus::kOk,
          "POP EDI and POP ESI stack fixture initializes");
    ctx = X86Context{};
    ctx.esp = pop_stack - sizeof(values);
    Check(Runs(sub_000111EA, ctx) && ctx.edi == values[0] && ctx.esi == values[1] && ctx.esp == pop_stack,
          "checked POP EDI and POP ESI read stack before advancing ESP");

    ctx = X86Context{};
    ctx.esp = 0x7C000;
    ctx.edi = 0x12345678;
    ctx.esi = 0x87654321;
    SeedFlags(ctx);
    const X86Context before_pop_fault = ctx;
    Check(ThrowsMemoryFault(sub_000111EA, ctx) && last_fault.instruction == 0x111EA &&
              last_fault.address == 0x7C000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_pop_fault),
          "checked POP EDI read fault preserves EDI, ESI, ESP, and flags");

    constexpr uint32_t pop_ebx_stack = 0xC1000;
    const uint32_t saved_ebx = 0xA1B2C3D4;
    Check(mem::Commit(pop_ebx_stack, mem::kPage) &&
              mem::Write(pop_ebx_stack, &saved_ebx, sizeof(saved_ebx)) == mem::AccessStatus::kOk,
          "POP EBX fixture stack word initializes");
    ctx = X86Context{};
    ctx.esp = pop_ebx_stack;
    ctx.ebx = 0x12345678;
    ctx.eax = 0x11223344;
    ctx.ecx = 0x55667788;
    ctx.edx = 0x99AABBCC;
    ctx.esi = 0xAABBCCDD;
    ctx.edi = 0xEEFF0011;
    ctx.ebp = 0x22334455;
    SeedFlags(ctx);
    const SubFlags pop_ebx_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(Runs(sub_000111ED, ctx) && ctx.ebx == saved_ebx && ctx.esp == pop_ebx_stack + 8 &&
              ctx.eax == 0x11223344 && ctx.ecx == 0x55667788 && ctx.edx == 0x99AABBCC &&
              ctx.esi == 0xAABBCCDD && ctx.edi == 0xEEFF0011 && ctx.ebp == 0x22334455 &&
              MatchesFlags(ctx, pop_ebx_flags),
          "checked POP EBX reads stack, advances ESP, and preserves other registers and flags");

    constexpr uint32_t pop_ebx_guard = 0xC0000;
    ctx = X86Context{};
    ctx.esp = pop_ebx_guard;
    ctx.ebx = 0xCAFEBABE;
    SeedFlags(ctx);
    const X86Context before_pop_ebx_fault = ctx;
    Check(ThrowsMemoryFault(sub_000111ED, ctx) && last_fault.instruction == 0x111ED &&
              last_fault.address == pop_ebx_guard && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_pop_ebx_fault),
          "checked POP EBX uncommitted read fault preserves EBX, ESP, and flags");

    constexpr uint32_t pop_ebx_cross = 0xC2000;
    Check(mem::Commit(pop_ebx_cross, mem::kPage), "POP EBX cross-page fixture commits lower page");
    ctx = X86Context{};
    ctx.esp = pop_ebx_cross + mem::kPage - 2;
    ctx.ebx = 0x13579BDF;
    SeedFlags(ctx);
    const X86Context before_pop_ebx_cross = ctx;
    Check(ThrowsMemoryFault(sub_000111ED, ctx) && last_fault.instruction == 0x111ED &&
              last_fault.address == pop_ebx_cross + mem::kPage &&
              last_fault.status == mem::AccessStatus::kUncommitted && !last_fault.write &&
              SameCoreState(ctx, before_pop_ebx_cross),
          "POP EBX cross-page read validates the full dword before changing EBX or ESP");
}

void CheckNotRepAndFsTests() {
    X86Context ctx{};
    ctx.eax = 0xA1B2C3A5;
    SeedFlags(ctx);
    const SubFlags flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(Runs(sub_00011210, ctx) && ctx.eax == 0xA1B2C35A && MatchesFlags(ctx, flags),
          "checked generated NOT AL flips only AL and preserves every flag");

    const struct {
        X86Func* function;
        uint32_t pc;
        const char* name;
    } not_traps[] = {{sub_00011213, 0x11213, "NOT EAX"}, {sub_00011216, 0x11216, "NOT BL"},
                     {sub_00011219, 0x11219, "NOT AH"}, {sub_0001121C, 0x1121C, "NOT memory"}};
    for (const auto& test : not_traps) {
        ctx = X86Context{};
        ctx.eax = 0x12345678;
        ctx.ebx = 0x87654321;
        SeedFlags(ctx);
        const X86Context before = ctx;
        Check(ThrowsTrap(test.function, ctx) && last_trap_pc == test.pc && SameCoreState(ctx, before), test.name);
    }

    const struct {
        X86Func* function;
        uint32_t pc;
        const char* name;
    } extend_traps[] = {{sub_00011262, 0x11262, "MOVZX GS memory"},
                        {sub_0001126B, 0x1126B, "16-bit-address MOVZX memory"},
                        {sub_00011270, 0x11270, "MOVSX GS memory"},
                        {sub_00011279, 0x11279, "16-bit-address MOVSX memory"}};
    for (const auto& test : extend_traps) {
        ctx = X86Context{};
        ctx.fs_base = 0xA3000;
        ctx.eax = 0x12345678;
        SeedFlags(ctx);
        const X86Context before = ctx;
        Check(ThrowsTrap(test.function, ctx) && last_trap_pc == test.pc && SameCoreState(ctx, before), test.name);
    }

    constexpr uint32_t source = 0x90000, destination = 0x91000;
    Check(mem::Commit(source, 5 * mem::kPage) && mem::Commit(destination, 2 * mem::kPage),
          "REP semantic fixture pages commit at 4 KB granularity");
    const uint8_t input[] = {1, 2, 3, 4, 5, 6, 7, 8};
    Check(mem::Write(source, input, sizeof(input)) == mem::AccessStatus::kOk,
          "REP MOVS source bytes initialize");
    ctx = X86Context{};
    ctx.esi = source;
    ctx.edi = destination;
    ctx.ecx = 4;
    SeedFlags(ctx);
    const SubFlags rep_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(Runs(sub_0001121F, ctx) && ctx.esi == source + 4 && ctx.edi == destination + 4 && ctx.ecx == 0 &&
              MatchesFlags(ctx, rep_flags),
          "REP MOVSB advances ESI, EDI, ECX and preserves flags");
    uint8_t copied[8]{};
    Check(mem::Read(destination, copied, sizeof(copied)) == mem::AccessStatus::kOk &&
              std::equal(std::begin(input), std::begin(input) + 4, std::begin(copied)),
          "REP MOVSB copies each byte through checked guest memory");

    const uint32_t words[] = {0x12345678, 0xAABBCCDD};
    Check(mem::Write(source, words, sizeof(words)) == mem::AccessStatus::kOk,
          "REP MOVSD source dwords initialize");
    ctx = X86Context{};
    ctx.esi = source;
    ctx.edi = destination;
    ctx.ecx = 2;
    SeedFlags(ctx);
    const SubFlags movsd_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(Runs(sub_00011222, ctx) && ctx.esi == source + sizeof(words) &&
              ctx.edi == destination + sizeof(words) && ctx.ecx == 0 && MatchesFlags(ctx, movsd_flags),
          "REP MOVSD advances full-width indices and count");
    uint32_t copied_words[2]{};
    Check(mem::Read(destination, copied_words, sizeof(copied_words)) == mem::AccessStatus::kOk &&
              std::equal(std::begin(words), std::end(words), std::begin(copied_words)),
          "REP MOVSD copies full dwords across the checked helper");

    ctx = X86Context{};
    ctx.eax = 0x123456AA;
    ctx.edi = destination;
    ctx.ecx = 4;
    SeedFlags(ctx);
    const SubFlags stosb_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(Runs(sub_00011225, ctx) && ctx.edi == destination + 4 && ctx.ecx == 0 &&
              MatchesFlags(ctx, stosb_flags),
          "REP STOSB advances EDI and ECX without changing flags");
    std::fill(std::begin(copied), std::begin(copied) + 4, uint8_t(0xAA));
    Check(mem::Read(destination, copied, 4) == mem::AccessStatus::kOk &&
              std::all_of(std::begin(copied), std::begin(copied) + 4, [](uint8_t value) { return value == 0xAA; }),
          "REP STOSB writes AL only");

    ctx = X86Context{};
    ctx.eax = 0xAABBCCDD;
    ctx.edi = destination;
    ctx.ecx = 2;
    SeedFlags(ctx);
    const SubFlags stosd_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(Runs(sub_00011228, ctx) && ctx.edi == destination + 8 && ctx.ecx == 0 &&
              mem::Read(destination, copied_words, sizeof(copied_words)) == mem::AccessStatus::kOk &&
              copied_words[0] == 0xAABBCCDD && copied_words[1] == 0xAABBCCDD && MatchesFlags(ctx, stosd_flags),
          "REP STOSD writes EAX dwords, updates count and destination, and preserves flags");

    Check(mem::Commit(0xAD000, mem::kPage), "backward REP STOSD destination page commits");
    ctx = X86Context{};
    ctx.eax = 0x55667788;
    ctx.edi = 0xAD008;
    ctx.ecx = 3;
    ctx.df = 1;
    Check(Runs(sub_00011228, ctx) && ctx.edi == 0xACFFC && ctx.ecx == 0 &&
              mem::Read(0xAD000, copied_words, sizeof(copied_words)) == mem::AccessStatus::kOk &&
              copied_words[0] == 0x55667788 && copied_words[1] == 0x55667788,
          "backward REP STOSD follows DF and writes each descending dword");

    const uint8_t overlap[] = {1, 2, 3, 4, 5};
    Check(mem::Write(0x92000, overlap, sizeof(overlap)) == mem::AccessStatus::kOk,
          "overlapping REP MOVSB fixture initializes");
    ctx = X86Context{};
    ctx.esi = 0x92000;
    ctx.edi = 0x92001;
    ctx.ecx = 4;
    Check(Runs(sub_0001121F, ctx) && ctx.esi == 0x92004 && ctx.edi == 0x92005 && ctx.ecx == 0 &&
              mem::Read(0x92000, copied, 5) == mem::AccessStatus::kOk &&
              std::all_of(std::begin(copied), std::begin(copied) + 5,
                          [](uint8_t value) { return value == 1; }),
          "overlapping forward REP MOVSB preserves sequential string-instruction semantics");

    const uint8_t backward_overlap[] = {1, 2, 3, 4, 5};
    Check(mem::Write(0x92000, backward_overlap, sizeof(backward_overlap)) == mem::AccessStatus::kOk,
          "backward overlapping REP MOVSB fixture initializes");
    ctx = X86Context{};
    ctx.esi = 0x92004;
    ctx.edi = 0x92003;
    ctx.ecx = 4;
    ctx.df = 1;
    Check(Runs(sub_0001121F, ctx) && ctx.esi == 0x92000 && ctx.edi == 0x91FFF && ctx.ecx == 0 &&
              mem::Read(0x92000, copied, 5) == mem::AccessStatus::kOk &&
              std::all_of(std::begin(copied), std::begin(copied) + 5,
                          [](uint8_t value) { return value == 5; }),
          "overlapping backward REP MOVSB follows per-iteration DF semantics");

    const uint8_t backward[] = {9, 8, 7};
    Check(mem::Write(0x93000, backward, sizeof(backward)) == mem::AccessStatus::kOk,
          "backward REP MOVSB source initializes");
    ctx = X86Context{};
    ctx.esi = 0x93002;
    ctx.edi = 0x94002;
    ctx.ecx = 3;
    ctx.df = 1;
    Check(Runs(sub_0001121F, ctx) && ctx.esi == 0x92FFF && ctx.edi == 0x93FFF && ctx.ecx == 0 &&
              mem::Read(0x94000, copied, 3) == mem::AccessStatus::kOk &&
              std::equal(std::begin(backward), std::end(backward), std::begin(copied)),
          "backward REP MOVSB follows DF and decrements indices");

    ctx = X86Context{};
    ctx.esi = UINT32_MAX;
    ctx.edi = UINT32_MAX;
    SeedFlags(ctx);
    X86Context zero_count_after = ctx;
    zero_count_after.esp += 4;
    Check(Runs(sub_0001121F, ctx) && SameCoreState(ctx, zero_count_after),
          "zero-count REP MOVSB touches no guest memory or index registers");

    Check(mem::Commit(0x95000, 2 * mem::kPage) && mem::Commit(0x97000, mem::kPage),
          "cross-page REP MOVSD fixture pages commit independently");
    const uint32_t crossing_value = 0xDEADBEEF;
    Check(mem::Write(0x95FFE, &crossing_value, sizeof(crossing_value)) == mem::AccessStatus::kOk,
          "REP MOVSD dword straddles two guest pages");
    ctx = X86Context{};
    ctx.esi = 0x95FFE;
    ctx.edi = 0x97000;
    ctx.ecx = 1;
    Check(Runs(sub_00011222, ctx) && ctx.esi == 0x96002 && ctx.edi == 0x97004 && ctx.ecx == 0 &&
              mem::Read(0x97000, copied_words, sizeof(uint32_t)) == mem::AccessStatus::kOk &&
              copied_words[0] == crossing_value,
          "REP MOVSD validates a full element across independent 4 KB guest pages");

    Check(mem::Commit(0x98000, mem::kPage) && mem::Commit(0x9A000, mem::kPage) &&
              mem::Write(0x98FFF, input, 1) == mem::AccessStatus::kOk,
          "REP MOVSB source fault fixture commits only its first guest page");
    ctx = X86Context{};
    ctx.esi = 0x98FFF;
    ctx.edi = 0x9A000;
    ctx.ecx = 2;
    SeedFlags(ctx);
    const SubFlags source_fault_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(ThrowsMemoryFault(sub_0001121F, ctx) && last_fault.instruction == 0x1121F &&
              last_fault.address == 0x99000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && ctx.esi == 0x99000 && ctx.edi == 0x9A001 && ctx.ecx == 1 &&
              MatchesFlags(ctx, source_fault_flags),
          "REP MOVSB source fault reports next page and preserves one completed iteration");

    Check(mem::Commit(0x9B000, mem::kPage) && mem::Commit(0x9C000, 2 * mem::kPage) &&
              mem::Write(0x9B000, input, 2) == mem::AccessStatus::kOk &&
              mem::Protect(0x9D000, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "REP MOVSB destination page becomes read-only after the first writable page");
    ctx = X86Context{};
    ctx.esi = 0x9B000;
    ctx.edi = 0x9CFFF;
    ctx.ecx = 2;
    SeedFlags(ctx);
    const SubFlags destination_fault_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(ThrowsMemoryFault(sub_0001121F, ctx) && last_fault.instruction == 0x1121F &&
              last_fault.address == 0x9D000 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.write && ctx.esi == 0x9B001 && ctx.edi == 0x9D000 && ctx.ecx == 1 &&
              MatchesFlags(ctx, destination_fault_flags),
          "REP MOVSB destination protection fault keeps exact partial progress");
    Check(mem::Protect(0x9D000, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "REP MOVSB destination protection restores");

    Check(mem::Commit(0xB3000, 2 * mem::kPage) && mem::Commit(0xB5000, mem::kPage) &&
              mem::Write(0xB3FFF, input, 1) == mem::AccessStatus::kOk &&
              mem::Decommit(0xB4000, mem::kPage),
          "REP MOVSB source tail decommits after the first readable page");
    ctx = X86Context{};
    ctx.esi = 0xB3FFF;
    ctx.edi = 0xB5000;
    ctx.ecx = 2;
    SeedFlags(ctx);
    const SubFlags decommit_fault_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(ThrowsMemoryFault(sub_0001121F, ctx) && last_fault.instruction == 0x1121F &&
              last_fault.address == 0xB4000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && ctx.esi == 0xB4000 && ctx.edi == 0xB5001 && ctx.ecx == 1 &&
              MatchesFlags(ctx, decommit_fault_flags),
          "REP MOVSB decommit fault retains the single completed iteration");

    Check(mem::Commit(0xA1000, mem::kPage) && mem::Protect(0xA1000, mem::kPage, mem::kNoAccess) ==
              mem::AccessStatus::kOk,
          "REP STOSB destination protection fault fixture becomes inaccessible");
    ctx = X86Context{};
    ctx.edi = 0xA1000;
    ctx.ecx = 1;
    SeedFlags(ctx);
    const X86Context before_stos_fault = ctx;
    Check(ThrowsMemoryFault(sub_00011225, ctx) && last_fault.instruction == 0x11225 &&
              last_fault.address == 0xA1000 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.write && SameCoreState(ctx, before_stos_fault),
          "REP STOSB protection fault changes neither EDI, ECX, nor flags");
    Check(mem::Protect(0xA1000, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "REP STOSB destination protection restores");

    Check(mem::Commit(0xAB000, mem::kPage), "REP STOSD partial-fault destination page commits");
    ctx = X86Context{};
    ctx.eax = 0xCAFEBABE;
    ctx.edi = 0xABFF8;
    ctx.ecx = 3;
    SeedFlags(ctx);
    const SubFlags stosd_fault_flags{ctx.cf, ctx.pf, ctx.af, ctx.zf, ctx.sf, ctx.of};
    Check(ThrowsMemoryFault(sub_00011228, ctx) && last_fault.instruction == 0x11228 &&
              last_fault.address == 0xAC000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              last_fault.write && ctx.eax == 0xCAFEBABE && ctx.edi == 0xAC000 && ctx.ecx == 1 &&
              MatchesFlags(ctx, stosd_fault_flags) &&
              mem::Read(0xABFF8, copied_words, sizeof(copied_words)) == mem::AccessStatus::kOk &&
              copied_words[0] == 0xCAFEBABE && copied_words[1] == 0xCAFEBABE,
          "REP STOSD write fault preserves two completed dwords and reports exact next-page progress");

    Check(mem::Commit(0xA2000, mem::kPage) && mem::Write(0xA2000, input, 2) == mem::AccessStatus::kOk,
          "REP wrap fixture destination and source bytes initialize");
    Check(mem::Commit(0xFFFFF000, mem::kPage) && mem::Write(UINT32_MAX, input, 1) == mem::AccessStatus::kOk,
          "REP byte source commits at final guest page");
    ctx = X86Context{};
    ctx.esi = UINT32_MAX;
    ctx.edi = 0xA2000;
    ctx.ecx = 2;
    Check(ThrowsMemoryFault(sub_0001121F, ctx) && last_fault.instruction == 0x1121F &&
              last_fault.address == 0 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && ctx.esi == 0 && ctx.edi == 0xA2001 && ctx.ecx == 1,
          "REP MOVSB wraps 32-bit ESI and faults at zero after one completed iteration");

    constexpr uint32_t kpcr = 0xA3000, ethread = 0xA4000, stack_top = 0xA7000, tls = 0xA6000;
    Check(mem::Commit(kpcr, mem::kPage) && mem::Commit(ethread, mem::kPage) &&
              kernel::InitializeThreadGuestMemory(kpcr, ethread, stack_top, mem::kPage, tls, 0x1234) ==
                  mem::AccessStatus::kOk,
          "synthetic KPCR and ETHREAD thread memory initializes through production helper");
    ctx = X86Context{};
    ctx.fs_base = kpcr;
    Check(Runs(sub_00011259, ctx) && ctx.eax == (ethread & 0xFFFF),
          "generated FS-relative MOVZX retains the checked KPCR base");
    ctx = X86Context{};
    ctx.fs_base = kpcr;
    Check(Runs(sub_0001123F, ctx) && ctx.eax == ethread,
          "generated FS absolute lookup reads KPCR ETHREAD pointer");
    ctx.eax = ethread - kpcr;
    ctx.ecx = 0;
    Check(Runs(sub_00011246, ctx) && ctx.eax == tls,
          "generated FS base plus 32-bit base/index lookup reaches ETHREAD TLS");
    ctx = X86Context{};
    ctx.fs_base = kpcr;
    SeedFlags(ctx);
    const X86Context before_fs_fault = ctx;
    Check(mem::Protect(kpcr, mem::kPage, mem::kNoAccess) == mem::AccessStatus::kOk &&
              ThrowsMemoryFault(sub_0001123F, ctx) && last_fault.instruction == 0x1123F &&
              last_fault.address == kpcr + 0x28 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              !last_fault.write && SameCoreState(ctx, before_fs_fault),
          "generated FS fault reports checked KPCR address and preserves guest state");
    Check(mem::Protect(kpcr, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "synthetic KPCR page protection restores");

    const struct {
        X86Func* function;
        uint32_t pc;
    } unsupported[] = {{sub_0001122B, 0x1122B}, {sub_0001122D, 0x1122D}, {sub_00011230, 0x11230},
                       {sub_00011234, 0x11234}, {sub_00011238, 0x11238}, {sub_0001123C, 0x1123C},
                       {sub_0001124C, 0x1124C}, {sub_00011253, 0x11253}};
    for (const auto& test : unsupported) {
        ctx = X86Context{};
        SeedFlags(ctx);
        const X86Context before = ctx;
        Check(ThrowsTrap(test.function, ctx) && last_trap_pc == test.pc && SameCoreState(ctx, before),
              "unsupported REP prefix or FS/GS operand form remains trapped");
    }
}

void CheckProtectionExportFailClosedTests() {
    constexpr uint32_t stack = 0xA8000, page = 0xA9000, output = 0xAA000;
    Check(mem::Commit(stack, mem::kPage) && mem::Commit(page, mem::kPage) && mem::Commit(output, mem::kPage),
          "protection-export fixture stack, target, and output pages commit");
    struct Invocation {
        bool registered;
        bool trapped;
        uint32_t trap_pc;
        X86Context context;
    };
    auto invoke = [&](uint32_t ordinal, const std::array<uint32_t, 4>& args, uint32_t count) {
        std::array<uint32_t, 5> frame{};
        frame[0] = 0x12345678;
        std::copy_n(args.begin(), count, frame.begin() + 1);
        Check(mem::Write(stack, frame.data(), (count + 1) * sizeof(uint32_t)) == mem::AccessStatus::kOk,
              "protection-export stdcall frame initializes");
        X86Context ctx{};
        ctx.esp = stack;
        const bool registered = kernel::HasExport(ordinal);
        traps = 0;
        last_trap_pc = 0;
        if (registered) {
            try {
                x86_kernel(ctx, nullptr, ordinal);
            } catch (const FixtureStop&) {
            }
        }
        return Invocation{registered, traps == 1, last_trap_pc, ctx};
    };
    const auto query = invoke(179, {UINT32_MAX, 0, 0, 0}, 1);
    Check(query.registered && query.trapped && query.trap_pc == 0x12345678 && query.context.esp == stack,
          "MmQueryAddressProtect traps before interpreting its unverified protection contract");
    uint32_t marker = 0xAABBCCDD;
    Check(mem::Write(page, &marker, sizeof(marker)) == mem::AccessStatus::kOk,
          "protection-export target page sentinel initializes");
    const auto set = invoke(182, {page, mem::kPage, 2, 0}, 3);
    uint32_t marker_after = 0;
    Check(set.registered && set.trapped && set.trap_pc == 0x12345678 && set.context.esp == stack &&
              mem::Read(page, &marker_after, sizeof(marker_after)) == mem::AccessStatus::kOk &&
              marker_after == marker && mem::CheckAccess(page, sizeof(marker), mem::kWrite) == mem::AccessStatus::kOk,
          "MmSetAddressProtect traps without mutating the target page");
    const uint32_t old_protect_sentinel = 0x87654321;
    Check(mem::Write(output, &old_protect_sentinel, sizeof(old_protect_sentinel)) == mem::AccessStatus::kOk,
          "NtProtectVirtualMemory old-protection output sentinel initializes");
    const auto nt = invoke(204, {UINT32_MAX, UINT32_MAX, 0x40, output}, 4);
    uint32_t old_protect_after = 0;
    Check(nt.registered && nt.trapped && nt.trap_pc == 0x12345678 && nt.context.esp == stack &&
              mem::Read(output, &old_protect_after, sizeof(old_protect_after)) == mem::AccessStatus::kOk &&
              old_protect_after == old_protect_sentinel,
          "NtProtectVirtualMemory traps without dereferencing arguments or publishing old protection");
    Check(mem::Decommit(stack, mem::kPage), "protection-export fixture stack decommits");
    X86Context invalid_stack{};
    invalid_stack.esp = stack;
    bool checked_stack_fault = false;
    try {
        x86_kernel(invalid_stack, nullptr, 179);
    } catch (const X86MemoryFault& fault) {
        checked_stack_fault = fault.instruction == 0 && fault.address == stack && !fault.write &&
                              fault.status == mem::AccessStatus::kUncommitted;
    }
    Check(checked_stack_fault && invalid_stack.esp == stack,
          "unverified protection export uses a checked return-address read before trapping");
}

void CheckDispatchLeaseTests() {
    std::barrier lease_started(2), release_lease(2);
    std::atomic<bool> resolved{false}, reset_started{false}, reset_finished{false};
    std::thread active_call([&] {
        dispatch::CallLease lease;
        resolved.store(lease.Resolve(0x111B2).kind == dispatch::TargetKind::kGeneratedFunction);
        lease_started.arrive_and_wait();
        release_lease.arrive_and_wait();
    });
    lease_started.arrive_and_wait();
    std::thread resetter([&] {
        reset_started.store(true);
        dispatch::Reset();
        reset_finished.store(true);
    });
    while (!reset_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool reset_waited = !reset_finished.load();
    release_lease.arrive_and_wait();
    active_call.join();
    resetter.join();
    std::string error;
    Check(resolved.load() && reset_waited && reset_finished.load(),
          "dispatch reset waits until an active checked-call lease releases its resolved target");
    Check(dispatch::Init(error), "generated dispatch metadata restores after checked-call lease test");
}

void CheckEntryChainTests() {
    constexpr uint32_t stack = 0x7A000;
    Check(mem::Commit(stack, mem::kPage), "entry-chain fixture stack commits");

    X86Context ctx{};
    ctx.esp = stack + 0x100;
    ctx.eax = 0x11223344;
    ctx.ecx = 0x55667788;
    ctx.esi = 0x99AABBCC;
    SeedFlags(ctx);
    const uint32_t popped = 0xA1B2C3D4;
    Check(mem::Write(ctx.esp, &popped, sizeof(popped)) == mem::AccessStatus::kOk,
          "POP ECX stack input initializes");
    Check(Runs(sub_00011154, ctx) && ctx.ecx == popped && ctx.esp == stack + 0x108 && ctx.eax == 0x11223344 &&
              ctx.esi == 0x99AABBCC && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
              ctx.af == 1,
          "checked POP ECX reads dword, advances ESP after success, preserves other registers and flags");

    constexpr uint32_t pop_guard = 0x7C000;
    Check(mem::Commit(pop_guard + mem::kPage, mem::kPage), "POP fixture leaves lower guard page uncommitted");
    ctx = X86Context{};
    ctx.esp = pop_guard;
    ctx.ecx = 0xCAFEBABE;
    SeedFlags(ctx);
    const X86Context before_pop_fault = ctx;
    Check(ThrowsMemoryFault(sub_00011154, ctx) && last_fault.instruction == 0x11154 &&
              last_fault.address == pop_guard && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_pop_fault),
          "POP guard-page read fault reports guest PC and preserves ECX, ESP, and flags");

    constexpr uint32_t pop_cross = 0x7E000;
    Check(mem::Commit(pop_cross, mem::kPage), "POP fixture commits page below unreadable boundary");
    ctx = X86Context{};
    ctx.esp = pop_cross + mem::kPage - 2;
    ctx.ecx = 0x13579BDF;
    SeedFlags(ctx);
    const X86Context before_pop_cross_fault = ctx;
    Check(ThrowsMemoryFault(sub_00011154, ctx) && last_fault.instruction == 0x11154 &&
              last_fault.address == pop_cross + mem::kPage && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_pop_cross_fault),
          "POP cross-page read fault validates full dword before changing guest state");

    ctx = X86Context{};
    ctx.esp = stack + 0x180;
    ctx.eax = 0x12345678;
    Check(ThrowsTrap(sub_00011156, ctx) && last_trap_pc == 0x11156 && ctx.esp == stack + 0x180 &&
              ctx.eax == 0x12345678,
          "unsupported POP EAX stays blocked before stack access");

    struct CdqCase { uint32_t eax, edx; };
    for (const CdqCase test : {CdqCase{0, 0}, CdqCase{0x7FFFFFFF, 0}, CdqCase{0x80000000, UINT32_MAX}}) {
        ctx = X86Context{};
        ctx.eax = test.eax;
        ctx.edx = 0x12345678;
        SeedFlags(ctx);
        Check(Runs(sub_00011158, ctx) && ctx.eax == test.eax && ctx.edx == test.edx && ctx.cf == 1 && ctx.zf == 0 &&
                  ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 && ctx.af == 1,
              "checked CDQ sign-extends EAX into EDX without changing flags");
    }
    ctx = X86Context{};
    Check(ThrowsTrap(sub_00011160, ctx) && last_trap_pc == 0x11160,
          "CWD remains unsupported while checked CDQ is enabled");

    const auto set_dividend = [](X86Context& state, int64_t dividend) {
        const uint64_t bits = std::bit_cast<uint64_t>(dividend);
        state.eax = uint32_t(bits);
        state.edx = uint32_t(bits >> 32);
    };
    const auto divide = [&](int64_t dividend, int32_t divisor, uint32_t quotient, uint32_t remainder,
                            const char* message) {
        ctx = X86Context{};
        ctx.esp = stack + 0x200;
        set_dividend(ctx, dividend);
        ctx.ecx = std::bit_cast<uint32_t>(divisor);
        SeedFlags(ctx);
        Check(Runs(sub_0001115A, ctx) && ctx.eax == quotient && ctx.edx == remainder && ctx.ecx == std::bit_cast<uint32_t>(divisor) &&
                  ctx.esp == stack + 0x204 && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
                  ctx.af == 1,
              message);
    };
    divide(17, 5, 3, 2, "checked IDIV computes positive quotient and remainder");
    divide(-17, 5, std::bit_cast<uint32_t>(int32_t(-3)), std::bit_cast<uint32_t>(int32_t(-2)),
           "checked IDIV truncates negative dividend toward zero with signed remainder");
    divide(17, -5, std::bit_cast<uint32_t>(int32_t(-3)), 2,
           "checked IDIV handles negative divisor with signed quotient");
    divide(INT32_MIN, 1, std::bit_cast<uint32_t>(INT32_MIN), 0,
           "checked IDIV accepts exact INT32_MIN quotient");

    const auto divide_trap = [&](int64_t dividend, int32_t divisor, const char* reason_fragment, const char* message) {
        ctx = X86Context{};
        ctx.esp = stack + 0x280;
        set_dividend(ctx, dividend);
        ctx.ecx = std::bit_cast<uint32_t>(divisor);
        SeedFlags(ctx);
        const X86Context before = ctx;
        Check(ThrowsTrap(sub_0001115A, ctx) && last_trap_pc == 0x1115A &&
                  last_trap_reason.find(reason_fragment) != std::string::npos && SameCoreState(ctx, before),
              message);
    };
    divide_trap(1, 0, "division by zero", "checked IDIV zero divisor stops at guest PC and preserves guest state");
    divide_trap(INT32_MIN, -1, "quotient overflow", "checked IDIV detects INT32_MIN divided by -1 overflow");
    divide_trap(int64_t(INT32_MIN) - 1, 1, "quotient overflow", "checked IDIV rejects negative quotient outside int32 range");
    divide_trap(int64_t(INT32_MAX) + 1, 1, "quotient overflow", "checked IDIV rejects positive quotient outside int32 range");
    divide_trap(INT64_MIN, -1, "quotient overflow", "checked IDIV guards signed 64-bit host division overflow");
    Check(ThrowsTrap(sub_00011163, ctx) && last_trap_pc == 0x11163,
          "memory-source IDIV remains rejected in checked mode");

    ctx = X86Context{};
    ctx.esp = stack + 0x300;
    ctx.eax = 0x12345678;
    ctx.esi = 0xCAFEBABE;
    SeedFlags(ctx);
    Check(Runs(sub_0001115D, ctx) && ctx.esi == 0 && ctx.eax == 0x12345678 && ctx.esp == stack + 0x304 && ctx.cf == 0 &&
              ctx.of == 0 && ctx.zf == 1 && ctx.sf == 0 && ctx.pf == 1 && ctx.af == 1,
          "checked XOR ESI,ESI sets defined flags and preserves undefined AF");
}

void CheckCalleeInstructionTests() {
    constexpr uint32_t stack = 0x8A000;
    constexpr uint32_t operands = 0x8B000;
    Check(mem::Commit(stack, mem::kPage) && mem::Commit(operands, mem::kPage) &&
              mem::Commit(0x12000, mem::kPage),
          "callee instruction stack and memory operands commit");

    uint32_t expected = 0x1122AABB;
    Check(mem::Write(operands + 0x14, &expected, sizeof(expected)) == mem::AccessStatus::kOk,
          "PUSH [EBP+displacement] source initializes");
    X86Context ctx{};
    ctx.esp = stack + 0x100;
    ctx.ebp = operands;
    SeedFlags(ctx);
    Check(Runs(sub_0001116D, ctx) && ctx.esp == stack + 0x100 &&
              mem::Read(stack + 0xFC, &expected, sizeof(expected)) == mem::AccessStatus::kOk && expected == 0x1122AABB &&
              ctx.cf == 1 && ctx.af == 1,
          "checked PUSH dword [EBP+0x14] reads before stack publication");

    expected = 0xAABBCCDD;
    Check(mem::Write(0x12000, &expected, sizeof(expected)) == mem::AccessStatus::kOk,
          "absolute PUSH source initializes");
    ctx = X86Context{};
    ctx.esp = stack + 0x120;
    SeedFlags(ctx);
    Check(Runs(sub_00011171, ctx) && ctx.esp == stack + 0x120 &&
              mem::Read(stack + 0x11C, &expected, sizeof(expected)) == mem::AccessStatus::kOk && expected == 0xAABBCCDD,
          "checked PUSH dword absolute memory uses checked guest read and stack write");

    expected = 0x76543210;
    ctx = X86Context{};
    ctx.esp = stack + 0x140;
    Check(mem::Write(ctx.esp, &expected, sizeof(expected)) == mem::AccessStatus::kOk,
          "PUSH [ESP] pre-decrement source initializes");
    SeedFlags(ctx);
    Check(Runs(sub_00011178, ctx) && ctx.esp == stack + 0x140 &&
              mem::Read(stack + 0x13C, &expected, sizeof(expected)) == mem::AccessStatus::kOk && expected == 0x76543210,
          "checked PUSH [ESP] reads using old ESP before decrementing stack");

    struct TestCase { uint32_t value; AndFlags flags; };
    for (const TestCase test : {TestCase{0, ReferenceAndFlags(0, 0, 32)},
                                TestCase{0x80000001, ReferenceAndFlags(0x80000001, 0x80000001, 32)},
                                TestCase{3, ReferenceAndFlags(3, 3, 32)}}) {
        ctx = X86Context{};
        ctx.eax = test.value;
        SeedFlags(ctx);
        Check(Runs(sub_00011166, ctx) && ctx.eax == test.value && MatchesAndFlags(ctx, test.flags) && ctx.af == 1,
              "checked TEST EAX,EAX updates defined flags and preserves undefined AF");
    }

    struct ShiftCase {
        X86Func* function;
        uint32_t input, result, cf, of, zf, sf, pf;
    };
    const ShiftCase shifts[] = {
        {sub_00011169, 0xD0000003, 0x34000000, 1, 1, 0, 0, 1},
        {sub_00011169, 3, 0, 1, 1, 1, 0, 1},
        {sub_0001118E, 0x80000003, 0x40000001, 1, 1, 0, 0, 0},
        {sub_00011192, 0xA5, 0xA5, 1, 1, 0, 1, 0},
        {sub_00011196, 0x80000000, 1, 0, 1, 0, 0, 0},
        {sub_0001119A, 0x12345678, 0x12345678, 1, 1, 0, 1, 0},
    };
    for (const ShiftCase test : shifts) {
        ctx = X86Context{};
        if (test.function == sub_00011192) ctx.ebx = test.input;
        else if (test.function == sub_00011169) ctx.ecx = test.input;
        else ctx.eax = test.input;
        SeedFlags(ctx);
        Check(Runs(test.function, ctx) &&
                  (test.function == sub_00011192 ? ctx.ebx : test.function == sub_00011169 ? ctx.ecx : ctx.eax) == test.result &&
                  ctx.cf == test.cf && ctx.of == test.of && ctx.zf == test.zf && ctx.sf == test.sf && ctx.pf == test.pf &&
                  ctx.af == 1,
              "checked SHR immediate handles count, result, defined flags, and preserved undefined flags");
    }

    ctx = X86Context{};
    ctx.eax = 0x80000000;
    ctx.ecx = 0x12345678;
    ctx.esp = stack + 0x180;
    SeedFlags(ctx);
    const X86Context before_unsupported_shift = ctx;
    Check(ThrowsTrap(sub_0001117F, ctx) && last_trap_pc == 0x1117F && SameCoreState(ctx, before_unsupported_shift),
          "SHR ECX,CL remains unsupported without changing guest state");
    Check(ThrowsTrap(sub_0001117C, ctx) && last_trap_pc == 0x1117C,
          "memory-source TEST remains unsupported in checked mode");
    Check(ThrowsTrap(sub_00011182, ctx) && last_trap_pc == 0x11182,
          "memory-destination SHR remains unsupported in checked mode");
    Check(ThrowsTrap(sub_00011186, ctx) && last_trap_pc == 0x11186,
          "16-bit memory-source PUSH remains unsupported in checked mode");
    Check(ThrowsTrap(sub_0001118A, ctx) && last_trap_pc == 0x1118A,
          "segment-overridden memory-source PUSH remains unsupported in checked mode");
}

void CheckPushLeaAndTests() {
    constexpr uint32_t stack = 0x60000;
    Check(mem::Commit(stack, mem::kPage), "checked PUSH fixture stack page commits");
    X86Context ctx{};
    ctx.esp = stack + 0x104;
    ctx.esi = 0xAABBCCDD;
    SeedFlags(ctx);
    sub_000110AB(ctx, nullptr);
    uint32_t word = 0;
    Check(ctx.esp == stack + 0x104 && mem::Read(stack + 0x100, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == 0xAABBCCDD && ctx.cf == 1 && ctx.af == 1,
          "PUSH register decrements 32-bit ESP, writes little-endian dword, and RET restores ESP");

    ctx = X86Context{};
    ctx.esp = stack + 0x114;
    Check(Runs(sub_000110AD, ctx) && mem::Read(stack + 0x110, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == UINT32_MAX - 3,
          "PUSH imm8 sign-extends -4 to a 32-bit stack value");
    ctx = X86Context{};
    ctx.esp = stack + 0x124;
    Check(Runs(sub_000110B0, ctx) && mem::Read(stack + 0x120, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == 0x12345678,
          "PUSH imm32 writes full little-endian value");
    ctx = X86Context{};
    ctx.esp = stack + 0x134;
    const uint32_t esp_before = ctx.esp;
    Check(Runs(sub_000110B6, ctx) && mem::Read(stack + 0x130, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == esp_before,
          "PUSH ESP stores pre-decrement ESP value");
    ctx = X86Context{};
    ctx.esp = stack + 0x150;
    ctx.esi = 0x10203040;
    Check(Runs(sub_000110BB, ctx) && ctx.esp == stack + 0x14C &&
              mem::Read(stack + 0x148, &word, sizeof(word)) == mem::AccessStatus::kOk && word == UINT32_MAX - 3 &&
              mem::Read(stack + 0x14C, &word, sizeof(word)) == mem::AccessStatus::kOk && word == 0x10203040,
          "repeated PUSH operations preserve LIFO stack contents");

    constexpr uint32_t guard = 0x70000;
    Check(mem::Commit(guard, mem::kPage), "PUSH guard fixture commits only upper stack page");
    ctx = X86Context{};
    ctx.esp = guard + 4;
    ctx.esi = 0x55667788;
    Check(Runs(sub_000110AB, ctx) && mem::Read(guard, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == 0x55667788,
          "PUSH succeeds at committed stack boundary");

    const auto check_push_fault = [&](uint32_t esp, uint32_t address, mem::AccessStatus status, const char* message) {
        X86Context failed{};
        failed.esp = esp;
        failed.esi = 0xCAFEBABE;
        SeedFlags(failed);
        const X86Context before = failed;
        Check(ThrowsMemoryFault(sub_000110AB, failed) && last_fault.instruction == 0x110AB &&
                  last_fault.address == address && last_fault.status == status && last_fault.write &&
                  SameCoreState(failed, before), message);
    };
    check_push_fault(guard, guard - 4, mem::AccessStatus::kUncommitted,
                     "PUSH guard-page fault leaves ESP, registers, and flags unchanged");
    check_push_fault(guard + 2, guard - 2, mem::AccessStatus::kUncommitted,
                     "PUSH cross-page fault validates full dword before changing ESP");
    Check(mem::Protect(guard, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "PUSH read-only stack fixture protection set");
    check_push_fault(guard + 4, guard, mem::AccessStatus::kPermissionDenied,
                     "PUSH denied stack write preserves architectural state");
    Check(mem::Protect(guard, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "PUSH stack protection restored");

    constexpr uint32_t push_source = 0x63000;
    Check(mem::Commit(push_source, mem::kPage), "memory-source PUSH operand page commits");
    const uint32_t source_value = 0xDEADC0DE;
    Check(mem::Write(push_source, &source_value, sizeof(source_value)) == mem::AccessStatus::kOk,
          "memory-source PUSH operand initializes");
    ctx = X86Context{};
    ctx.esp = stack + 0x180;
    ctx.esi = push_source;
    SeedFlags(ctx);
    Check(Runs(sub_000110B8, ctx) && ctx.esp == stack + 0x180 &&
              mem::Read(ctx.esp - 4, &word, sizeof(word)) == mem::AccessStatus::kOk && word == source_value &&
              ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 && ctx.af == 1,
          "checked PUSH dword [ESI] reads source, writes stack, then RET restores ESP");

    ctx = X86Context{};
    ctx.esp = stack + 0x184;
    ctx.esi = 0x24000;
    SeedFlags(ctx);
    const X86Context before_memory_push_read_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110B8, ctx) && last_fault.instruction == 0x110B8 &&
              last_fault.address == 0x24000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_memory_push_read_fault),
          "memory-source PUSH read fault preserves stack and reports read direction");

    constexpr uint32_t push_guard = 0x88000;
    Check(mem::Commit(push_guard + mem::kPage, mem::kPage), "memory-source PUSH leaves destination guard uncommitted");
    ctx = X86Context{};
    ctx.esp = push_guard + 4;
    ctx.esi = push_source;
    SeedFlags(ctx);
    const X86Context before_memory_push_write_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110B8, ctx) && last_fault.instruction == 0x110B8 &&
              last_fault.address == push_guard && last_fault.status == mem::AccessStatus::kUncommitted &&
              last_fault.write && SameCoreState(ctx, before_memory_push_write_fault),
          "memory-source PUSH stack-write fault preserves registers and ESP after checked source read");

    ctx = X86Context{};
    ctx.eax = 0xFFFFFFF0;
    ctx.ecx = 0x30;
    SeedFlags(ctx);
    const uint32_t old_flags = ctx.cf | ctx.zf << 1 | ctx.sf << 2 | ctx.of << 3 | ctx.pf << 4 | ctx.af << 5;
    Check(Runs(sub_000110BF, ctx) && ctx.eax == 0x2F &&
              (ctx.cf | ctx.zf << 1 | ctx.sf << 2 | ctx.of << 3 | ctx.pf << 4 | ctx.af << 5) == old_flags,
          "LEA computes base/index/displacement with 32-bit wrap and preserves flags without reading memory");
    ctx = X86Context{};
    ctx.ebx = 0x1000;
    ctx.esi = 3;
    Check(Runs(sub_000110C4, ctx) && ctx.eax == 0x1008,
          "LEA applies SIB scale and signed displacement");
    ctx = X86Context{};
    ctx.ebx = 0x1234FFFF;
    Check(Runs(sub_000110C9, ctx) && ctx.eax == 0,
          "LEA address-size override wraps effective address to 16 bits");

    ctx = X86Context{};
    ctx.eax = 0xF0F0F0F0;
    ctx.ebx = 0x0F0F0F0F;
    SeedFlags(ctx);
    AndFlags flags = ReferenceAndFlags(ctx.eax, ctx.ebx, 32);
    const uint32_t old_af = ctx.af;
    Check(Runs(sub_000110CE, ctx) && ctx.eax == 0 && MatchesAndFlags(ctx, flags) && ctx.af == old_af,
          "AND register form updates defined flags and preserves undefined AF");
    ctx = X86Context{};
    ctx.eax = UINT32_MAX;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(UINT32_MAX, UINT32_MAX - 127, 32);
    Check(Runs(sub_000110D1, ctx) && ctx.eax == UINT32_MAX - 127 && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND sign-extends imm8 -128 to dword result and flags");
    ctx = X86Context{};
    ctx.eax = 0xABCD00A5;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(0xA5, 0xF0, 8);
    Check(Runs(sub_000110DB, ctx) && ctx.eax == 0xABCD00A0 && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND byte immediate preserves upper register bits and undefined AF");
    ctx = X86Context{};
    ctx.eax = 0xAAAAFFFF;
    ctx.ebx = 0xBBBB000F;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(0xFFFF, 0x000F, 16);
    Check(Runs(sub_000110DE, ctx) && ctx.eax == 0xAAAA000F && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND word register form preserves upper register bits and undefined AF");

    constexpr uint32_t and_memory = 0x62000;
    Check(mem::Commit(and_memory, mem::kPage), "AND memory fixture page commits");
    uint32_t memory = 0xF0F0F0F0;
    Check(mem::Write(and_memory, &memory, sizeof(memory)) == mem::AccessStatus::kOk,
          "AND memory destination initializes");
    ctx = X86Context{};
    ctx.esi = and_memory;
    ctx.eax = 0x0FF00FF0;
    SeedFlags(ctx);
    const uint32_t expected_memory = memory & ctx.eax;
    flags = ReferenceAndFlags(memory, ctx.eax, 32);
    Check(Runs(sub_000110D5, ctx) && mem::Read(and_memory, &memory, sizeof(memory)) == mem::AccessStatus::kOk &&
              memory == expected_memory && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND register to memory writes result and updates defined flags");

    memory = 0x800000F0;
    Check(mem::Write(and_memory + 4, &memory, sizeof(memory)) == mem::AccessStatus::kOk,
          "AND memory source initializes");
    ctx = X86Context{};
    ctx.esi = and_memory + 4;
    ctx.eax = UINT32_MAX;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(ctx.eax, memory, 32);
    Check(Runs(sub_000110D8, ctx) && ctx.eax == memory && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND memory to register checks source read and sets flags");

    uint32_t byte_memory = UINT32_MAX;
    Check(mem::Write(and_memory + 8, &byte_memory, sizeof(byte_memory)) == mem::AccessStatus::kOk,
          "AND memory immediate initializes");
    ctx = X86Context{};
    ctx.esi = and_memory + 8;
    flags = ReferenceAndFlags(UINT32_MAX, 0x0F, 32);
    Check(Runs(sub_000110E2, ctx) && mem::Read(and_memory + 8, &memory, sizeof(memory)) == mem::AccessStatus::kOk &&
              memory == 0x0F && MatchesAndFlags(ctx, flags),
          "AND memory immediate uses checked read-modify-write");

    memory = 0x13579BDF;
    Check(mem::Write(and_memory + 0x10, &memory, sizeof(memory)) == mem::AccessStatus::kOk,
          "AND denied-write sentinel initializes");
    Check(mem::Protect(and_memory, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "AND destination page made read-only");
    ctx = X86Context{};
    ctx.esi = and_memory + 0x10;
    ctx.eax = 0xFFFFFFFF;
    SeedFlags(ctx);
    const X86Context before_and_write_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110D5, ctx) && last_fault.instruction == 0x110D5 &&
              last_fault.address == and_memory + 0x10 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.write && SameCoreState(ctx, before_and_write_fault) &&
              mem::Read(and_memory + 0x10, &memory, sizeof(memory)) == mem::AccessStatus::kOk &&
              memory == 0x13579BDF,
          "AND denied write preserves memory, destination registers, and prior flags");
    Check(mem::Protect(and_memory, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "AND destination protection restored");

    ctx = X86Context{};
    ctx.eax = 0xF0;
    ctx.ebx = 0x0F;
    ctx.ecx = 0xAABBCCDD;
    ctx.esi = 0x24000;
    SeedFlags(ctx);
    Check(ThrowsMemoryFault(sub_000110E6, ctx) && last_fault.instruction == 0x110E8 &&
              last_fault.address == 0x24000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && ctx.eax == 0 && ctx.ecx == 0xAABBCCDD && ctx.cf == 0 && ctx.of == 0 &&
              ctx.zf == 1 && ctx.sf == 0 && ctx.pf == 1 && ctx.af == 1,
          "AND memory fault retains prior instruction results and flags in context");

    ctx = X86Context{};
    ctx.eax = 0xF0;
    ctx.ebx = 0x0F;
    ctx.esp = 0x24004;
    ctx.esi = 0xCAFEBABE;
    SeedFlags(ctx);
    Check(ThrowsMemoryFault(sub_000110EB, ctx) && last_fault.instruction == 0x110ED &&
              last_fault.address == 0x24000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              last_fault.write && ctx.eax == 0 && ctx.esp == 0x24004 && ctx.cf == 0 && ctx.zf == 1 &&
              ctx.sf == 0 && ctx.of == 0 && ctx.pf == 1 && ctx.af == 1,
          "PUSH fault preserves stack pointer and preceding AND flags");

    ctx = X86Context{};
    ctx.eax = 0x12345678;
    ctx.esi = UINT32_MAX;
    SeedFlags(ctx);
    const X86Context before_and_read_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110D8, ctx) && last_fault.instruction == 0x110D8 &&
              last_fault.address == UINT32_MAX && last_fault.status == mem::AccessStatus::kInvalidAddress &&
              !last_fault.write && SameCoreState(ctx, before_and_read_fault),
          "AND invalid source span preserves guest state");
}

}  // namespace

[[noreturn]] void x86_memory_fault(X86Context&, const X86MemoryFault& fault) {
    last_fault = fault;
    ++faults;
    throw FixtureStop{};
}

[[noreturn]] void x86_checked_trap(X86Context&, uint32_t pc, const char* reason) {
    last_trap_pc = pc;
    last_trap_reason = reason;
    ++traps;
    throw FixtureStop{};
}

extern "C" int RunCheckedRecompilerFixtureTests() {
    static_assert(sizeof(uint64_t) == 8);
    Check(mem::Init(), "production guest memory initializes");
    if (!mem::IsInitialized()) return 1;
    std::string dispatch_error;
    Check(dispatch::Init(dispatch_error), "generated function map initializes production dispatch table");

    constexpr uint32_t source = 0x20FFE, destination = 0x30000;
    Check(mem::Commit(0x20000, 0x2000), "source pages commit independently at 4 KB granularity");
    Check(mem::Commit(destination, mem::kPage), "destination page commits");
    std::array<uint8_t, 24> input{};
    input[0] = 0x80;
    input[1] = 0x22;
    input[2] = 0x33;
    input[3] = 0x44;
    input[4] = 0x80;
    input[5] = 0x01;
    input[6] = 0x80;
    for (size_t i = 8; i < 16; ++i) input[i] = uint8_t(i * 17);
    Check(mem::Write(source, input.data(), input.size()) == mem::AccessStatus::kOk,
          "fixture input crosses a 4 KB guest-page boundary");

    X86Context ctx{};
    ctx.ebx = source;
    ctx.edi = destination;
    ctx.esi = source;
    ctx.ecx = 0xA1B2;
    ctx.cf = 1;
    ctx.zf = 0;
    ctx.sf = 1;
    ctx.of = 1;
    ctx.pf = 0;
    sub_00011000(ctx, nullptr);

    uint32_t dword = 0;
    std::array<uint8_t, 16> output{};
    Check(mem::Read(destination, &dword, sizeof(dword)) == mem::AccessStatus::kOk && dword == 0x44332280,
          "generated 32 bit load and store preserve little-endian value across pages");
    Check(mem::Read(destination, output.data(), output.size()) == mem::AccessStatus::kOk &&
              output[4] == 0x80 && output[5] == 0xB2 && output[6] == 0xA1,
          "generated 8 and 16 bit stores use correct widths");
    Check(ctx.ecx == 0x80 && ctx.edx == 0xFFFFFF80 && ctx.ebx == 0x8001 && ctx.ebp == 0xFFFF8001,
          "generated signed and unsigned byte and word loads extend correctly");
    uint64_t expected_qword = 0;
    std::memcpy(&expected_qword, input.data() + 8, sizeof(expected_qword));
    Check(ctx.mm[0] == expected_qword,
          "generated 64 bit MMX load uses checked guest memory");
    uint64_t qword = 0;
    Check(mem::Read(destination + 8, &qword, sizeof(qword)) == mem::AccessStatus::kOk && qword == ctx.mm[0],
          "generated 64 bit MMX store uses checked guest memory");
    Check(ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0,
          "scalar MOV instructions preserve guest flags");

    ctx = X86Context{};
    ctx.ebx = UINT32_MAX - 2;
    ctx.edi = destination;
    ctx.esi = source;
    ctx.eax = 0xDEADBEEF;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.instruction == 0x11000 &&
              last_fault.address == UINT32_MAX - 2 && last_fault.status == mem::AccessStatus::kInvalidAddress &&
              !last_fault.write && ctx.eax == 0xDEADBEEF,
          "generated overflow read reports faulting PC without changing destination register");

    ctx = X86Context{};
    ctx.ebx = 0x24000;
    ctx.edi = destination;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.instruction == 0x11000 &&
              last_fault.status == mem::AccessStatus::kUncommitted && !last_fault.write,
          "generated uncommitted read halts at instruction address");

    Check(mem::Protect(source & ~(mem::kPage - 1), mem::kPage, mem::kNoAccess) == mem::AccessStatus::kOk,
          "source page becomes inaccessible");
    ctx = X86Context{};
    ctx.ebx = source;
    ctx.edi = destination;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.instruction == 0x11000 && !last_fault.write,
          "generated denied read reports access direction and PC");
    Check(mem::Protect(source & ~(mem::kPage - 1), mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "source page becomes readable again");

    Check(mem::Protect(destination, mem::kPage, mem::kNoAccess) == mem::AccessStatus::kOk,
          "destination page becomes inaccessible");
    ctx = X86Context{};
    ctx.ebx = source;
    ctx.edi = destination;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.instruction == 0x11002 && last_fault.write,
          "generated denied write reports access direction and PC");
    Check(mem::Protect(destination, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "destination page protection restored");

    uint16_t prefix_before = 0;
    Check(mem::Write(0x30FFE, &prefix_before, sizeof(prefix_before)) == mem::AccessStatus::kOk,
          "partial destination prefix initializes");
    const uint16_t prefix = 0xBEEF;
    Check(mem::Write(0x30FFE, &prefix, sizeof(prefix)) == mem::AccessStatus::kOk,
          "destination prefix sentinel writes");
    ctx = X86Context{};
    ctx.ebx = source;
    ctx.edi = 0x30FFE;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.instruction == 0x11002 &&
              last_fault.address == 0x31000 && last_fault.status == mem::AccessStatus::kUncommitted && last_fault.write,
          "generated cross-page store rejects uncommitted tail at the store PC");
    uint16_t prefix_after = 0;
    Check(mem::Read(0x30FFE, &prefix_after, sizeof(prefix_after)) == mem::AccessStatus::kOk && prefix_after == prefix,
          "rejected generated cross-page store leaves committed prefix unchanged");

    const uint32_t rmw_va = destination + 0x20;
    const uint32_t maximum = UINT32_MAX;
    Check(mem::Write(rmw_va, &maximum, sizeof(maximum)) == mem::AccessStatus::kOk,
          "RMW input initialized");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    ctx.ecx = 0xCAFE;
    sub_00011022(ctx, nullptr);
    uint32_t rmw_result = 1;
    Check(mem::Read(rmw_va, &rmw_result, sizeof(rmw_result)) == mem::AccessStatus::kOk && rmw_result == 0 &&
              ctx.ecx == 0xCAFE,
          "generated memory ADD updates data and preserves zero-flag branch behavior");

    const uint32_t read_only_value = 41;
    Check(mem::Write(rmw_va, &read_only_value, sizeof(read_only_value)) == mem::AccessStatus::kOk &&
              mem::Protect(rmw_va & ~(mem::kPage - 1), mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "RMW target becomes read-only");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    ctx.ecx = 0xCAFE;
    Check(ThrowsMemoryFault(sub_00011022, ctx) && last_fault.instruction == 0x11022 &&
              last_fault.status == mem::AccessStatus::kPermissionDenied && last_fault.write && ctx.ecx == 0xCAFE &&
              ctx.cf == 0 && ctx.zf == 0 && ctx.sf == 0 && ctx.of == 0 && ctx.pf == 0,
          "generated RMW write fault leaves later instruction, flags, and guest registers untouched");
    Check(mem::Protect(rmw_va & ~(mem::kPage - 1), mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "RMW target protection restored");

    uint32_t rmw_value = 0;
    Check(mem::Write(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk,
          "SUB input initialized");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    sub_0001104A(ctx, nullptr);
    Check(mem::Read(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk && rmw_value == UINT32_MAX &&
              ctx.cf == 1 && ctx.sf == 1 && ctx.zf == 0,
          "generated memory SUB commits result and flags after checked store");

    rmw_value = UINT32_MAX;
    Check(mem::Write(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk,
          "INC input initialized");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    ctx.cf = 1;
    sub_0001104E(ctx, nullptr);
    Check(mem::Read(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk && rmw_value == 0 &&
              ctx.cf == 1 && ctx.zf == 1,
          "generated memory INC commits result while preserving carry flag");

    CheckCmpTests(destination);
    CheckPushLeaAndTests();
    CheckCallTests(destination);
    CheckEntryChainTests();
    CheckCalleeInstructionTests();
    CheckIndirectCallTests();
    CheckKernel255AbiAndPreparationTests();
    CheckPostCallInstructionTests();
    CheckNotRepAndFsTests();
    CheckProtectionExportFailClosedTests();
    CheckDispatchLeaseTests();

    ctx = X86Context{};
    ctx.edi = destination;
    Check(ThrowsTrap(sub_0001102D, ctx) && last_trap_pc == 0x1102D &&
              last_trap_reason.find("unimplemented: xchg") != std::string::npos,
          "unsupported atomic memory instruction reaches checked-safe trap path");
    Check(ThrowsTrap(sub_00011030, ctx) && last_trap_pc == 0x11030 &&
              last_trap_reason.find("unimplemented:") != std::string::npos,
          "16-byte SIMD memory operation remains blocked in checked mode");
    Check(ThrowsTrap(sub_00011034, ctx) && last_trap_pc == 0x11034 &&
              last_trap_reason.find("unimplemented:") != std::string::npos,
          "10-byte x87 memory operation remains blocked in checked mode");
    Check(ThrowsTrap(sub_00011037, ctx) && last_trap_pc == 0x11037 &&
              last_trap_reason.find("unimplemented:") != std::string::npos,
          "locked XADD remains blocked in checked mode");
    Check(Runs(sub_0001103C, ctx) && ctx.ecx == 0 && ctx.esp == 4,
          "checked REP MOVSD zero-count fixture completes without guest memory access");

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE_CLI
    RunReleaseBenchmarks(source, destination);
#endif

    CheckGuestCallbackContract();
    CheckGuestThreadLifecycle();
    mem::Shutdown();
    return failures ? 1 : 0;
}

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE_CLI
int main() { return RunCheckedRecompilerFixtureTests(); }
#endif
