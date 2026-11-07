#include "kernel/worker_lifecycle.h"

#include <algorithm>
#include <new>
#include <utility>

#include "core/log.h"

namespace kernel {

struct WorkerStopToken::State {
    std::mutex lock;
    std::vector<WorkerStopCallback*> callbacks;
    std::atomic<bool> requested{false};

    void RequestStop() {
        if (requested.exchange(true)) return;
        std::vector<WorkerStopCallback*> pending;
        {
            std::lock_guard guard(lock);
            pending.swap(callbacks);
            for (WorkerStopCallback* callback : pending) {
                std::lock_guard callback_lock(callback->lock_);
                if (callback->active_) ++callback->running_;
            }
        }
        for (WorkerStopCallback* callback : pending) {
            {
                std::lock_guard callback_lock(callback->lock_);
                if (!callback->running_) continue;
            }
            try {
                callback->callback_();
            } catch (...) {
                LOGE("kernel: worker stop callback failed");
            }
            {
                std::lock_guard callback_lock(callback->lock_);
                --callback->running_;
            }
            callback->changed_.notify_all();
        }
    }
};

bool WorkerStopToken::StopRequested() const { return state_ && state_->requested.load(); }

WorkerStopCallback::WorkerStopCallback(const WorkerStopToken& token, std::function<void()> callback)
    : state_(token.state_), callback_(std::move(callback)) {
    if (!state_ || !callback_) return;
    std::unique_lock guard(state_->lock);
    if (state_->requested.load()) {
        guard.unlock();
        try {
            callback_();
        } catch (...) {
            LOGE("kernel: worker stop callback failed");
        }
        ready_ = true;
        return;
    }
    try {
        state_->callbacks.push_back(this);
        registered_ = true;
        ready_ = true;
    } catch (const std::bad_alloc&) {
        LOGE("kernel: could not register worker stop callback");
    }
}

WorkerStopCallback::~WorkerStopCallback() {
    if (!registered_) return;
    {
        std::lock_guard guard(state_->lock);
        std::lock_guard callback_guard(lock_);
        active_ = false;
        auto found = std::find(state_->callbacks.begin(), state_->callbacks.end(), this);
        if (found != state_->callbacks.end()) state_->callbacks.erase(found);
        registered_ = false;
    }
    std::unique_lock guard(lock_);
    changed_.wait(guard, [this] { return running_ == 0; });
}

struct WorkerLifecycle::Worker {
    std::thread thread;
    std::shared_ptr<WorkerStopToken::State> stop = std::make_shared<WorkerStopToken::State>();
    std::atomic<State> state{State::kPrepared};
};

WorkerLifecycle::WorkerLifecycle(bool accepting) : accepting_(accepting) {}

WorkerLifecycle::~WorkerLifecycle() {
    if (!StopAndJoin()) std::terminate();
}

bool WorkerLifecycle::Start(Task task) {
    if (!task) return false;
    const bool owned_thread = CurrentThreadOwnsWorker();
    std::unique_lock<std::mutex> joining(join_lock_, std::defer_lock);
    std::vector<Worker*> finished;
    if (!owned_thread) {
        joining.lock();
    }
    {
        std::unique_lock lock(lock_);
        if (!accepting_.load()) return false;
        if (!owned_thread)
            changed_.wait(lock, [this] {
                return std::none_of(workers_.begin(), workers_.end(), [](const auto& worker) {
                    return worker->state.load() == State::kJoining;
                });
            });
        try {
            finished.reserve(workers_.size());
        } catch (const std::bad_alloc&) {
            return false;
        }
        for (auto it = workers_.begin(); it != workers_.end();) {
            if ((*it)->state.load() == State::kQuiescing) {
                (*it)->state.store(State::kJoining);
                finished.push_back(it->get());
                ++it;
            } else {
                ++it;
            }
        }
    }
    for (Worker* worker : finished) {
        if (worker->thread.joinable()) worker->thread.join();
    }
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
    if (!finished.empty()) {
        std::unique_lock test_lock(reap_test_lock_);
        reap_test_changed_.wait(test_lock, [this] { return !hold_after_reap_; });
    }
#endif
    if (!finished.empty()) {
        std::lock_guard lock(lock_);
        for (Worker* worker : finished) {
            worker->state.store(State::kJoined);
            worker->state.store(State::kResourcesReleased);
            auto found = std::find_if(workers_.begin(), workers_.end(), [worker](const auto& owned) {
                return owned.get() == worker;
            });
            if (found != workers_.end()) workers_.erase(found);
        }
        changed_.notify_all();
    }
    std::lock_guard lock(lock_);
    if (!accepting_.load()) return false;
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
    if (fail_next_start_) {
        fail_next_start_ = false;
        return false;
    }
#endif
    std::unique_ptr<Worker> worker;
    try {
        worker = std::make_unique<Worker>();
        Worker* raw = worker.get();
        workers_.push_back(std::move(worker));
        try {
            raw->thread = std::thread([this, raw, task = std::move(task)]() mutable {
                struct QuiesceOnExit {
                    Worker& worker;
                    ~QuiesceOnExit() { worker.state.store(State::kQuiescing); }
                } quiesce{*raw};

#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
                {
                    std::unique_lock activation(activation_lock_);
                    activation_changed_.wait(activation, [this, raw] {
                        return !hold_activation_ || raw->stop->requested.load();
                    });
                }
#endif
                State expected = State::kPrepared;
                raw->state.compare_exchange_strong(expected, State::kRunning);
                task(WorkerStopToken(raw->stop));
            });
        } catch (...) {
            workers_.pop_back();
            return false;
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void WorkerLifecycle::BeginShutdown() {
    std::lock_guard lock(lock_);
    if (!accepting_.exchange(false)) return;
    for (const auto& worker : workers_) {
        State state = worker->state.load();
        while ((state == State::kPrepared || state == State::kRunning) &&
               !worker->state.compare_exchange_weak(state, State::kStopRequested)) {}
        worker->stop->RequestStop();
    }
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
    activation_changed_.notify_all();
#endif
}

bool WorkerLifecycle::Join() {
    if (CurrentThreadOwnsWorker()) {
        BeginShutdown();
        return false;
    }
    std::lock_guard joining(join_lock_);
    BeginShutdown();
    std::vector<Worker*> workers;
    {
        std::unique_lock lock(lock_);
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
        ++join_waiters_for_test_;
#endif
        changed_.wait(lock, [this] {
            return std::none_of(workers_.begin(), workers_.end(), [](const auto& worker) {
                return worker->state.load() == State::kJoining;
            });
        });
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
        --join_waiters_for_test_;
#endif
        try {
            workers.reserve(workers_.size());
            for (const auto& worker : workers_) workers.push_back(worker.get());
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    for (Worker* worker : workers) {
        if (worker->thread.joinable()) worker->thread.join();
        worker->state.store(State::kJoined);
        worker->state.store(State::kResourcesReleased);
    }
    {
        std::lock_guard lock(lock_);
        workers_.clear();
    }
    return true;
}

bool WorkerLifecycle::StopAndJoin() {
    BeginShutdown();
    return Join();
}

bool WorkerLifecycle::StartAccepting() {
    std::lock_guard lock(lock_);
    if (accepting_.load()) return true;
    if (!workers_.empty()) return false;
    accepting_.store(true);
    return true;
}

bool WorkerLifecycle::Accepting() const { return accepting_.load(); }

bool WorkerLifecycle::CurrentThreadOwnsWorker() const {
    std::lock_guard lock(lock_);
    for (const auto& worker : workers_)
        if (worker->thread.joinable() && worker->thread.get_id() == std::this_thread::get_id()) return true;
    return false;
}

std::size_t WorkerLifecycle::OwnedCount() const {
    std::lock_guard lock(lock_);
    return workers_.size();
}

#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
void WorkerLifecycle::FailNextStartForTest() {
    std::lock_guard lock(lock_);
    fail_next_start_ = true;
}

void WorkerLifecycle::HoldActivationForTest(bool hold) {
    {
        std::lock_guard lock(activation_lock_);
        hold_activation_ = hold;
    }
    if (!hold) activation_changed_.notify_all();
}

void WorkerLifecycle::HoldAfterReapForTest(bool hold) {
    {
        std::lock_guard lock(reap_test_lock_);
        hold_after_reap_ = hold;
    }
    if (!hold) reap_test_changed_.notify_all();
}

std::size_t WorkerLifecycle::QuiescingCountForTest() const {
    std::lock_guard lock(lock_);
    return std::count_if(workers_.begin(), workers_.end(), [](const auto& worker) {
        return worker->state.load() == State::kQuiescing;
    });
}

std::size_t WorkerLifecycle::JoiningCountForTest() const {
    std::lock_guard lock(lock_);
    return std::count_if(workers_.begin(), workers_.end(), [](const auto& worker) {
        return worker->state.load() == State::kJoining;
    });
}

std::size_t WorkerLifecycle::JoinWaiterCountForTest() const {
    std::lock_guard lock(lock_);
    return join_waiters_for_test_;
}
#endif

namespace {
WorkerLifecycle g_kernel_workers(false);
std::shared_mutex g_guest_dispatch_gate;
thread_local uint32_t g_guest_dispatch_depth = 0;
}

bool StartKernelWorker(WorkerLifecycle::Task task) { return g_kernel_workers.Start(std::move(task)); }

bool StopKernelWorkers() {
    g_kernel_workers.BeginShutdown();
    if (g_kernel_workers.CurrentThreadOwnsWorker()) return false;
    {
        std::unique_lock gate(g_guest_dispatch_gate);
    }
    return g_kernel_workers.Join();
}

bool RestartKernelWorkers() { return g_kernel_workers.StartAccepting(); }
bool KernelWorkersAccepting() { return g_kernel_workers.Accepting(); }

GuestDispatchLease::GuestDispatchLease() {
    if (g_guest_dispatch_depth) {
        if (!KernelWorkersAccepting()) return;
        ++g_guest_dispatch_depth;
        allowed_ = true;
        return;
    }
    lock_ = std::shared_lock<std::shared_mutex>(g_guest_dispatch_gate);
    if (!KernelWorkersAccepting()) return;
    ++g_guest_dispatch_depth;
    outer_ = true;
    allowed_ = true;
}

GuestDispatchLease::~GuestDispatchLease() {
    if (!allowed_) return;
    --g_guest_dispatch_depth;
    if (outer_) lock_.unlock();
}

}  // namespace kernel
