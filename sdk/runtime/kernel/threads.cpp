#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#include "kernel/kernel.h"
#include "kernel/thread_memory.h"
#include "kernel/worker_lifecycle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace kernel {

namespace {

thread_local ThreadRef t_current;
struct GuestThreadTermination {};
std::mutex g_threads_lock;
std::vector<ThreadRef> g_threads;
std::atomic<uint32_t> g_next_id{0x10};
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
std::atomic<ThreadPreparationFailPoint> g_thread_fail_point{ThreadPreparationFailPoint::kNone};
std::atomic<uint32_t> g_worker_launch_count{0};

bool FailAt(ThreadPreparationFailPoint point) {
    auto expected = point;
    return g_thread_fail_point.compare_exchange_strong(expected, ThreadPreparationFailPoint::kNone);
}
#endif

// guest structures per thread
constexpr uint32_t kKpcrSize = 0x400;
constexpr uint32_t kEthreadSize = 0x200;

bool FinalizeThreadResources(const ThreadRef& thread, bool memory_shutdown);

bool TrackThreadOwner(const ThreadRef& thread) {
    if (!thread || thread->memory_owner) return bool(thread);
    thread->memory_owner = mem::RegisterGuestThreadOwner([thread] {
        return FinalizeThreadResources(thread, true);
    });
    return thread->memory_owner != 0;
}

bool FreeGuestBlocks(Thread& t) {
    mem::ScopedVirtualRangeLock memory_lock;
    if (t.memory_generation != mem::Generation()) {
        t.ethread = t.kpcr = t.stack_top = t.stack_size = t.tls = 0;
        return true;
    }
    bool released = true;
    if (t.ethread && mem::FreeSystem(t.ethread)) t.ethread = 0;
    else if (t.ethread) released = false;
    if (t.kpcr && mem::FreeSystem(t.kpcr)) t.kpcr = 0;
    else if (t.kpcr) released = false;
    if (t.stack_top && mem::FreeStack(t.stack_top)) t.stack_top = 0;
    else if (t.stack_top) released = false;
    if (!t.stack_top) t.stack_size = t.tls = 0;
    return released && !t.ethread && !t.kpcr && !t.stack_top;
}

bool SetupGuestBlocks(const ThreadRef& thread, uint32_t stack_size, uint32_t tls_size) {
    if (!thread) return false;
    mem::ScopedVirtualRangeLock memory_lock;
    Thread& t = *thread;
    t.memory_generation = mem::Generation();
    if (!t.memory_generation || !mem::IsInitialized() || !TrackThreadOwner(thread)) return false;
    if (tls_size > UINT32_MAX - 3) return false;
    stack_size = std::max<uint32_t>(stack_size, 0x10000);
    tls_size = (tls_size + 3) & ~3u;
    constexpr uint32_t kMaximumStackRequest = mem::kStackSize - 0x10000;
    if (stack_size > kMaximumStackRequest - 0x1000 ||
        tls_size > kMaximumStackRequest - 0x1000 - stack_size)
        return false;
    t.stack_top = mem::AllocStack(stack_size + tls_size + 0x1000);
    t.stack_size = stack_size;
    if (!t.stack_top) {
        LOGE("thread: guest stack allocation failed");
        return false;
    }
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
    if (FailAt(ThreadPreparationFailPoint::kAfterStack)) {
        FreeGuestBlocks(t);
        return false;
    }
#endif
    t.kpcr = mem::AllocSystem(kKpcrSize, 64);
    if (!t.kpcr) {
        LOGE("thread: KPCR allocation failed");
        FreeGuestBlocks(t);
        return false;
    }
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
    if (FailAt(ThreadPreparationFailPoint::kAfterKpcr)) {
        FreeGuestBlocks(t);
        return false;
    }
#endif
    t.ethread = mem::AllocSystem(kEthreadSize, 64);
    if (!t.ethread) {
        LOGE("thread: ETHREAD allocation failed");
        FreeGuestBlocks(t);
        return false;
    }
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
    if (FailAt(ThreadPreparationFailPoint::kAfterEthread)) {
        FreeGuestBlocks(t);
        return false;
    }
#endif
    // TLS sits at the top of the stack: StackBase + tls_index * 4 must equal TlsData
    t.tls = t.stack_top - tls_size;
#ifdef __APPLE__
    const auto status = InitializeThreadGuestMemory(t.kpcr, t.ethread, t.stack_top, stack_size, t.tls, t.id);
    if (status != mem::AccessStatus::kOk) {
        LOGE("thread: guest KPCR/ETHREAD initialization failed: %s", mem::StatusName(status));
        FreeGuestBlocks(t);
        return false;
    }
#else
    uint32_t k = t.kpcr;
    W32(k + 0x00, 0xFFFFFFFF);
    W32(k + 0x04, t.stack_top);
    W32(k + 0x08, t.stack_top - stack_size);
    W32(k + 0x18, k);
    W32(k + 0x1C, k);
    W32(k + 0x20, k + 0x28);
    G<uint8_t>(k + 0x24) = 0;
    W32(k + 0x28, t.ethread);
    uint32_t e = t.ethread;
    G<uint8_t>(e + 0) = 6;
    G<uint8_t>(e + 2) = uint8_t(kEthreadSize / 4);
    W32(e + 0x1C, t.stack_top);
    W32(e + 0x20, t.stack_top - stack_size);
    W32(e + 0x28, t.tls);
    W32(e + 0x12C, t.id);
#endif
    return true;
}

bool ValidThreadSizes(uint32_t stack_size, uint32_t tls_size) {
    if (tls_size > UINT32_MAX - 3) return false;
    stack_size = std::max<uint32_t>(stack_size, 0x10000);
    tls_size = (tls_size + 3) & ~3u;
    constexpr uint32_t kMaximumStackRequest = mem::kStackSize - 0x10000;
    return stack_size <= kMaximumStackRequest - 0x1000 &&
           tls_size <= kMaximumStackRequest - 0x1000 - stack_size;
}

void InitContext(Thread& t, X86Context& ctx) {
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (t.tls & ~15u) - 16;
    ctx.fs_base = t.kpcr;
    ctx.fcw = 0x27F;  // x87 53 bit precision, exceptions masked
    ctx.mxcsr = 0x1F80;  // SSE exceptions masked, round to nearest
    ctx.thread_handle = 0;
    t.ctx = &ctx;
}

bool Register(const ThreadRef& t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    try {
        g_threads.push_back(t);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void Unregister(Thread* t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.erase(std::remove_if(g_threads.begin(), g_threads.end(), [t](const ThreadRef& r) { return r.get() == t; }),
                    g_threads.end());
}

void MarkExited(Thread& t, uint32_t code, ThreadState state = ThreadState::kExited,
                bool write_guest_signal = true) {
    t.exit_code = code;
    {
        std::lock_guard<std::mutex> apcs(t.apc_lock);
        t.state = state;
        t.apcs.clear();
    }
    std::lock_guard<std::mutex> lock(DispatchLock());
    if (write_guest_signal)
        SetSignal(t, 1);
    else
        t.signal = 1;
    SignalChanged();
}

void NameHostThread(const char* name) {
#ifdef _WIN32
    wchar_t w[64];
    size_t i = 0;
    for (; name[i] && i < 63; i++) w[i] = wchar_t(name[i]);
    w[i] = 0;
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static Fn set = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), w);
#else
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
#endif
}

}  // namespace

Thread* Current() { return t_current.get(); }
ThreadRef CurrentRef() { return t_current; }
X86Context& CurrentContext() { return *t_current->ctx; }

std::vector<ThreadRef> Threads() {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    return g_threads;
}

ThreadRef CreateThread(uint32_t stack_size, uint32_t tls_size, uint32_t system_routine, uint32_t start_routine,
                       uint32_t start_context, bool suspended) {
    ThreadRef t;
    try {
        t = std::make_shared<Thread>();
    } catch (const std::bad_alloc&) {
        return {};
    }
    mem::ScopedVirtualRangeLock memory_lock;
    t->id = g_next_id.fetch_add(4);
    if (!SetupGuestBlocks(t, stack_size, tls_size)) {
        FinalizeThreadResources(t, false);
        return {};
    }
    t->system_routine = system_routine;
    t->start_routine = start_routine;
    t->start_context = start_context;
    if (!TryBind(t, t->ethread)) {
        FinalizeThreadResources(t, false);
        return {};
    }
    if (suspended) t->suspend = 1;
    return t;
}

namespace {

uint32_t PublishPreparedThread(const ThreadRef& t, uint32_t handle_out, uint32_t id_out) {
    if (!t) return kInvalidParameter;
    mem::ScopedVirtualRangeLock memory_range_lock;
    std::lock_guard lifecycle_lock(t->lifecycle_lock);
    if (t->state.load() != ThreadState::kPrepared || t->memory_generation != mem::Generation())
        return kInvalidParameter;
    if (handle_out && id_out && uint64_t(handle_out) < uint64_t(id_out) + 4 &&
        uint64_t(id_out) < uint64_t(handle_out) + 4)
        return kInvalidParameter;
    mem::ScopedLock memory_lock;
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
    if (FailAt(ThreadPreparationFailPoint::kBeforePublish)) return kNoMemory;
#endif
    if ((handle_out && mem::CheckAccess(handle_out, sizeof(uint32_t), mem::kRead | mem::kWrite) != mem::AccessStatus::kOk) ||
        (id_out && mem::CheckAccess(id_out, sizeof(uint32_t), mem::kRead | mem::kWrite) != mem::AccessStatus::kOk))
        return kAccessViolation;

    uint32_t handle = 0;
    if (handle_out && !TryNewHandle(t, handle)) return kNoMemory;
    t->handle_out = handle_out;
    t->id_out = id_out;
    t->published_handle = handle;
    if (id_out) {
        if (mem::Write(id_out, &t->id, sizeof(t->id)) != mem::AccessStatus::kOk) return kAccessViolation;
        t->id_published = true;
    }
    if (handle_out) {
        if (mem::Write(handle_out, &handle, sizeof(handle)) != mem::AccessStatus::kOk) return kAccessViolation;
        t->handle_published = true;
    }
    return kSuccess;
}

void ClearPublishedOutputs(Thread& t) {
    if (t.memory_generation != mem::Generation()) return;
    mem::ScopedLock memory_lock;
    uint32_t value = 0;
    if (t.handle_out && t.handle_published && t.published_handle &&
        mem::Read(t.handle_out, &value, sizeof(value)) == mem::AccessStatus::kOk && value == t.published_handle) {
        value = 0;
        mem::Write(t.handle_out, &value, sizeof(value));
    }
    value = 0;
    if (t.id_out && t.id_published && mem::Read(t.id_out, &value, sizeof(value)) == mem::AccessStatus::kOk && value == t.id) {
        value = 0;
        mem::Write(t.id_out, &value, sizeof(value));
    }
}

bool FinalizeThreadResources(const ThreadRef& thread, bool memory_shutdown) {
    if (!thread) return false;
    mem::ScopedVirtualRangeLock memory_range_lock;
    std::lock_guard lifecycle_lock(thread->lifecycle_lock);
    if (thread->resources_released) return true;

    ThreadState state = thread->state.load();
    if (memory_shutdown && (state == ThreadState::kPrepared || state == ThreadState::kActive)) {
        MarkExited(*thread, 0, ThreadState::kCancelled, false);
        state = ThreadState::kCancelled;
    }
    if (state == ThreadState::kActive) return false;

    const bool was_prepared = !thread->activation_started;
    thread->ctx = nullptr;
    {
        std::lock_guard apcs(thread->apc_lock);
        thread->apcs.clear();
    }
    Unregister(thread.get());
    Unbind(std::static_pointer_cast<Object>(thread));
    thread->system_routine_ticket = thread->start_routine_ticket = {};
    thread->system_routine = thread->start_routine = thread->start_context = 0;
    if (was_prepared) {
        ClearPublishedOutputs(*thread);
        if (thread->published_handle) CloseHandle(thread->published_handle);
        thread->published_handle = 0;
    }
    thread->handle_out = thread->id_out = 0;
    thread->handle_published = thread->id_published = false;
    if (!FreeGuestBlocks(*thread)) return false;

    thread->resources_released = true;
    mem::UnregisterGuestThreadOwner(thread->memory_owner);
    thread->memory_owner = 0;
    return true;
}

}  // namespace

uint32_t PrepareSystemThreadEx(uint32_t handle_out, uint32_t stack_size, uint32_t tls_size, uint32_t id_out,
                               uint32_t start_routine, uint32_t start_context, bool suspended,
                               uint32_t system_routine, ThreadRef& prepared) {
    if (prepared) return kInvalidParameter;
    if (!ValidThreadSizes(stack_size, tls_size)) return kInvalidParameter;
    ThreadRef t = CreateThread(stack_size, tls_size, system_routine, start_routine, start_context, suspended);
    if (!t) return kNoMemory;
    const uint32_t status = PublishPreparedThread(t, handle_out, id_out);
    if (status != kSuccess) {
        CancelPreparedThread(t);
        return status;
    }
    prepared = std::move(t);
    return kSuccess;
}

bool CancelPreparedThread(const ThreadRef& t) {
    if (!t) return false;
    mem::ScopedVirtualRangeLock memory_range_lock;
    {
        std::lock_guard lifecycle_lock(t->lifecycle_lock);
        if (t->resources_released || t->activation_started) return false;
        std::lock_guard<std::mutex> lock(t->apc_lock);
        const ThreadState state = t->state.load();
        if (state == ThreadState::kPrepared) {
            t->state = ThreadState::kCancelled;
            t->exit_code = 0;
        } else if (state != ThreadState::kCancelled) {
            return false;
        }
        t->apcs.clear();
    }
    {
        std::lock_guard lock(DispatchLock());
        t->signal = 1;
        SignalChanged();
    }
    return FinalizeThreadResources(t, false);
}

bool StartThread(ThreadRef t) {
    if (!t) return false;
    mem::ScopedVirtualRangeLock memory_range_lock;
    std::unique_lock lifecycle_lock(t->lifecycle_lock);
    if (t->resources_released || t->memory_generation != mem::Generation() ||
        t->state.load() != ThreadState::kPrepared)
        return false;
    GuestCallbackTicket start_ticket, system_ticket;
    if (PrepareGuestCallback(t->start_routine, GuestCallbackClass::kThreadStart, start_ticket) !=
            GuestCallbackStatus::kReady ||
        (t->system_routine && PrepareGuestCallback(t->system_routine, GuestCallbackClass::kSystemRoutine,
                                                   system_ticket) != GuestCallbackStatus::kReady))
        return false;
    t->start_routine_ticket = start_ticket;
    t->system_routine_ticket = system_ticket;
    t->state = ThreadState::kActive;
    t->activation_started = true;
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
    if (FailAt(ThreadPreparationFailPoint::kBeforeThreadRegistration)) {
        t->state = ThreadState::kPrepared;
        t->activation_started = false;
        t->start_routine_ticket = t->system_routine_ticket = {};
        return false;
    }
#endif
    if (!Register(t)) {
        t->state = ThreadState::kPrepared;
        t->activation_started = false;
        t->start_routine_ticket = t->system_routine_ticket = {};
        return false;
    }
    bool worker_started = false;
    try {
        worker_started = StartKernelWorker([t](WorkerStopToken stop) {
            t_current = t;
            try {
                char name[32];
                std::snprintf(name, sizeof(name), "guest %X", t->id);
                NameHostThread(name);
                X86Context ctx;
                InitContext(*t, ctx);
                {
                    WorkerStopCallback wake(stop, [t] {
                        t->stop_requested = true;
                        {
                            std::lock_guard<std::mutex> lock(t->apc_lock);
                            t->apcs.clear();
                        }
                        t->resume_cv.notify_all();
                        SignalChanged();
                    });
                    if (!wake.Ready() || stop.StopRequested()) {
                        MarkExited(*t, 0, ThreadState::kCancelled);
                    } else {
                        std::unique_lock<std::mutex> lock(t->apc_lock);
                        t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0 || stop.StopRequested(); });
                        lock.unlock();
                        if (stop.StopRequested()) {
                            MarkExited(*t, 0, ThreadState::kCancelled);
                        } else {
                            LOGI("thread %X starts at %08X (%08X, %08X)", t->id,
                                 t->system_routine ? t->system_routine : t->start_routine, t->start_routine,
                                 t->start_context);
                            const std::array<uint32_t, 2> system_arguments{t->start_routine, t->start_context};
                            const std::array<uint32_t, 1> start_arguments{t->start_context};
                            const uint32_t stack_base = t->stack_top - t->stack_size;
                            uint32_t code = 0;
                            const GuestCallbackStatus callback_status =
                                t->system_routine
                                    ? CallGuestCallback(ctx, t->system_routine_ticket, t->system_routine,
                                                        system_arguments, stack_base, t->stack_size, code)
                                    : CallGuestCallback(ctx, t->start_routine_ticket, t->start_routine,
                                                        start_arguments, stack_base, t->stack_size, code);
                            if (callback_status == GuestCallbackStatus::kInvoked) {
                                LOGI("thread %X returned %08X", t->id, code);
                                MarkExited(*t, code);
                            } else {
                                LOGI("thread %X callback rejected: %u", t->id, unsigned(callback_status));
                                MarkExited(*t, 0, ThreadState::kCancelled);
                            }
                        }
                    }
                }
            } catch (const GuestThreadTermination&) {
            } catch (...) {
                MarkExited(*t, 0, ThreadState::kCancelled);
            }
            t->ctx = nullptr;
            Unregister(t.get());
            t_current.reset();
            FinalizeThreadResources(t, false);
        }, 64u << 20);
    } catch (const std::bad_alloc&) {
    }
    if (!worker_started) {
        Unregister(t.get());
        t->state = ThreadState::kPrepared;
        t->activation_started = false;
        t->start_routine_ticket = t->system_routine_ticket = {};
        return false;
    }
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
    ++g_worker_launch_count;
#endif
    return true;
}

#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
void SetThreadPreparationFailureForTest(ThreadPreparationFailPoint point) { g_thread_fail_point = point; }
uint32_t ThreadWorkerLaunchCountForTest() { return g_worker_launch_count.load(); }
bool InitializePreparedThreadContextForTest(const ThreadRef& thread, X86Context& context) {
    if (!thread || thread->state.load() != ThreadState::kPrepared) return false;
    InitContext(*thread, context);
    return true;
}
#endif

bool StartManagedHostThread(const char* name, std::function<void(X86Context&, WorkerStopToken)> fn,
                            std::size_t stack_size) {
    ThreadRef t;
    try {
        t = std::make_shared<Thread>();
    } catch (const std::bad_alloc&) {
        return false;
    }
    t->id = g_next_id.fetch_add(4);
    mem::ScopedVirtualRangeLock memory_lock;
    if (!SetupGuestBlocks(t, 0x20000, 0x100)) {
        FinalizeThreadResources(t, false);
        return false;
    }
    if (!TryBind(t, t->ethread)) {
        FinalizeThreadResources(t, false);
        return false;
    }
    std::string n;
    try {
        n = name;
    } catch (const std::bad_alloc&) {
        FinalizeThreadResources(t, false);
        return false;
    }
    t->state = ThreadState::kActive;
    t->activation_started = true;
    bool worker_started = false;
    try {
        worker_started = StartKernelWorker([t, fn = std::move(fn), n = std::move(n)](WorkerStopToken stop) {
            t_current = t;
            try {
                NameHostThread(n.c_str());
                X86Context ctx;
                InitContext(*t, ctx);
                fn(ctx, stop);
            } catch (const GuestThreadTermination&) {
            } catch (...) {
                MarkExited(*t, 0, ThreadState::kCancelled);
            }
            if (t->state.load() == ThreadState::kActive) MarkExited(*t, t->exit_code.load());
            t->ctx = nullptr;
            t_current.reset();
            Unregister(t.get());
            FinalizeThreadResources(t, false);
        }, stack_size);
    } catch (const std::bad_alloc&) {
    }
    if (!worker_started) {
        t->state = ThreadState::kPrepared;
        t->activation_started = false;
        FinalizeThreadResources(t, false);
        return false;
    }
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
    ++g_worker_launch_count;
#endif
    return true;
}

void ExitThread(uint32_t code) {
    Thread* t = Current();
    if (!t) {
#ifdef _WIN32
        ::ExitThread(code);
#else
        pthread_exit(nullptr);
#endif
    }
    if (t->state.load() != ThreadState::kActive) throw GuestThreadTermination{};
    LOGI("thread %X exits with %08X", t->id, code);
    MarkExited(*t, code);
    throw GuestThreadTermination{};
}

void Resume(Thread& t) {
    {
        std::lock_guard<std::mutex> lock(t.apc_lock);
        if (t.suspend > 0) t.suspend--;
    }
    t.resume_cv.notify_all();
    SignalChanged();
}

uint32_t Suspend(Thread& t) {
    uint32_t prev;
    {
        std::lock_guard<std::mutex> lock(t.apc_lock);
        prev = uint32_t(t.suspend++);
    }
    t.resume_cv.notify_all();
    SignalChanged();
    if (&t == Current()) SuspendPoint();
    return prev;
}

void SuspendPoint() {
    Thread* t = Current();
    if (!t || t->suspend.load() <= 0 || t->stop_requested.load()) return;
    std::unique_lock<std::mutex> lock(t->apc_lock);
    t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0 || t->stop_requested.load(); });
}

bool QueueApc(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3,
              GuestCallbackClass callback_class) {
    GuestCallbackTicket ticket;
    if (PrepareGuestCallback(routine, callback_class, ticket) != GuestCallbackStatus::kReady) return false;
    return QueueApcWithTicket(t, routine, a1, a2, a3, ticket);
}

bool QueueApcWithTicket(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3,
                        const GuestCallbackTicket& expected) {
    if (!KernelWorkersAccepting()) return false;
    GuestCallbackTicket ticket;
    if (PrepareGuestCallback(routine, expected.callback_class, ticket) != GuestCallbackStatus::kReady ||
        expected.target != ticket.target || expected.dispatch_generation != ticket.dispatch_generation ||
        expected.callback_class != ticket.callback_class)
        return false;
    ticket = expected;
    {
        std::lock_guard<std::mutex> lock(t.apc_lock);
        const ThreadState state = t.state.load();
        if (!KernelWorkersAccepting() || state == ThreadState::kExited || state == ThreadState::kCancelled) return false;
        try {
            t.apcs.push_back({ticket, {a1, a2, a3}});
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    t.resume_cv.notify_all();
    SignalChanged();
    return true;
}

bool RunApcs(Thread& t) {
    bool ran = false;
    for (;;) {
        Thread::ApcRecord apc;
        {
            std::lock_guard<std::mutex> lock(t.apc_lock);
            if (!KernelWorkersAccepting() || t.apcs.empty()) {
                t.apcs.clear();
                break;
            }
            apc = std::move(t.apcs.front());
            t.apcs.pop_front();
        }
        uint32_t result = 0;
        const GuestCallbackStatus status =
            CallGuestCallback(*t.ctx, apc.callback, apc.callback.target, apc.arguments, t.stack_top - t.stack_size,
                              t.stack_size, result);
        if (status == GuestCallbackStatus::kInvoked) ran = true;
        if (status == GuestCallbackStatus::kCancelled) break;
    }
    return ran;
}

}  // namespace kernel
