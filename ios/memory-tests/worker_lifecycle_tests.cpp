#include "kernel/worker_lifecycle.h"
#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
#include "kernel/kernel.h"
#endif

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel/memory.h"

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
#include "cpu/dispatch_table.h"
#endif

namespace {

int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool WaitFor(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, const auto& ready) {
    return changed.wait_for(lock, std::chrono::seconds(3), ready);
}

bool WaitFor(const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
}

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
struct ThreadWaitTest {
    std::mutex lock;
    std::condition_variable changed;
    kernel::ThreadRef thread;
    bool entered = false;
    bool wait_blocked = false;
    bool suspend_observed = false;
    bool release_suspend_hook = false;
    bool apc_observed = false;
    bool release_apc_hook = false;
    std::atomic<uint32_t> result{UINT32_MAX};
    std::atomic<bool> returned{false};
};

std::atomic<ThreadWaitTest*> g_thread_wait_test{nullptr};
std::atomic<kernel::Thread*> g_thread_wait_target{nullptr};

void WaitBlocked(kernel::Thread* thread) {
    auto* state = g_thread_wait_test.load();
    if (!state || g_thread_wait_target.load() != thread) return;
    std::lock_guard lock(state->lock);
    state->wait_blocked = true;
    state->changed.notify_all();
}

void WaitSuspended(kernel::Thread* thread) {
    auto* state = g_thread_wait_test.load();
    if (!state || g_thread_wait_target.load() != thread) return;
    std::unique_lock lock(state->lock);
    state->suspend_observed = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->release_suspend_hook; });
}

void WaitApcPending(kernel::Thread* thread) {
    auto* state = g_thread_wait_test.load();
    if (!state || g_thread_wait_target.load() != thread) return;
    std::unique_lock lock(state->lock);
    state->apc_observed = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->release_apc_hook; });
}

bool StartWaitWorker(ThreadWaitTest& state, kernel::ObjectRef event, bool alertable) {
    return kernel::StartManagedHostThread("synthetic-wait-test", [&state, event = std::move(event), alertable](
                                             X86Context&, kernel::WorkerStopToken stop) {
        kernel::ThreadRef thread = kernel::CurrentRef();
        kernel::WorkerStopCallback wake(stop, [thread] {
            thread->stop_requested = true;
            {
                std::lock_guard lock(thread->apc_lock);
                thread->apcs.clear();
            }
            thread->resume_cv.notify_all();
            kernel::SignalChanged();
        });
        {
            std::lock_guard lock(state.lock);
            state.thread = thread;
            state.entered = true;
            g_thread_wait_target.store(thread.get());
            state.changed.notify_all();
        }
        if (!wake.Ready()) {
            state.result = kernel::kUnsuccessful;
        } else {
            state.result = kernel::Wait({event}, false, INT64_MAX, alertable);
        }
        state.returned = true;
        state.changed.notify_all();
    });
}
#endif

struct BlockingWorker {
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false;
    bool stop_seen = false;
    bool release = false;
};

void CheckStopJoinAndCreationFailure() {
    kernel::WorkerLifecycle workers;
    workers.FailNextStartForTest();
    Check(!workers.Start([](kernel::WorkerStopToken) {}) && workers.OwnedCount() == 0,
          "worker creation failure releases registration state");

    BlockingWorker blocked;
    Check(workers.Start([&](kernel::WorkerStopToken stop) {
              kernel::WorkerStopCallback wake(stop, [&] {
                  std::lock_guard lock(blocked.lock);
                  blocked.stop_seen = true;
                  blocked.changed.notify_all();
              });
              std::unique_lock lock(blocked.lock);
              blocked.entered = true;
              blocked.changed.notify_all();
              blocked.changed.wait(lock, [&] { return blocked.release; });
          }),
          "synthetic production worker starts");
    {
        std::unique_lock lock(blocked.lock);
        Check(WaitFor(blocked.changed, lock, [&] { return blocked.entered; }), "worker reaches synthetic barrier");
    }
    workers.BeginShutdown();
    Check(!workers.Accepting() && !workers.Start([](kernel::WorkerStopToken) {}),
          "shutdown rejects later worker registration");
    std::atomic<bool> joined = false;
    std::thread joiner([&] { joined = workers.Join(); });
    bool stop_seen = false;
    {
        std::unique_lock lock(blocked.lock);
        stop_seen = WaitFor(blocked.changed, lock, [&] { return blocked.stop_seen; });
    }
    Check(stop_seen && !joined.load(), "join waits after stop while the worker callback remains active");
    {
        std::lock_guard lock(blocked.lock);
        blocked.release = true;
    }
    blocked.changed.notify_all();
    joiner.join();
    Check(joined.load() && workers.OwnedCount() == 0,
          "stop joins active worker and releases owned resources");
}

void CheckCancellationAndRepeatSetup() {
    kernel::WorkerLifecycle workers;
    std::atomic<int> dispatched = 0;
    std::atomic<int> cancelled_before_dispatch = 0;
    workers.HoldActivationForTest(true);
    Check(workers.Start([&](kernel::WorkerStopToken stop) {
              if (stop.StopRequested()) ++cancelled_before_dispatch;
              else ++dispatched;
          }),
          "worker can be prepared behind activation barrier");
    workers.BeginShutdown();
    const bool joined = workers.Join();
    workers.HoldActivationForTest(false);
    Check(joined && cancelled_before_dispatch == 1 && dispatched == 0,
          "cancellation before activation skips callback dispatch");

    for (int cycle = 0; cycle < 3; ++cycle) {
        Check(workers.StartAccepting(), "joined worker set accepts a fresh runtime cycle");
        std::barrier entered(2);
        Check(workers.Start([&](kernel::WorkerStopToken) {
                  ++dispatched;
                  entered.arrive_and_wait();
              }),
              "worker starts after runtime restart");
        entered.arrive_and_wait();
        Check(workers.StopAndJoin(), "repeated worker setup and teardown joins cleanly");
    }
    Check(dispatched == 3 && workers.OwnedCount() == 0,
          "repeated lifecycle leaves no owned worker resources");
}

void CheckSuspendedAndQueuedCancellation() {
    kernel::WorkerLifecycle workers;
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false;
    int queued = 1;
    int dispatched = 0;
    bool woke_for_stop = false;
    Check(workers.Start([&](kernel::WorkerStopToken stop) {
              kernel::WorkerStopCallback wake(stop, [&] { changed.notify_all(); });
              if (!wake.Ready()) return;
              std::unique_lock guard(lock);
              entered = true;
              changed.notify_all();
              changed.wait(guard, [&] { return stop.StopRequested(); });
              woke_for_stop = true;
              queued = 0;
              if (!stop.StopRequested() && queued) ++dispatched;
          }),
          "suspended synthetic worker starts");
    {
        std::unique_lock guard(lock);
        Check(WaitFor(changed, guard, [&] { return entered; }), "worker reaches suspended wait");
    }
    Check(workers.StopAndJoin() && woke_for_stop && queued == 0 && dispatched == 0,
          "shutdown wakes suspended worker and cancels queued work");
}

void CheckConcurrentSubmissionAndExit() {
    kernel::WorkerLifecycle workers;
    std::barrier start(5);
    std::atomic<int> accepted = 0;
    std::atomic<int> completed = 0;
    std::vector<std::thread> submitters;
    for (int i = 0; i < 4; ++i) {
        submitters.emplace_back([&] {
            start.arrive_and_wait();
            for (int n = 0; n < 4; ++n) {
                if (!workers.Start([&](kernel::WorkerStopToken) { ++completed; })) break;
                ++accepted;
            }
        });
    }
    start.arrive_and_wait();
    workers.BeginShutdown();
    for (auto& submitter : submitters) submitter.join();
    Check(workers.Join() && !workers.Accepting() && workers.OwnedCount() == 0 && completed <= accepted,
          "concurrent shutdown and submission drain only accepted work");

    kernel::WorkerLifecycle exiting;
    std::barrier exit_gate(2);
    Check(exiting.Start([&](kernel::WorkerStopToken) { exit_gate.arrive_and_wait(); }),
          "worker starts for concurrent-exit shutdown");
    exit_gate.arrive_and_wait();
    Check(exiting.StopAndJoin() && exiting.OwnedCount() == 0,
          "worker exit during shutdown is joined exactly once");

    kernel::WorkerLifecycle self_join;
    std::atomic<bool> self_join_rejected = false;
    Check(self_join.Start([&](kernel::WorkerStopToken) { self_join_rejected = !self_join.StopAndJoin(); }),
          "worker starts for self-join rejection test");
    Check(self_join.Join() && self_join_rejected && self_join.OwnedCount() == 0,
          "owned worker cannot self-join while external shutdown waits");
}

void CheckExitedWorkersAreReapedBeforeNewRegistration() {
    kernel::WorkerLifecycle workers;
    std::atomic<int> completed = 0;
    Check(workers.Start([&](kernel::WorkerStopToken) { ++completed; }), "short-lived worker starts for reaping test");
    Check(WaitFor([&] { return completed == 1 && workers.QuiescingCountForTest() == 1; }),
          "short-lived worker reaches quiescence");
    bool reaped = true;
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::barrier entered(2);
        const bool started = workers.Start([&](kernel::WorkerStopToken) {
            entered.arrive_and_wait();
            ++completed;
        });
        if (!started) {
            reaped = false;
            break;
        }
        reaped &= workers.OwnedCount() == 1;
        entered.arrive_and_wait();
        reaped &= WaitFor([&] { return completed == cycle + 2 && workers.QuiescingCountForTest() == 1; });
    }
    Check(reaped && workers.StopAndJoin() && workers.OwnedCount() == 0,
          "new registration joins and releases naturally exited workers");

    kernel::WorkerLifecycle child_workers;
    std::atomic<int> child_exits = 0;
    std::atomic<bool> parent_reaped = true;
    Check(child_workers.Start([&](kernel::WorkerStopToken) {
              for (int cycle = 0; cycle < 3; ++cycle) {
                  if (!child_workers.Start([&](kernel::WorkerStopToken) { ++child_exits; })) {
                      parent_reaped = false;
                      return;
                  }
                  const bool exited = WaitFor([&] {
                      return child_exits == cycle + 1 && child_workers.QuiescingCountForTest() == 1;
                  });
                  if (!exited) {
                      parent_reaped = false;
                      return;
                  }
              }
              parent_reaped = child_workers.OwnedCount() == 2;
          }),
          "managed worker starts child workers for churn test");
    Check(WaitFor([&] { return child_exits == 3 && child_workers.QuiescingCountForTest() >= 1; }) &&
              child_workers.StopAndJoin() && parent_reaped && child_workers.OwnedCount() == 0,
          "managed worker registrations reap completed child threads without self-join");
}

void CheckManagedReapingConcurrentWithJoin() {
    kernel::WorkerLifecycle workers;
    workers.HoldAfterReapForTest(true);
    std::atomic<int> child_exits = 0;
    std::atomic<bool> next_start = true;
    Check(workers.Start([&](kernel::WorkerStopToken) {
              if (!workers.Start([&](kernel::WorkerStopToken) { ++child_exits; })) {
                  next_start = false;
                  return;
              }
              if (!WaitFor([&] { return child_exits == 1 && workers.QuiescingCountForTest() == 1; })) {
                  next_start = false;
                  return;
              }
              next_start = workers.Start([](kernel::WorkerStopToken) {});
          }),
          "managed parent worker starts a short-lived child for join-race test");
    Check(WaitFor([&] { return workers.JoiningCountForTest() == 1; }),
          "managed registration reaches the reaper join barrier");
    std::atomic<bool> joined = false;
    std::thread joiner([&] { joined = workers.Join(); });
    const bool join_waiting = WaitFor([&] { return workers.JoinWaiterCountForTest() == 1; });
    workers.HoldAfterReapForTest(false);
    joiner.join();
    Check(join_waiting && joined.load() && !next_start.load() && workers.OwnedCount() == 0,
          "external shutdown waits for managed reaping without duplicate join or stale worker access");
}

void CheckStartDoesNotHoldGuestRangeBehindJoin() {
    kernel::WorkerLifecycle workers;
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false;
    bool stop_requested = false;
    bool release_worker = false;
    bool start_finished = false;
    bool shutdown_finished = false;
    bool start_succeeded = true;
    bool shutdown_joined = false;
    Check(workers.Start([&](kernel::WorkerStopToken stop) {
              kernel::WorkerStopCallback wake(stop, [&] {
                  {
                      std::lock_guard guard(lock);
                      stop_requested = true;
                  }
                  changed.notify_all();
              });
              std::unique_lock guard(lock);
              entered = true;
              changed.notify_all();
              changed.wait(guard, [&] { return release_worker; });
              guard.unlock();
              mem::ScopedVirtualRangeLock guest_owner_lock;
          }),
          "worker enters a finalizer barrier protected by the guest range lock");
    {
        std::unique_lock guard(lock);
        Check(changed.wait_for(guard, std::chrono::seconds(5), [&] { return entered; }),
              "range-lock worker reaches the shutdown barrier");
    }
    std::thread shutdown([&] {
        shutdown_joined = workers.StopAndJoin();
        {
            std::lock_guard guard(lock);
            shutdown_finished = true;
        }
        changed.notify_all();
    });
    {
        std::unique_lock guard(lock);
        Check(changed.wait_for(guard, std::chrono::seconds(5), [&] { return stop_requested; }),
              "shutdown requests stop before worker join");
    }
    Check(workers.WaitForJoinInProgressForTest(std::chrono::seconds(5)),
          "shutdown owns the worker join while the finalizer is held");
    std::thread starter([&] {
        mem::ScopedVirtualRangeLock guest_range_lock;
        start_succeeded = workers.Start([](kernel::WorkerStopToken) {});
        {
            std::lock_guard guard(lock);
            start_finished = true;
        }
        changed.notify_all();
    });
    bool start_returned = false;
    {
        std::unique_lock guard(lock);
        start_returned = changed.wait_for(guard, std::chrono::seconds(5), [&] { return start_finished; });
    }
    if (!start_returned) {
        std::fprintf(stderr, "FAIL: worker start blocked behind shutdown join while holding guest range lock\n");
        std::abort();
    }
    {
        std::lock_guard guard(lock);
        release_worker = true;
    }
    changed.notify_all();
    starter.join();
    shutdown.join();
    Check(!start_succeeded && shutdown_joined && shutdown_finished && workers.OwnedCount() == 0,
          "concurrent worker start rejects promptly without blocking guest resource finalization");
}

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
void CheckProductionWaitSuspensionAndApcShutdown() {
    Check(mem::Init(), "guest memory initializes for synthetic thread-wait tests");
    if (!mem::IsInitialized()) return;

    const uint32_t code_base = X86CodeBase & ~(mem::kPage - 1);
    const uint32_t code_end = (X86CodeEnd + mem::kPage - 1) & ~(mem::kPage - 1);
    std::string dispatch_error;
    const bool callback_map_ready = mem::Commit(code_base, code_end - code_base) &&
                                    mem::Protect(code_base, code_end - code_base, mem::kRead | mem::kExecute) ==
                                        mem::AccessStatus::kOk &&
                                    dispatch::Init(dispatch_error);
    Check(callback_map_ready, "generated fixture callback map initializes for cancellation tests");
    if (!callback_map_ready) return;
    const uint32_t callback_target = X86EntryPoint;

    ThreadWaitTest suspended;
    auto event = std::make_shared<kernel::Object>(kernel::ObjType::Event);
    g_thread_wait_test.store(&suspended);
    kernel::SetWaitBlockedHookForTest(WaitBlocked);
    kernel::SetWaitSuspendHookForTest(WaitSuspended);
    Check(StartWaitWorker(suspended, event, false), "managed synthetic thread starts an unsignaled wait");
    {
        std::unique_lock lock(suspended.lock);
        Check(WaitFor(suspended.changed, lock, [&] { return suspended.entered && suspended.wait_blocked; }),
              "synthetic thread reaches the production dispatcher wait");
    }
    kernel::ThreadRef target = suspended.thread;
    Check(target && kernel::Suspend(*target) == 0, "dispatcher-wait target accepts suspension");
    {
        std::unique_lock lock(suspended.lock);
        Check(WaitFor(suspended.changed, lock, [&] { return suspended.suspend_observed; }),
              "dispatcher wait observes suspension and yields to SuspendPoint");
    }
    kernel::SetEvent(*event);
    {
        std::lock_guard lock(suspended.lock);
        suspended.release_suspend_hook = true;
    }
    suspended.changed.notify_all();
    bool held_suspended = false;
    {
        std::unique_lock lock(suspended.lock);
        held_suspended = !suspended.changed.wait_for(lock, std::chrono::milliseconds(50), [&] {
            return suspended.returned.load();
        });
    }
    kernel::Resume(*target);
    {
        std::unique_lock lock(suspended.lock);
        Check(WaitFor(suspended.changed, lock, [&] { return suspended.returned.load(); }),
              "resumed synthetic thread leaves its wait");
    }
    Check(held_suspended && suspended.result == kernel::kWait0,
          "dispatcher wait does not consume a signaled object until the thread resumes");
    Check(WaitFor([&] { return target->state.load() == kernel::ThreadState::kExited; }) &&
              !kernel::QueueApc(*target, callback_target, 0, 0, 0),
          "APC registration rejects a thread after its completion transition");
    Check(kernel::StopKernelWorkers() && kernel::RestartKernelWorkers(),
          "suspended-wait test reaps its managed thread before runtime reuse");
    kernel::SetWaitBlockedHookForTest(nullptr);
    kernel::SetWaitSuspendHookForTest(nullptr);
    g_thread_wait_target.store(nullptr);
    g_thread_wait_test.store(nullptr);

    ThreadWaitTest apc;
    auto apc_event = std::make_shared<kernel::Object>(kernel::ObjType::Event);
    g_thread_wait_test.store(&apc);
    kernel::SetWaitBlockedHookForTest(WaitBlocked);
    kernel::SetWaitApcHookForTest(WaitApcPending);
    Check(StartWaitWorker(apc, apc_event, true), "managed synthetic thread starts an alertable wait");
    {
        std::unique_lock lock(apc.lock);
        Check(WaitFor(apc.changed, lock, [&] { return apc.entered && apc.wait_blocked; }),
              "alertable thread reaches the production dispatcher wait");
    }
    target = apc.thread;
    Check(target && kernel::QueueApc(*target, callback_target, 0, 0, 0), "synthetic APC queues before shutdown");
    {
        std::unique_lock lock(apc.lock);
        Check(WaitFor(apc.changed, lock, [&] { return apc.apc_observed; }),
              "alertable wait observes the queued APC before dispatch");
    }
    std::atomic<bool> stopped = false;
    std::thread stopper([&] { stopped = kernel::StopKernelWorkers(); });
    const bool shutdown_started = WaitFor([&] { return !kernel::KernelWorkersAccepting(); });
    const bool cancelled = WaitFor([&] { return target->stop_requested.load(); });
    {
        std::lock_guard lock(apc.lock);
        apc.release_apc_hook = true;
    }
    apc.changed.notify_all();
    stopper.join();
    bool queue_empty = false;
    {
        std::lock_guard lock(target->apc_lock);
        queue_empty = target->apcs.empty();
    }
    Check(shutdown_started && cancelled && stopped.load() && apc.returned.load() &&
              apc.result == kernel::kAlerted && queue_empty,
          "shutdown drops a pending APC and reports cancellation instead of a callback that did not run");
    Check(kernel::RestartKernelWorkers(), "alertable-wait test restarts worker registration");
    kernel::SetWaitBlockedHookForTest(nullptr);
    kernel::SetWaitApcHookForTest(nullptr);
    g_thread_wait_target.store(nullptr);
    g_thread_wait_test.store(nullptr);

    constexpr uint32_t dpc_address = 0x7A000;
    std::array<uint8_t, 0x1C> dpc_bytes{};
    const uint16_t dpc_type = 19;
    const uint32_t dpc_context = 0x7B000;
    std::memcpy(dpc_bytes.data(), &dpc_type, sizeof(dpc_type));
    std::memcpy(dpc_bytes.data() + 0x0C, &callback_target, sizeof(callback_target));
    std::memcpy(dpc_bytes.data() + 0x10, &dpc_context, sizeof(dpc_context));
    const bool dpc_ready = mem::Commit(dpc_address, mem::kPage) &&
                           mem::Write(dpc_address, dpc_bytes.data(), dpc_bytes.size()) == mem::AccessStatus::kOk;
    Check(dpc_ready, "synthetic KDPC memory initializes for queued-removal test");
    if (dpc_ready) {
        kernel::HoldKernelWorkerActivationForTest(true);
        const bool queued = kernel::QueueDpc(dpc_address, 0x1111, 0x2222);
        const bool removed = kernel::RemoveQueuedDpc(dpc_address);
        kernel::HoldKernelWorkerActivationForTest(false);
        const bool stopped = kernel::StopKernelWorkers();
        uint8_t inserted = 1;
        const bool read_inserted = mem::Read(dpc_address + 2, &inserted, sizeof(inserted)) == mem::AccessStatus::kOk;
        Check(queued && removed && stopped && read_inserted && inserted == 0,
              "queued DPC removal clears guest state before worker activation and shutdown");
        Check(kernel::RestartKernelWorkers(), "DPC cancellation test reopens worker registration");

        constexpr uint32_t shutdown_dpc = dpc_address + 0x100;
        Check(mem::Write(shutdown_dpc, dpc_bytes.data(), dpc_bytes.size()) == mem::AccessStatus::kOk,
              "synthetic KDPC memory initializes for shutdown cancellation test");
        kernel::HoldKernelWorkerActivationForTest(true);
        const bool shutdown_queued = kernel::QueueDpc(shutdown_dpc, 0x3333, 0x4444);
        const bool shutdown_stopped = kernel::StopKernelWorkers();
        kernel::HoldKernelWorkerActivationForTest(false);
        inserted = 1;
        const bool shutdown_read = mem::Read(shutdown_dpc + 2, &inserted, sizeof(inserted)) ==
                                   mem::AccessStatus::kOk;
        Check(shutdown_queued && shutdown_stopped && shutdown_read && inserted == 0,
              "shutdown cancels a queued DPC and clears its guest Inserted state");
        Check(kernel::RestartKernelWorkers(), "DPC shutdown test reopens worker registration");
    }
    dispatch::Reset();
}
#endif

void CheckRuntimeTeardownOrdering() {
    constexpr uint32_t address = 0x6C000;
    Check(mem::Init() && mem::Commit(address, mem::kPage), "guest memory initializes for worker teardown test");
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false;
    std::atomic<bool> released = false;
    std::atomic<int> accesses_after_release = 0;
    Check(kernel::StartKernelWorker([&](kernel::WorkerStopToken stop) {
              kernel::WorkerStopCallback wake(stop, [&] { changed.notify_all(); });
              if (!wake.Ready()) {
                  std::lock_guard guard(lock);
                  entered = true;
                  changed.notify_all();
                  return;
              }
              std::unique_lock guard(lock);
              entered = true;
              changed.notify_all();
              changed.wait(guard, [&] { return stop.StopRequested(); });
              if (released.load()) ++accesses_after_release;
          }),
          "production worker manager accepts synthetic memory callback");
    {
        std::unique_lock guard(lock);
        Check(WaitFor(changed, guard, [&] { return entered; }), "production worker reaches teardown barrier");
    }
    mem::Shutdown();
    released = true;
    Check(!mem::IsInitialized() && accesses_after_release == 0,
          "memory teardown joins workers before releasing guest memory");

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
    Check(mem::Init(), "memory restarts for dispatch teardown ordering test");
    entered = false;
    Check(kernel::StartKernelWorker([&](kernel::WorkerStopToken stop) {
              dispatch::CallLease lease;
              kernel::WorkerStopCallback wake(stop, [&] { changed.notify_all(); });
              if (!wake.Ready()) {
                  std::lock_guard guard(lock);
                  entered = true;
                  changed.notify_all();
                  return;
              }
              std::unique_lock guard(lock);
              entered = true;
              changed.notify_all();
              changed.wait(guard, [&] { return stop.StopRequested(); });
          }),
          "synthetic worker holds production dispatch-table lease");
    {
        std::unique_lock guard(lock);
        Check(WaitFor(changed, guard, [&] { return entered; }), "worker enters dispatch lease barrier");
    }
    Check(kernel::StopKernelWorkers(), "runtime quiescence joins worker holding dispatch lease");
    dispatch::Reset();
    Check(kernel::RestartKernelWorkers(), "runtime worker set reopens after dispatch teardown");
#endif
    Check(mem::Init(), "guest memory lifecycle can start again after teardown");
}

}  // namespace

extern "C" int RunWorkerLifecycleTests() {
    failures = 0;
    CheckStopJoinAndCreationFailure();
    CheckCancellationAndRepeatSetup();
    CheckSuspendedAndQueuedCancellation();
    CheckConcurrentSubmissionAndExit();
    CheckExitedWorkersAreReapedBeforeNewRegistration();
    CheckManagedReapingConcurrentWithJoin();
    CheckStartDoesNotHoldGuestRangeBehindJoin();
#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
    CheckProductionWaitSuspensionAndApcShutdown();
#endif
    CheckRuntimeTeardownOrdering();
    return failures ? 1 : 0;
}
