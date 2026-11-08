#include "cpu/dispatch_table.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/vblank_worker.h"
#include "x86_recomp_shared.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include <unistd.h>

namespace kernel {
uint8_t* g_base = nullptr;
}

namespace {

constexpr uint32_t kDevice = 0x08000000;
constexpr uint32_t kCount = 0x20;
constexpr uint32_t kSwaps = 0x24;
constexpr uint32_t kEvent = 0x28;
constexpr uint32_t kCallback = 0x40;
constexpr uint32_t kCode = 0x90000;
constexpr uint32_t kTarget = kCode + 0x10;

std::recursive_mutex g_device_lock;
std::atomic<bool> g_focus_paused{true};
std::mutex g_callback_lock;
std::condition_variable g_callback_changed;
bool g_callback_entered = false;
bool g_release_callback = false;
std::atomic<uint32_t> g_callback_calls{0};
std::atomic<uint32_t> g_fake_calls{0};
std::atomic<kernel::GuestCallbackStatus> g_callback_status{kernel::GuestCallbackStatus::kCancelled};
std::array<uint32_t, 3> g_callback_info{};
uint32_t g_callback_count = 0, g_callback_swaps = 0, g_event_state = 0;
bool g_callback_frame_ok = false;
int g_failures = 0;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++g_failures;
}

void SyntheticGuestCallback(X86Context&, uint8_t*) { ++g_fake_calls; }

kernel::GuestCallbackStatus CallbackSubstitute(X86Context& ctx, const kernel::GuestCallbackTicket& ticket,
                                               uint32_t target, std::span<const uint32_t> arguments,
                                               uint32_t stack_base, uint32_t stack_size, uint32_t& result) {
    ++g_callback_calls;
    uint32_t info = arguments.size() == 1 ? arguments[0] : 0;
    g_callback_frame_ok = arguments.size() == 1 && !stack_base && !stack_size;
    g_callback_frame_ok &= mem::Read(info, g_callback_info.data(), sizeof(g_callback_info)) == mem::AccessStatus::kOk;
    g_callback_frame_ok &= mem::Read(kDevice + kCount, &g_callback_count, sizeof(g_callback_count)) ==
                           mem::AccessStatus::kOk;
    g_callback_frame_ok &= mem::Read(kDevice + kSwaps, &g_callback_swaps, sizeof(g_callback_swaps)) ==
                           mem::AccessStatus::kOk;
    g_callback_frame_ok &= mem::Read(kDevice + kEvent + 4, &g_event_state, sizeof(g_event_state)) ==
                           mem::AccessStatus::kOk;
    g_callback_status = kernel::CallGuestCallbackForTest(ctx, ticket, target, arguments, stack_base, stack_size,
                                                         result);
    {
        std::unique_lock lock(g_callback_lock);
        g_callback_entered = true;
        g_callback_changed.notify_all();
        g_callback_changed.wait(lock, [] { return g_release_callback; });
    }
    return g_callback_status.load();
}

bool WaitForCount(uint32_t expected, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        uint32_t count = 0;
        if (mem::Read(kDevice + kCount, &count, sizeof(count)) == mem::AccessStatus::kOk && count >= expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool InitializeDevice(uint32_t host_page, uint32_t count, uint32_t swaps, uint32_t callback) {
    std::array<uint32_t, 17> fields{};
    fields[kCount / 4] = count;
    fields[kSwaps / 4] = swaps;
    fields[kEvent / 4] = 0;  // Notification event.
    fields[kCallback / 4] = callback;
    return mem::Commit(kDevice, host_page) &&
           mem::Write(kDevice, fields.data(), sizeof(fields)) == mem::AccessStatus::kOk;
}

kernel::VBlankWorkerConfig Config() {
    return {kDevice, 0x80, kCount, kSwaps, kEvent, kCallback, &g_device_lock, &g_focus_paused};
}

}  // namespace

int main() {
    const long host_page_long = sysconf(_SC_PAGESIZE);
    Check(host_page_long > 0 && (uint32_t(host_page_long) % mem::kPage) == 0,
          "host page size is a whole number of guest pages");
    if (host_page_long <= 0 || uint32_t(host_page_long) % mem::kPage) return 1;
    const uint32_t host_page = uint32_t(host_page_long);

    Check(mem::Init(), "guest memory initializes for compiled VBlank runtime");
#ifdef __APPLE__
    kernel::g_base = nullptr;
#else
    kernel::g_base = mem::Base();
#endif
    std::string error;
    const std::array<X86FuncMapping, 1> mappings = {{{kTarget, SyntheticGuestCallback}}};
    const std::array<uint32_t, 1> continuations = {kTarget + 4};
    const dispatch::Metadata metadata{kCode, kCode + mem::kPage, kTarget, mappings, continuations};
    Check(mem::Commit(kCode, mem::kPage) &&
              mem::Protect(kCode, mem::kPage, mem::kRead | mem::kExecute) == mem::AccessStatus::kOk &&
              dispatch::Init(metadata, error) && kernel::RestartKernelWorkers(),
          "VBlank callback map and managed worker gate initialize");
    Check(InitializeDevice(host_page, 5, 42, kTarget), "synthetic D3D device, event, and callback fields initialize");
    kernel::ObjectRef event = kernel::FromPointer(kDevice + kEvent);
    Check(bool(event), "synthetic VBlank event binds to a production kernel object");
    kernel::SetVBlankCallbackInvokerForTest(CallbackSubstitute);

    kernel::FailNextKernelWorkerStartForTest();
    Check(!kernel::StartVBlankWorker("vblank runtime test", Config()) &&
              kernel::StartVBlankWorker("vblank runtime test", Config()),
          "compiled VBlank worker creation failure rolls back and permits retry");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    uint32_t paused_count = 0;
    Check(mem::Read(kDevice + kCount, &paused_count, sizeof(paused_count)) == mem::AccessStatus::kOk &&
              paused_count == 5 && g_callback_calls.load() == 0,
          "focus pause suspends VBlank count and callback delivery");

    g_focus_paused.store(false);
    bool callback_entered = false;
    {
        std::unique_lock lock(g_callback_lock);
        callback_entered = g_callback_changed.wait_for(lock, std::chrono::seconds(3), [] { return g_callback_entered; });
    }
    Check(callback_entered && g_callback_frame_ok && g_callback_count == 6 && g_callback_swaps == 42 &&
              g_callback_info == std::array<uint32_t, 3>{6, 42, 1} && g_event_state == 1 &&
              g_callback_status.load() == kernel::GuestCallbackStatus::kInvalidStack && g_fake_calls.load() == 0,
          "compiled VBlank updates count, signals event, publishes frame data, and rejects callback without guest stack");

    const uint32_t aligned_device = kDevice & ~(host_page - 1);
    Check(mem::Protect(aligned_device, host_page, mem::kRead) == mem::AccessStatus::kOk &&
              mem::Protect(aligned_device, host_page, mem::kReadWrite) == mem::AccessStatus::kOk &&
              mem::Decommit(aligned_device, host_page) && mem::Commit(aligned_device, host_page),
          "device page protection and decommit are safe while worker is outside device access");
    std::atomic<bool> stopped{false};
    std::thread stop_thread([&] {
        kernel::StopKernelWorkers();
        stopped = true;
    });
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (kernel::KernelWorkersAccepting() && std::chrono::steady_clock::now() < stop_deadline)
        std::this_thread::yield();
    Check(!stopped.load(), "worker shutdown waits for the active test callback substitute");
    {
        std::lock_guard lock(g_callback_lock);
        g_release_callback = true;
    }
    g_callback_changed.notify_all();
    stop_thread.join();
    Check(stopped.load() && g_callback_calls.load() == 1 && g_fake_calls.load() == 0,
          "cancellation joins VBlank worker without a second callback");
    kernel::Unbind(event);

    g_release_callback = false;
    g_callback_entered = false;
    g_focus_paused.store(false);
    Check(mem::Fill(kDevice, 0, host_page) == mem::AccessStatus::kOk &&
              mem::Write(kDevice + kSwaps, &paused_count, sizeof(paused_count)) == mem::AccessStatus::kOk &&
              kernel::RestartKernelWorkers() && kernel::StartVBlankWorker("vblank repeat test", Config()),
          "VBlank worker restarts against a fresh device cycle");
    kernel::ObjectRef second_event = kernel::FromPointer(kDevice + kEvent);
    Check(bool(second_event), "restarted VBlank cycle binds its own kernel event object");
    Check(WaitForCount(1, std::chrono::seconds(3)), "restarted VBlank loop updates device count");
    Check(kernel::StopKernelWorkers(), "repeated VBlank cycle joins cleanly");
    kernel::Unbind(second_event);

    g_focus_paused.store(true);
    Check(kernel::RestartKernelWorkers() && kernel::StartVBlankWorker("vblank memory shutdown test", Config()),
          "VBlank worker starts before guest-memory shutdown");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::atomic<bool> memory_shutdown_done{false};
    std::thread memory_shutdown([&] {
        mem::Shutdown();
        memory_shutdown_done = true;
    });
    const auto shutdown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (kernel::KernelWorkersAccepting() && std::chrono::steady_clock::now() < shutdown_deadline)
        std::this_thread::yield();
    memory_shutdown.join();
    Check(memory_shutdown_done.load() && !kernel::KernelWorkersAccepting() && g_fake_calls.load() == 0,
          "guest-memory shutdown stops and joins VBlank before unmapping pages");
    return g_failures ? 1 : 0;
}
