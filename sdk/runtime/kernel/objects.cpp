#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <new>
#include <queue>

#include "kernel/kernel.h"
#include "kernel/worker_lifecycle.h"

namespace kernel {

namespace {

std::mutex g_dispatch;
std::condition_variable g_cv;
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
std::atomic<WaitBlockedHook> g_wait_blocked_hook{nullptr};
std::atomic<WaitSuspendHook> g_wait_suspend_hook{nullptr};
std::atomic<WaitApcHook> g_wait_apc_hook{nullptr};
#endif

std::mutex g_handles_lock;
std::unordered_map<uint32_t, ObjectRef> g_handles;
uint32_t g_next_handle = 0x1000;

std::mutex g_ptr_lock;
struct PtrEntry {
    ObjectRef obj;
    uint32_t serial;
};
std::unordered_map<uint32_t, PtrEntry> g_by_ptr;
uint32_t g_serial = 1;
constexpr uint32_t kStamp = 0x4A424F4Bu;  // written into WaitListHead.Blink of objects the host tracks

std::mutex g_names_lock;
std::unordered_map<std::string, std::weak_ptr<Object>> g_names;

}  // namespace

std::mutex& DispatchLock() { return g_dispatch; }
void SignalChanged() { g_cv.notify_all(); }

#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
void SetWaitBlockedHookForTest(WaitBlockedHook hook) { g_wait_blocked_hook.store(hook); }
void SetWaitSuspendHookForTest(WaitSuspendHook hook) { g_wait_suspend_hook.store(hook); }
void SetWaitApcHookForTest(WaitApcHook hook) { g_wait_apc_hook.store(hook); }
#endif

uint32_t NewHandle(ObjectRef obj) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = std::move(obj);
    return h;
}

bool TryNewHandle(ObjectRef obj, uint32_t& handle) {
    handle = 0;
    try {
        handle = NewHandle(std::move(obj));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return handle != 0;
}

ObjectRef FromHandle(uint32_t handle) {
    if (handle == 0xFFFFFFFE) return CurrentRef();  // NtCurrentThread pseudo handle
    std::lock_guard<std::mutex> lock(g_handles_lock);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool CloseHandle(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    return g_handles.erase(handle) != 0;
}

uint32_t DuplicateHandle(uint32_t handle) {
    ObjectRef o = FromHandle(handle);
    return o ? NewHandle(o) : 0;
}

// serial in WaitListHead.Flink, stamp in Blink
static void Stamp(uint32_t guest, uint32_t serial) {
    W32(guest + 8, serial);
    W32(guest + 12, kStamp);
}

void Bind(ObjectRef obj, uint32_t guest) {
    obj->guest = guest;
    std::lock_guard<std::mutex> lock(g_ptr_lock);
    uint32_t serial = g_serial++;
    g_by_ptr[guest] = {obj, serial};
    Stamp(guest, serial);
}

bool TryBind(ObjectRef obj, uint32_t guest) {
    if (!obj || !guest) return false;
    std::lock_guard<std::mutex> lock(g_ptr_lock);
    uint32_t serial = g_serial++;
    try {
        g_by_ptr[guest] = {obj, serial};
    } catch (const std::bad_alloc&) {
        return false;
    }
    obj->guest = guest;
    Stamp(guest, serial);
    return true;
}

void Unbind(const ObjectRef& obj) {
    if (!obj || !obj->guest) return;
    std::lock_guard<std::mutex> lock(g_ptr_lock);
    auto it = g_by_ptr.find(obj->guest);
    if (it != g_by_ptr.end() && it->second.obj.get() == obj.get()) g_by_ptr.erase(it);
    obj->guest = 0;
}

#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
bool IsPointerBoundForTest(uint32_t guest, const Object* obj) {
    std::lock_guard<std::mutex> lock(g_ptr_lock);
    auto it = g_by_ptr.find(guest);
    return it != g_by_ptr.end() && it->second.obj.get() == obj;
}
#endif

ObjectRef FromPointer(uint32_t guest) {
    if (!guest) return nullptr;
    std::lock_guard<std::mutex> lock(g_ptr_lock);
    auto it = g_by_ptr.find(guest);
    if (it != g_by_ptr.end() && R32(guest + 12) == kStamp && R32(guest + 8) == it->second.serial) return it->second.obj;
    // first sight, or the guest reinitialised the structure: build from the header
    uint8_t type = R8(guest);
    int32_t state = int32_t(R32(guest + 4));
    ObjectRef o;
    switch (type) {
        case 0:
        case 1:
            o = std::make_shared<Object>(ObjType::Event);
            o->notification = type == 0;
            o->signal = state;
            break;
        case 2:
            o = std::make_shared<Object>(ObjType::Mutant);
            o->signal = state;
            break;
        case 5:
            o = std::make_shared<Object>(ObjType::Semaphore);
            o->signal = state;
            o->limit = int32_t(R32(guest + 0x10));
            break;
        case 8:
        case 9:
            o = std::make_shared<Object>(ObjType::Timer);
            o->notification = type == 8;
            o->signal = state;
            break;
        case 6:
            if (it != g_by_ptr.end()) return it->second.obj;  // threads keep their object
            o = std::make_shared<Object>(ObjType::Other);
            break;
        default:
            LOGW("objects: unknown dispatcher type %u at %08X", type, guest);
            if (Current()) LogGuestStack(*Current()->ctx, 64);
            o = std::make_shared<Object>(ObjType::Event);
            o->notification = true;
            o->signal = state;
            break;
    }
    o->guest = guest;
    uint32_t serial = g_serial++;
    g_by_ptr[guest] = {o, serial};
    Stamp(guest, serial);
    return o;
}

ObjectRef FindNamed(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_names_lock);
    auto it = g_names.find(name);
    return it == g_names.end() ? nullptr : it->second.lock();
}

void AddNamed(const std::string& name, ObjectRef obj) {
    std::lock_guard<std::mutex> lock(g_names_lock);
    obj->name = name;
    g_names[name] = obj;
}

void SetSignal(Object& o, int32_t v) {
    o.signal = v;
    if (o.guest) W32(o.guest + 4, uint32_t(v));
}

static uint32_t Me() {
    Thread* t = Current();
    return t ? t->ethread : 1;
}

static bool Satisfied(Object& o, uint32_t me) {
    if (o.type == ObjType::Mutant) return o.signal > 0 || o.owner == me;
    return o.signal > 0;
}

static void Consume(Object& o, uint32_t me) {
    switch (o.type) {
        case ObjType::Event:
        case ObjType::Timer:
            if (!o.notification) SetSignal(o, 0);
            break;
        case ObjType::Semaphore:
            SetSignal(o, o.signal - 1);
            break;
        case ObjType::Mutant:
            if (o.owner == me) {
                o.recursion++;
            } else {
                o.owner = me;
                o.recursion = 1;
                SetSignal(o, 0);
            }
            break;
        default:
            break;
    }
}

int64_t Now100ns() {
    using namespace std::chrono;
    // 1601 to 1970 in 100 ns units
    auto t = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    return t / 100 + 116444736000000000ll;
}

int64_t ToDeadline(int64_t due) {
    using namespace std::chrono;
    int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    if (due < 0) return now + (-due) * 100;  // negative due is relative, positive is absolute system time
    int64_t rel = due - Now100ns();
    return now + std::max<int64_t>(rel, 0) * 100;
}

static uint32_t WaitFor(const std::vector<ObjectRef>& objs, bool wait_all, int64_t timeout, bool alertable) {
    using namespace std::chrono;
    Thread* self = Current();
    uint32_t me = Me();
    bool infinite = timeout == INT64_MAX;
    int64_t deadline = infinite ? 0 : ToDeadline(timeout);
    std::unique_lock<std::mutex> lock(g_dispatch);
    for (;;) {
        if (self && self->stop_requested.load()) return kAlerted;
        if (self && self->suspend.load() > 0) {
            lock.unlock();
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
            if (auto hook = g_wait_suspend_hook.load()) hook(self);
#endif
            SuspendPoint();
            if (self->stop_requested.load()) return kAlerted;
            lock.lock();
            continue;
        }
        if (wait_all) {
            bool all = std::all_of(objs.begin(), objs.end(), [&](const ObjectRef& o) { return o && Satisfied(*o, me); });
            if (all) {
                for (auto& o : objs) Consume(*o, me);
                return kWait0;
            }
        } else {
            for (size_t i = 0; i < objs.size(); i++)
                if (objs[i] && Satisfied(*objs[i], me)) {
                    Consume(*objs[i], me);
                    return kWait0 + uint32_t(i);
                }
        }
        if (alertable && self) {
            bool pending;
            {
                std::lock_guard<std::mutex> al(self->apc_lock);
                pending = !self->apcs.empty();
            }
            if (pending) {
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
                if (auto hook = g_wait_apc_hook.load()) hook(self);
#endif
                lock.unlock();
                if (RunApcs(*self)) return kUserApc;
                if (self->stop_requested.load() || !KernelWorkersAccepting()) return kAlerted;
                lock.lock();
                continue;
            }
        }
        if (!infinite) {
            int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
            if (now >= deadline) return kTimeout;
            // bounded so alertable waits notice new APCs
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
            if (self)
                if (auto hook = g_wait_blocked_hook.load()) hook(self);
#endif
            g_cv.wait_for(lock, nanoseconds(std::min<int64_t>(deadline - now, 50000000)));
        } else {
#ifdef MOSTWANTED_KERNEL_WORKER_TESTS
            if (self)
                if (auto hook = g_wait_blocked_hook.load()) hook(self);
#endif
            g_cv.wait_for(lock, milliseconds(50));
        }
    }
}

uint32_t Wait(const std::vector<ObjectRef>& objs, bool wait_all, int64_t timeout, bool alertable) {
    SuspendPoint();
    uint32_t r = WaitFor(objs, wait_all, timeout, alertable);
    SuspendPoint();
    return r;
}

int32_t SetEvent(Object& o) {
    std::lock_guard<std::mutex> lock(g_dispatch);
    int32_t old = o.signal;
    SetSignal(o, 1);
    g_cv.notify_all();
    return old;
}

int32_t ResetEvent(Object& o) {
    std::lock_guard<std::mutex> lock(g_dispatch);
    int32_t old = o.signal;
    SetSignal(o, 0);
    return old;
}

int32_t PulseEvent(Object& o) {
    // waiters already blocked cannot be told apart here, so a pulse behaves like set for one pass
    std::lock_guard<std::mutex> lock(g_dispatch);
    int32_t old = o.signal;
    SetSignal(o, 1);
    g_cv.notify_all();
    return old;
}

int32_t ReleaseSemaphore(Object& o, int32_t count, uint32_t& status) {
    std::lock_guard<std::mutex> lock(g_dispatch);
    int32_t old = o.signal;
    if (o.limit && old + count > o.limit) {
        status = kSemaphoreLimitExceeded;
        return old;
    }
    SetSignal(o, old + count);
    status = kSuccess;
    g_cv.notify_all();
    return old;
}

int32_t ReleaseMutant(Object& o, uint32_t& status) {
    std::lock_guard<std::mutex> lock(g_dispatch);
    uint32_t me = Me();
    if (o.owner != me) {
        status = kMutantNotOwned;
        return o.signal;
    }
    int32_t old = o.signal;
    if (--o.recursion == 0) {
        o.owner = 0;
        SetSignal(o, 1);
        g_cv.notify_all();
    }
    status = kSuccess;
    return old;
}

// timers and DPCs

namespace {

struct TimerEntry {
    int64_t deadline;
    uint64_t seq;
    ObjectRef timer;
    int32_t period_ms;
    uint32_t dpc;
    uint32_t dpc_routine;
    uint32_t dpc_context;
    GuestCallbackTicket dpc_ticket;
    std::weak_ptr<Thread> apc_thread;
    uint32_t apc_routine, apc_context;
    GuestCallbackTicket apc_ticket;
    bool operator>(const TimerEntry& o) const { return deadline > o.deadline; }
};

std::mutex g_timer_lock;
std::condition_variable g_timer_cv;
std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<TimerEntry>> g_timers;
std::unordered_map<Object*, uint64_t> g_timer_seq;  // latest arm per timer, older entries are stale
uint64_t g_seq = 1;
bool g_timer_thread = false;

std::mutex g_dpc_lock;
std::condition_variable g_dpc_cv;
struct DpcRecord {
    uint32_t dpc;
    uint32_t routine;
    uint32_t context;
    uint32_t arg1;
    uint32_t arg2;
    GuestCallbackTicket ticket;
};
std::deque<DpcRecord> g_dpcs;
bool g_dpc_thread = false;

void ClearDpcInserted(uint32_t dpc) {
#if defined(__APPLE__)
    mem::ScopedPageLease dpc_lease;
    uint8_t inserted = 0;
    if (dpc_lease.Acquire(dpc, 0x1C, mem::kReadWrite) == mem::AccessStatus::kOk)
        mem::Write(dpc + 2, &inserted, sizeof(inserted));
#else
    (void)dpc;
#endif
}

void CancelQueuedDpcs() {
    std::lock_guard<std::mutex> lock(g_dpc_lock);
    for (const DpcRecord& record : g_dpcs) ClearDpcInserted(record.dpc);
    g_dpcs.clear();
    g_dpc_thread = false;
}

}  // namespace

void RunDpcs(X86Context& ctx, WorkerStopToken stop) {
    WorkerStopCallback wake(stop, [] { g_dpc_cv.notify_all(); });
    if (!wake.Ready()) {
        CancelQueuedDpcs();
        return;
    }
    for (;;) {
        DpcRecord d{};
        {
            std::unique_lock<std::mutex> lock(g_dpc_lock);
            g_dpc_cv.wait(lock, [&] { return stop.StopRequested() || !g_dpcs.empty(); });
            if (stop.StopRequested()) {
                lock.unlock();
                CancelQueuedDpcs();
                return;
            }
            d = g_dpcs.front();
            g_dpcs.pop_front();
        }
        GuestDispatchLease dispatch_lease;
        if (!dispatch_lease.Allowed()) {
            ClearDpcInserted(d.dpc);
            CancelQueuedDpcs();
            return;
        }
        uint32_t routine = d.routine, context = d.context;
#if defined(__APPLE__)
        mem::ScopedPageLease dpc_lease;
        std::array<uint32_t, 4> current{};
        uint8_t inserted = 0;
        if (dpc_lease.Acquire(d.dpc, 0x1C, mem::kReadWrite) != mem::AccessStatus::kOk ||
            mem::Read(d.dpc + 2, &inserted, sizeof(inserted)) != mem::AccessStatus::kOk ||
            mem::Read(d.dpc + 0x0C, &current[0], sizeof(current[0])) != mem::AccessStatus::kOk ||
            mem::Read(d.dpc + 0x10, &current[1], sizeof(current[1])) != mem::AccessStatus::kOk ||
            mem::Read(d.dpc + 0x14, &current[2], sizeof(current[2])) != mem::AccessStatus::kOk ||
            mem::Read(d.dpc + 0x18, &current[3], sizeof(current[3])) != mem::AccessStatus::kOk)
            continue;
        inserted = 0;
        if (mem::Write(d.dpc + 2, &inserted, sizeof(inserted)) != mem::AccessStatus::kOk) continue;
        if (current[0] != d.routine || current[1] != d.context || current[2] != d.arg1 || current[3] != d.arg2)
            continue;
#else
        G<uint8_t>(d.dpc + 2) = 0;  // Inserted
        routine = R32(d.dpc + 0x0C);
        context = R32(d.dpc + 0x10);
#endif
        if (routine) {
            const std::array<uint32_t, 4> arguments{d.dpc, context, d.arg1, d.arg2};
            GuestCallbackTicket ticket = d.ticket;
#if !defined(__APPLE__)
            ticket = {routine, 0, d.ticket.callback_class};
#endif
            uint32_t result = 0;
            CallGuestCallback(ctx, ticket, routine, arguments, 0, 0, result);
        }
    }
}

bool QueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2, const GuestCallbackTicket* expected,
              uint32_t expected_context, GuestCallbackClass callback_class) {
    if (!KernelWorkersAccepting()) return false;
    std::lock_guard<std::mutex> lock(g_dpc_lock);
    if (!KernelWorkersAccepting()) return false;
    DpcRecord record{dpc, 0, 0, arg1, arg2, {}};
#if defined(__APPLE__)
    mem::ScopedPageLease dpc_lease;
    uint8_t inserted = 0;
    if (dpc_lease.Acquire(dpc, 0x1C, mem::kReadWrite) != mem::AccessStatus::kOk ||
        mem::Read(dpc + 2, &inserted, sizeof(inserted)) != mem::AccessStatus::kOk || inserted ||
        mem::Read(dpc + 0x0C, &record.routine, sizeof(record.routine)) != mem::AccessStatus::kOk ||
        mem::Read(dpc + 0x10, &record.context, sizeof(record.context)) != mem::AccessStatus::kOk)
        return false;
    if (record.routine && PrepareGuestCallback(record.routine, callback_class, record.ticket) !=
                              GuestCallbackStatus::kReady)
        return false;
    if (expected && (expected->target != record.routine ||
                     expected->dispatch_generation != record.ticket.dispatch_generation ||
                     expected_context != record.context || expected->callback_class != record.ticket.callback_class))
        return false;
    if (expected) record.ticket = *expected;
    if (mem::Write(dpc + 0x14, &arg1, sizeof(arg1)) != mem::AccessStatus::kOk ||
        mem::Write(dpc + 0x18, &arg2, sizeof(arg2)) != mem::AccessStatus::kOk)
        return false;
    inserted = 1;
    if (mem::Write(dpc + 2, &inserted, sizeof(inserted)) != mem::AccessStatus::kOk) return false;
#else
    if (G<uint8_t>(dpc + 2)) return false;
    record.routine = R32(dpc + 0x0C);
    record.context = R32(dpc + 0x10);
    W32(dpc + 0x14, arg1);  // KDPC.SystemArgument1 and 2
    W32(dpc + 0x18, arg2);
    W8(dpc + 2, 1);
    record.ticket = {record.routine, 0, callback_class};
#endif
    try {
        g_dpcs.push_back(record);
    } catch (const std::bad_alloc&) {
#if defined(__APPLE__)
        inserted = 0;
        mem::Write(dpc + 2, &inserted, sizeof(inserted));
#else
        W8(dpc + 2, 0);
#endif
        return false;
    }
    if (!g_dpc_thread) {
        g_dpc_thread = true;
        if (!StartManagedHostThread("dpc", [](X86Context& ctx, WorkerStopToken stop) { RunDpcs(ctx, stop); })) {
            g_dpcs.pop_back();
            g_dpc_thread = false;
#if defined(__APPLE__)
            inserted = 0;
            mem::Write(dpc + 2, &inserted, sizeof(inserted));
#else
            W8(dpc + 2, 0);
#endif
            return false;
        }
    }
#if defined(__APPLE__)
    dpc_lease.Release();
#endif
    g_dpc_cv.notify_one();
    return true;
}

bool RemoveQueuedDpc(uint32_t dpc) {
    std::lock_guard<std::mutex> lock(g_dpc_lock);
    const auto it = std::find_if(g_dpcs.begin(), g_dpcs.end(), [dpc](const DpcRecord& record) {
        return record.dpc == dpc;
    });
    if (it == g_dpcs.end()) return false;
#if defined(__APPLE__)
    mem::ScopedPageLease dpc_lease;
    uint8_t inserted = 0;
    if (dpc_lease.Acquire(dpc, 0x1C, mem::kReadWrite) == mem::AccessStatus::kOk) {
        mem::Write(dpc + 2, &inserted, sizeof(inserted));
    }
#else
    W8(dpc + 2, 0);
#endif
    g_dpcs.erase(it);
    return true;
}

static void TimerLoop(WorkerStopToken stop) {
    using namespace std::chrono;
    WorkerStopCallback wake(stop, [] { g_timer_cv.notify_all(); });
    if (!wake.Ready()) {
        std::lock_guard<std::mutex> lock(g_timer_lock);
        g_timers = {};
        g_timer_seq.clear();
        g_timer_thread = false;
        return;
    }
    std::unique_lock<std::mutex> lock(g_timer_lock);
    for (;;) {
        if (stop.StopRequested()) {
            g_timers = {};
            g_timer_seq.clear();
            g_timer_thread = false;
            return;
        }
        if (g_timers.empty()) {
            g_timer_cv.wait(lock, [&] { return stop.StopRequested() || !g_timers.empty(); });
            continue;
        }
        int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        TimerEntry e = g_timers.top();
        if (e.deadline > now) {
            g_timer_cv.wait_for(lock, nanoseconds(e.deadline - now));
            continue;
        }
        g_timers.pop();
        auto it = g_timer_seq.find(e.timer.get());
        if (it == g_timer_seq.end() || it->second != e.seq) continue;
        if (e.period_ms > 0) {
            e.deadline += int64_t(e.period_ms) * 1000000;
            try {
                g_timers.push(e);
            } catch (const std::bad_alloc&) {
                g_timer_seq.erase(it);
                LOGE("kernel: periodic timer could not be requeued");
                continue;
            }
        } else {
            g_timer_seq.erase(it);
        }
        if (stop.StopRequested()) continue;
        {
            std::lock_guard<std::mutex> dl(g_dispatch);
            SetSignal(*e.timer, 1);
            g_cv.notify_all();
        }
        if (!stop.StopRequested()) {
            const int64_t now = Now100ns();
            const uint32_t low = uint32_t(now), high = uint32_t(uint64_t(now) >> 32);
            if (e.dpc) QueueDpc(e.dpc, low, high, &e.dpc_ticket, e.dpc_context, GuestCallbackClass::kTimerDpc);
            if (auto t = e.apc_thread.lock(); t && e.apc_routine)
                QueueApcWithTicket(*t, e.apc_routine, e.apc_context, low, high, e.apc_ticket);
        }
    }
}

bool SetTimer(ObjectRef timer, int64_t due, int32_t period_ms, uint32_t dpc, std::shared_ptr<Thread> apc_thread,
              uint32_t apc_routine, uint32_t apc_context) {
    if (!timer || !KernelWorkersAccepting()) return false;
    uint32_t dpc_routine = 0, dpc_context = 0;
    GuestCallbackTicket dpc_ticket, apc_ticket;
#if defined(__APPLE__)
    if (dpc) {
        mem::ScopedPageLease dpc_lease;
        if (dpc_lease.Acquire(dpc, 0x1C, mem::kReadWrite) != mem::AccessStatus::kOk ||
            mem::Read(dpc + 0x0C, &dpc_routine, sizeof(dpc_routine)) != mem::AccessStatus::kOk ||
            mem::Read(dpc + 0x10, &dpc_context, sizeof(dpc_context)) != mem::AccessStatus::kOk ||
            (dpc_routine && PrepareGuestCallback(dpc_routine, GuestCallbackClass::kTimerDpc, dpc_ticket) !=
                                GuestCallbackStatus::kReady))
            return false;
    }
#endif
    if (apc_routine && PrepareGuestCallback(apc_routine, GuestCallbackClass::kTimerApc, apc_ticket) !=
                            GuestCallbackStatus::kReady)
        return false;
    std::lock_guard<std::mutex> lock(g_timer_lock);
    if (!KernelWorkersAccepting()) return false;
    if (!g_timer_thread) {
        if (!StartKernelWorker(TimerLoop)) return false;
        g_timer_thread = true;
    }
    {
        std::lock_guard<std::mutex> dl(g_dispatch);
        SetSignal(*timer, 0);
    }
    uint64_t seq = g_seq++;
    try {
        g_timer_seq[timer.get()] = seq;
        g_timers.push({ToDeadline(due), seq, timer, period_ms, dpc, dpc_routine, dpc_context, dpc_ticket,
                       apc_thread, apc_routine, apc_context, apc_ticket});
    } catch (const std::bad_alloc&) {
        g_timer_seq.erase(timer.get());
        return false;
    }
    g_timer_cv.notify_all();
    return true;
}

bool CancelTimer(Object& timer) {
    std::lock_guard<std::mutex> lock(g_timer_lock);
    return g_timer_seq.erase(&timer) != 0;
}

}  // namespace kernel
