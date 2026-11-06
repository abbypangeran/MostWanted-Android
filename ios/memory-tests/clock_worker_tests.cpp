#include "kernel/clock_worker.h"
#include "kernel/kernel_data_init.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

int failures = 0;
int write_calls = 0;
int fail_write_at = 0;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

bool WaitUntil(const auto& predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

struct SlowUpdate {
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<int> calls{0};
};

bool SlowTick(void* context) {
    auto& state = *static_cast<SlowUpdate*>(context);
    const int active = ++state.active;
    int maximum = state.maximum.load();
    while (maximum < active && !state.maximum.compare_exchange_weak(maximum, active)) {}
    ++state.calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    --state.active;
    return true;
}

struct BlockingUpdate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

bool BlockedTick(void* context) {
    auto& state = *static_cast<BlockingUpdate*>(context);
    std::unique_lock lock(state.mutex);
    state.entered = true;
    state.changed.notify_all();
    state.changed.wait(lock, [&] { return state.release; });
    return true;
}

bool FailTick(void*) { return false; }

struct GuestUpdate {
    uint32_t address = 0;
    std::atomic<uint32_t> calls{0};
};

bool WriteGuestTick(void* context) {
    auto& state = *static_cast<GuestUpdate*>(context);
    const uint32_t value = ++state.calls;
    return mem::Write(state.address, &value, sizeof(value)) == mem::AccessStatus::kOk;
}

mem::AccessStatus FailWriteOnce(uint32_t address, const void* source, size_t size) {
    if (++write_calls == fail_write_at) return mem::AccessStatus::kHostError;
    return mem::Write(address, source, size);
}

kernel::KernelDataLayout TestLayout() {
    constexpr uint32_t base = 0x70000;
    return {base, base + 0x100, base + 0x200, base + 0x300, base + 0x400, base + 0x500,
            base + 0x600, base + 0x700, base + 0x800, base + 0x900, base + 0xA00};
}

void CheckKernelDataInitialization() {
    Check(mem::Init() && mem::Commit(0x70000, mem::kPage), "kernel-data initialization fixture memory commits");
    constexpr uint32_t base = 0x70000;
    Check(mem::Fill(base, 0xA5, mem::kPage) == mem::AccessStatus::kOk, "kernel-data fixture starts with sentinels");
    std::array<uint8_t, mem::kPage> before{}, after{};
    Check(mem::Read(base, before.data(), before.size()) == mem::AccessStatus::kOk, "kernel-data sentinel snapshot reads");

    write_calls = 0;
    fail_write_at = 6;
    const kernel::KernelDataMemory injected{nullptr, FailWriteOnce};
    std::string error;
    Check(!kernel::InitializeKernelData(TestLayout(), error, injected) && !error.empty() &&
              mem::Read(base, after.data(), after.size()) == mem::AccessStatus::kOk && after == before &&
              !kernel::KernelClockWorkerRunning(),
          "failed synchronous kernel-data initialization rolls back writes and starts no worker");
    fail_write_at = 0;

    const auto layout = TestLayout();
    Check(kernel::InitializeKernelData(layout, error), "synchronous kernel-data initialization succeeds without a worker");
    uint32_t value = UINT32_MAX;
    std::array<uint8_t, 6> hardware{};
    std::array<uint16_t, 4> version{};
    std::array<uint32_t, 2> queue{};
    std::array<uint8_t, 8 + 26 + 1> image_descriptor{};
    Check(mem::Read(layout.launch_data_page, &value, sizeof(value)) == mem::AccessStatus::kOk && value == 0 &&
              mem::Read(layout.hardware_info, hardware.data(), hardware.size()) == mem::AccessStatus::kOk &&
              hardware == std::array<uint8_t, 6>{0x20, 0, 0, 0, 0xD3, 0xB1} &&
              mem::Read(layout.kernel_version, version.data(), sizeof(version)) == mem::AccessStatus::kOk &&
              version == std::array<uint16_t, 4>{1, 0, 5838, 1} &&
              mem::Read(layout.ide_channel_object + 0x28, queue.data(), sizeof(queue)) == mem::AccessStatus::kOk &&
              queue == std::array<uint32_t, 2>{layout.ide_channel_object + 0x28, layout.ide_channel_object + 0x28} &&
              mem::Read(layout.image_file_name, image_descriptor.data(), image_descriptor.size()) == mem::AccessStatus::kOk &&
              std::string(reinterpret_cast<char*>(image_descriptor.data() + 8)) == "\\Device\\CdRom0\\default.xbe",
          "kernel-data fields and ANSI descriptors match production layout");
    Check(kernel::InitializeKernelData(layout, error) && !kernel::KernelClockWorkerRunning(),
          "repeated synchronous initialization remains safe and worker-free");

    auto invalid = layout;
    invalid.image_file_name = UINT32_MAX - 4;
    Check(!kernel::InitializeKernelData(invalid, error) && mem::Read(base, after.data(), after.size()) == mem::AccessStatus::kOk &&
              after != before,
          "invalid kernel-data address fails before replacing initialized state");
    mem::Shutdown();
}

void CheckLocalLifecycle() {
    kernel::ClockWorker worker;
    SlowUpdate slow;
    Check(worker.Start(SlowTick, &slow) && worker.Start(SlowTick, &slow) && WaitUntil([&] { return slow.calls >= 2; }),
          "repeated clock start reuses one worker");
    Check(worker.Stop() && !worker.Running() && slow.maximum == 1, "clock stop joins worker without duplicate updates");
    const int stopped_calls = slow.calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    Check(slow.calls == stopped_calls, "joined clock worker performs no stale updates");

    Check(worker.Start(FailTick, nullptr) && WaitUntil([&] { return !worker.Running(); }) && worker.Failed() && !worker.Stop(),
          "clock update failure ends worker and is reported by stop");
    SlowUpdate restarted;
    Check(worker.Start(SlowTick, &restarted) && WaitUntil([&] { return restarted.calls > 0; }) && worker.Stop(),
          "clock worker can restart cleanly after failure");

    BlockingUpdate blocked;
    Check(worker.Start(BlockedTick, &blocked), "blocking clock worker starts for concurrent-stop test");
    {
        std::unique_lock lock(blocked.mutex);
        blocked.changed.wait_for(lock, std::chrono::seconds(1), [&] { return blocked.entered; });
    }
    std::atomic<int> completed = 0;
    std::thread first([&] { worker.Stop(); ++completed; });
    std::thread second([&] { worker.Stop(); ++completed; });
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const bool both_waited = completed == 0;
    {
        std::lock_guard lock(blocked.mutex);
        blocked.release = true;
    }
    blocked.changed.notify_all();
    first.join();
    second.join();
    Check(both_waited && completed == 2 && !worker.Running(), "concurrent stops wait for in-flight update then join once");

    bool repeated = true;
    for (int i = 0; i < 3; ++i) repeated &= worker.Start(SlowTick, &restarted) && worker.Stop();
    Check(repeated && !worker.Running(), "repeated clock startup and teardown stay deterministic");
}

void CheckMemoryShutdownStopsGlobalWorker() {
    Check(mem::Init(), "guest memory starts for clock shutdown integration");
    constexpr uint32_t address = 0x6A000;
    Check(mem::Commit(address, mem::kPage), "clock output page commits");
    GuestUpdate update{address};
    Check(kernel::StartKernelClockWorker(WriteGuestTick, &update) &&
              WaitUntil([&] { return update.calls.load() >= 2; }),
          "global clock worker writes checked guest memory");
    mem::Shutdown();
    const uint32_t stopped_calls = update.calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    Check(!kernel::KernelClockWorkerRunning() && update.calls.load() == stopped_calls,
          "guest-memory shutdown stops and joins clock before unmapping");
}

void CheckConcurrentStartAndMemoryShutdown() {
    Check(mem::Init(), "guest memory starts for concurrent clock-start/shutdown test");
    GuestUpdate update{0x68000};
    Check(mem::Commit(update.address, mem::kPage), "concurrent clock output page commits");
    std::barrier gate(4);
    std::thread starter([&] {
        gate.arrive_and_wait();
        kernel::StartKernelClockWorker(WriteGuestTick, &update);
    });
    std::thread shutdown([&] {
        gate.arrive_and_wait();
        mem::Shutdown();
    });
    std::thread second_shutdown([&] {
        gate.arrive_and_wait();
        mem::Shutdown();
    });
    gate.arrive_and_wait();
    starter.join();
    shutdown.join();
    second_shutdown.join();
    const uint32_t calls_after_shutdown = update.calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    const bool stopped = !kernel::KernelClockWorkerRunning() && !mem::IsInitialized() &&
                         update.calls.load() == calls_after_shutdown;
    Check(stopped, "memory shutdown serializes against clock start and joins every accepted worker");
    Check(mem::Init(), "guest memory can initialize after concurrent shutdown");
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    Check(update.calls.load() == calls_after_shutdown,
          "a stopped clock cannot write into a later guest-memory mapping");
    mem::Shutdown();
    GuestUpdate after_shutdown{update.address};
    Check(!kernel::StartKernelClockWorker(WriteGuestTick, &after_shutdown) &&
              !kernel::KernelClockWorkerRunning() && after_shutdown.calls.load() == 0,
          "clock start is rejected after guest-memory shutdown");
}

}  // namespace

extern "C" int RunClockWorkerTests() {
    CheckLocalLifecycle();
    CheckKernelDataInitialization();
    CheckMemoryShutdownStopsGlobalWorker();
    CheckConcurrentStartAndMemoryShutdown();
    return failures ? 1 : 0;
}
