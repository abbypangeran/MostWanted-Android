#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kernel {

class ClockWorker {
public:
    using Update = bool (*)(void*);

    ~ClockWorker();
    bool Start(Update update, void* context);
    bool Stop();
    bool Running() const;
    bool Failed() const;

private:
    void Run(Update update, void* context);

    std::mutex lifecycle_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    bool stop_requested_ = false;
    std::thread thread_;
    Update update_ = nullptr;
    void* context_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
};

bool StartKernelClockWorker(ClockWorker::Update update, void* context);
bool StopKernelClockWorker();
bool KernelClockWorkerRunning();

class KernelMemoryLifecycle {
public:
    KernelMemoryLifecycle();
    void SetReady(bool ready);
    bool StopWorker();

private:
    std::unique_lock<std::mutex> lock_;
};

}  // namespace kernel
