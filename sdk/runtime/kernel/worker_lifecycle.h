#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

namespace kernel {

class WorkerStopCallback;

class WorkerStopToken {
public:
    struct State;
    WorkerStopToken() = default;
    bool StopRequested() const;

private:
    explicit WorkerStopToken(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
    friend class WorkerLifecycle;
    friend class WorkerStopCallback;
};

class WorkerStopCallback {
public:
    WorkerStopCallback(const WorkerStopToken& token, std::function<void()> callback);
    ~WorkerStopCallback();
    WorkerStopCallback(const WorkerStopCallback&) = delete;
    WorkerStopCallback& operator=(const WorkerStopCallback&) = delete;
    bool Ready() const { return ready_; }

private:
    std::shared_ptr<WorkerStopToken::State> state_;
    std::function<void()> callback_;
    std::mutex lock_;
    std::condition_variable changed_;
    std::size_t running_ = 0;
    bool active_ = true;
    bool registered_ = false;
    bool ready_ = false;
    friend struct WorkerStopToken::State;
};

class WorkerLifecycle {
public:
    using Task = std::function<void(WorkerStopToken)>;
    enum class State : uint8_t { kPrepared, kRunning, kStopRequested, kQuiescing, kJoining, kJoined, kResourcesReleased };

    explicit WorkerLifecycle(bool accepting = true);
    ~WorkerLifecycle();
    WorkerLifecycle(const WorkerLifecycle&) = delete;
    WorkerLifecycle& operator=(const WorkerLifecycle&) = delete;

    bool Start(Task task, std::size_t stack_size = 0);
    void BeginShutdown();
    bool Join();
    bool StopAndJoin();
    bool StartAccepting();
    bool Accepting() const;
    bool CurrentThreadOwnsWorker() const;
    std::size_t OwnedCount() const;

#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
    void FailNextStartForTest();
    void HoldActivationForTest(bool hold);
    void HoldAfterReapForTest(bool hold);
    std::size_t QuiescingCountForTest() const;
    std::size_t JoiningCountForTest() const;
    std::size_t JoinWaiterCountForTest() const;
    bool WaitForJoinInProgressForTest(std::chrono::milliseconds timeout);
    bool WaitForShutdownStartedForTest(std::chrono::milliseconds timeout);
    bool TasksFinishedForTest() const;
#endif

private:
    struct Worker;
    mutable std::mutex lock_;
    std::condition_variable changed_;
    std::mutex join_lock_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> accepting_;
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
    std::mutex activation_lock_;
    std::condition_variable activation_changed_;
    bool hold_activation_ = false;
    bool fail_next_start_ = false;
    std::size_t join_waiters_for_test_ = 0;
    bool joining_workers_for_test_ = false;
    std::mutex reap_test_lock_;
    std::condition_variable reap_test_changed_;
    bool hold_after_reap_ = false;
#endif
};

bool StartKernelWorker(WorkerLifecycle::Task task, std::size_t stack_size = 0);
bool StopKernelWorkers();
bool RestartKernelWorkers();
bool KernelWorkersAccepting();
#if defined(MOSTWANTED_EXECUTION_PROBE_TESTS) || defined(MOSTWANTED_KERNEL_WORKER_TESTS)
std::size_t KernelWorkerCountForTest();
#endif
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
void HoldKernelWorkerActivationForTest(bool hold);
void FailNextKernelWorkerStartForTest();
bool WaitForKernelWorkersShutdownForTest(std::chrono::milliseconds timeout);
// Every worker has returned from its task (its QuiesceOnExit has run) even if
// its host thread has not been reaped yet. Test evidence for deterministic
// finalisation without forcing a reap/restart.
bool KernelWorkerTasksFinishedForTest();
#endif

class GuestDispatchLease {
public:
    GuestDispatchLease();
    ~GuestDispatchLease();
    GuestDispatchLease(const GuestDispatchLease&) = delete;
    GuestDispatchLease& operator=(const GuestDispatchLease&) = delete;
    bool Allowed() const { return allowed_; }

private:
    std::shared_lock<std::shared_mutex> lock_;
    bool allowed_ = false;
    bool outer_ = false;
};

}  // namespace kernel
