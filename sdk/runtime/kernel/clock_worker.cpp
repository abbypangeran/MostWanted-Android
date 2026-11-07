#include "kernel/clock_worker.h"

#include <chrono>

#include "core/log.h"
#include "kernel/worker_lifecycle.h"

namespace kernel {

ClockWorker::~ClockWorker() { Stop(); }

bool ClockWorker::Start(Update update, void* context) {
    if (!update) return false;
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (thread_.joinable()) {
        if (running_.load()) return update_ == update && context_ == context;
        thread_.join();
    }
    {
        std::lock_guard wait(wait_mutex_);
        stop_requested_ = false;
    }
    update_ = update;
    context_ = context;
    failed_.store(false);
    running_.store(true);
    try {
        thread_ = std::thread(&ClockWorker::Run, this, update, context);
    } catch (...) {
        running_.store(false);
        failed_.store(true);
        update_ = nullptr;
        context_ = nullptr;
        return false;
    }
    return true;
}

bool ClockWorker::Stop() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (!thread_.joinable()) return !failed_.load();
    {
        std::lock_guard wait(wait_mutex_);
        stop_requested_ = true;
    }
    wait_cv_.notify_all();
    thread_.join();
    update_ = nullptr;
    context_ = nullptr;
    return !failed_.load();
}

bool ClockWorker::Running() const { return running_.load(); }
bool ClockWorker::Failed() const { return failed_.load(); }

void ClockWorker::Run(Update update, void* context) {
    bool failed = false;
    for (;;) {
        bool updated = false;
        try {
            updated = update(context);
        } catch (...) {
            updated = false;
        }
        if (!updated) {
            failed = true;
            break;
        }
        std::unique_lock wait(wait_mutex_);
        if (wait_cv_.wait_for(wait, std::chrono::milliseconds(1), [this] { return stop_requested_; })) break;
    }
    if (failed) failed_.store(true);
    running_.store(false);
}

namespace {
ClockWorker g_worker;
std::mutex g_memory_lifecycle_mutex;
bool g_memory_ready = false;
}

bool StartKernelClockWorker(ClockWorker::Update update, void* context) {
    std::lock_guard lifecycle(g_memory_lifecycle_mutex);
    return g_memory_ready && g_worker.Start(update, context);
}
bool StopKernelClockWorker() {
    std::lock_guard lifecycle(g_memory_lifecycle_mutex);
    const bool stopped = g_worker.Stop();
    if (!stopped) LOGE("kernel: clock worker stopped after an update failure");
    return stopped;
}
bool KernelClockWorkerRunning() { return g_worker.Running(); }

KernelMemoryLifecycle::KernelMemoryLifecycle() : lock_(g_memory_lifecycle_mutex) {}

bool KernelMemoryLifecycle::SetReady(bool ready) {
    g_memory_ready = ready && RestartKernelWorkers();
    return !ready || g_memory_ready;
}

bool KernelMemoryLifecycle::StopWorker() {
    g_memory_ready = false;
    if (!g_worker.Stop()) LOGE("kernel: clock worker stopped after an update failure");
    return StopKernelWorkers();
}

}  // namespace kernel
