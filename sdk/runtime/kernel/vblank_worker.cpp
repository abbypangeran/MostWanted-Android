#include "kernel/vblank_worker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace kernel {
namespace {

std::atomic<bool> g_started{false};
#ifdef MOSTWANTED_VBLANK_RUNTIME_TESTS
std::atomic<VBlankCallbackInvoker> g_callback_invoker{nullptr};
#endif

GuestCallbackStatus InvokeCallback(X86Context& ctx, const GuestCallbackTicket& ticket, uint32_t target,
                                   std::span<const uint32_t> arguments, uint32_t stack_base, uint32_t stack_size,
                                   uint32_t& result) {
#ifdef MOSTWANTED_VBLANK_RUNTIME_TESTS
    if (auto invoker = g_callback_invoker.load())
        return invoker(ctx, ticket, target, arguments, stack_base, stack_size, result);
#endif
    return CallGuestCallback(ctx, ticket, target, arguments, stack_base, stack_size, result);
}

void RunVBlank(X86Context& ctx, WorkerStopToken stop, VBlankWorkerConfig config) {
    using clock = std::chrono::steady_clock;
    struct ResetStarted {
        ~ResetStarted() { g_started.store(false); }
    } reset_started;
    std::mutex wait_lock;
    std::condition_variable wait_changed;
    WorkerStopCallback wake(stop, [&] { wait_changed.notify_all(); });
    if (!wake.Ready()) return;
    const uint32_t info = mem::AllocSystem(12);
    if (!info) return;
    struct FreeInfo {
        uint32_t va;
        ~FreeInfo() { mem::FreeSystem(va); }
    } free_info{info};
    uint32_t shown = 0;
    auto wait_until = [&](clock::time_point deadline) {
        std::unique_lock lock(wait_lock);
        return wait_changed.wait_until(lock, deadline, [&] { return stop.StopRequested(); });
    };
    auto reset_event = [&](const ObjectRef& event) {
        std::lock_guard<std::recursive_mutex> lock(*config.device_lock);
        mem::ScopedPageLease device_lease;
        if (device_lease.Acquire(config.device, config.device_size, mem::kReadWrite) == mem::AccessStatus::kOk)
            ResetEvent(*event);
    };
    auto next = clock::now();
    for (;;) {
        while (config.focus_paused && config.focus_paused->load()) {
            if (wait_until(clock::now() + std::chrono::milliseconds(20))) return;
            next = clock::now();
        }
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        if (wait_until(next)) return;
        uint32_t count = 0, swaps = 0, callback = 0;
        ObjectRef event;
        {
            std::lock_guard<std::recursive_mutex> lock(*config.device_lock);
            mem::ScopedPageLease device_lease;
            if (device_lease.Acquire(config.device, config.device_size, mem::kReadWrite) != mem::AccessStatus::kOk ||
                mem::Read(config.device + config.count_offset, &count, sizeof(count)) != mem::AccessStatus::kOk ||
                mem::Read(config.device + config.swaps_offset, &swaps, sizeof(swaps)) != mem::AccessStatus::kOk ||
                mem::Read(config.device + config.callback_offset, &callback, sizeof(callback)) !=
                    mem::AccessStatus::kOk)
                continue;
            ++count;
            if (mem::Write(config.device + config.count_offset, &count, sizeof(count)) != mem::AccessStatus::kOk)
                continue;
            event = FromPointer(config.device + config.event_offset);
            if (!event) continue;
            SetEvent(*event);
        }
        if (callback && !stop.StopRequested()) {
            GuestCallbackTicket ticket;
            if (PrepareGuestCallback(callback, GuestCallbackClass::kVBlank, ticket) == GuestCallbackStatus::kReady) {
                const std::array<uint32_t, 3> values{count, swaps, uint32_t(swaps != shown)};
                mem::ScopedPageLease info_lease;
                if (info_lease.Acquire(info, sizeof(values), mem::kReadWrite) == mem::AccessStatus::kOk &&
                    mem::Write(info, values.data(), sizeof(values)) == mem::AccessStatus::kOk) {
                    const std::array<uint32_t, 1> arguments{info};
                    uint32_t result = 0;
                    InvokeCallback(ctx, ticket, callback, arguments, 0, 0, result);
                }
            }
        }
        shown = swaps;
        if (wait_until(clock::now() + std::chrono::milliseconds(1))) {
            reset_event(event);
            return;
        }
        reset_event(event);
    }
}

}  // namespace

bool StartVBlankWorker(const char* name, const VBlankWorkerConfig& config) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return true;
    if (!config.device_lock || !config.device_size ||
        !StartManagedHostThread(name, [config](X86Context& ctx, WorkerStopToken stop) {
            RunVBlank(ctx, stop, config);
        })) {
        g_started.store(false);
        return false;
    }
    return true;
}

#ifdef MOSTWANTED_VBLANK_RUNTIME_TESTS
void SetVBlankCallbackInvokerForTest(VBlankCallbackInvoker invoker) { g_callback_invoker.store(invoker); }
#endif

}  // namespace kernel
