#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#include "kernel/kernel.h"
#include "kernel/synthetic_worker.h"
#include "kernel/thread_memory.h"
#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
#include "kernel/guest_exception_chain.h"
#endif
#include "kernel/worker_lifecycle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace kernel {

struct Thread;

namespace {

thread_local ThreadRef t_current;
#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
thread_local Thread* t_synthetic_identity = nullptr;
#endif
struct GuestThreadTermination {};
std::mutex g_threads_lock;
std::vector<ThreadRef> g_threads;
std::atomic<uint32_t> g_next_id{0x10};
#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
std::mutex g_synthetic_worker_lock;
SyntheticWorkerAuthorization g_synthetic_worker_auth;
bool g_synthetic_worker_armed = false;
bool g_synthetic_worker_claimed = false;
bool g_synthetic_worker_callback_entered = false;
Thread* g_synthetic_worker_owner = nullptr;
SyntheticWorkerPreCallbackHook g_synthetic_worker_hook = nullptr;
std::atomic<SyntheticContinuationStatus> g_synthetic_continuation_status{
    SyntheticContinuationStatus::kNotAuthorized};
SyntheticContinuationFault g_synthetic_continuation_fault = SyntheticContinuationFault::kNone;
SyntheticContinuationAdmissionHook g_synthetic_continuation_hook = nullptr;
SyntheticSessionPolicy g_synthetic_session_policy;
bool g_synthetic_session_armed = false;
uint64_t g_synthetic_session_memory_generation = 0;
uint32_t g_synthetic_session_depth = 0;
uint32_t g_synthetic_session_admissions = 0;
uint32_t g_synthetic_session_max_depth = 0;
SyntheticSessionAdmissionHook g_synthetic_session_hook = nullptr;

void ClearSyntheticSessionLocked() {
    g_synthetic_session_armed = false;
    g_synthetic_session_policy = {};
    g_synthetic_session_memory_generation = 0;
    g_synthetic_session_depth = 0;
    g_synthetic_session_hook = nullptr;
}
#endif
#ifdef MOSTWANTED_THREAD_PREPARATION_TESTS
std::atomic<ThreadPreparationFailPoint> g_thread_fail_point{ThreadPreparationFailPoint::kNone};
std::atomic<uint32_t> g_worker_launch_count{0};

bool FailAt(ThreadPreparationFailPoint point) {
    auto expected = point;
    return g_thread_fail_point.compare_exchange_strong(expected, ThreadPreparationFailPoint::kNone);
}
#endif

// guest structures per thread

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
    // TLS sits at the top of the stack. The image CRT stores the negative
    // dword index -(tls_size/4) and reads the block through FS:[4 + index*4]
    // (002B3916, 002B37EE), so the block must end exactly at the stack top.
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
    W32(k + layout::kKpcrExceptionList, 0xFFFFFFFF);
    W32(k + layout::kKpcrStackBase, t.stack_top);
    W32(k + layout::kKpcrStackLimit, t.stack_top - stack_size);
    W32(k + layout::kKpcrSelf, k);
    W32(k + layout::kKpcrSelfPcr, k);
    W32(k + layout::kKpcrPrcb, k + layout::kKpcrPrcbData);
    G<uint8_t>(k + layout::kKpcrIrql) = 0;
    W32(k + layout::kKpcrPrcbData + layout::kPrcbCurrentThread, t.ethread);
    uint32_t e = t.ethread;
    G<uint8_t>(e + layout::kThreadHeaderType) = 6;
    G<uint8_t>(e + layout::kThreadHeaderSize) = uint8_t(kEthreadSize / 4);
    W32(e + layout::kThreadStackBase, t.stack_top);
    W32(e + layout::kThreadStackLimit, t.stack_top - stack_size);
    W32(e + layout::kThreadTlsData, t.tls);
    W32(e + layout::kThreadUniqueThread, t.id);
#endif
    if (!ValidateThreadGuestMemory(t.kpcr, t.ethread, t.stack_top, stack_size, t.tls, t.id)) {
        LOGE("thread: guest KPCR/ETHREAD read-back mismatch");
        FreeGuestBlocks(t);
        return false;
    }
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

Thread* Current() {
#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
    if (t_synthetic_identity) return t_synthetic_identity;
#endif
    return t_current.get();
}
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

#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
bool ArmSyntheticWorker(const SyntheticWorkerAuthorization& authorization) {
    if (authorization.image_identity != kSyntheticWorkerImageIdentity || !authorization.target ||
        !authorization.code_base || authorization.code_end <= authorization.code_base ||
        authorization.target < authorization.code_base || authorization.target >= authorization.code_end ||
        (authorization.continuation_target &&
         (authorization.continuation_target < authorization.code_base ||
          authorization.continuation_target >= authorization.code_end)) ||
        !authorization.dispatch_generation ||
        (authorization.callback_ticket.target &&
         (authorization.callback_ticket.callback_class != GuestCallbackClass::kThreadStart ||
          authorization.callback_ticket.dispatch_generation != authorization.dispatch_generation ||
          authorization.callback_ticket.target < authorization.code_base ||
          authorization.callback_ticket.target >= authorization.code_end)))
        return false;
    std::lock_guard lock(g_synthetic_worker_lock);
    if (g_synthetic_worker_armed || g_synthetic_worker_claimed) return false;
    g_synthetic_worker_auth = authorization;
    g_synthetic_worker_armed = true;
    g_synthetic_continuation_status = SyntheticContinuationStatus::kNotAuthorized;
    g_synthetic_continuation_fault = SyntheticContinuationFault::kNone;
    return true;
}

void DisarmSyntheticWorker() {
    std::lock_guard lock(g_synthetic_worker_lock);
    g_synthetic_worker_armed = false;
    g_synthetic_worker_claimed = false;
    g_synthetic_worker_callback_entered = false;
    g_synthetic_worker_owner = nullptr;
    g_synthetic_worker_auth = {};
    g_synthetic_continuation_fault = SyntheticContinuationFault::kNone;
    ClearSyntheticSessionLocked();
}

bool SyntheticWorkerArmed() {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_worker_armed;
}

SyntheticWorkerClaim ClaimSyntheticWorker(uint32_t target, uint32_t context, uint64_t dispatch_generation) {
    std::lock_guard lock(g_synthetic_worker_lock);
    if (!g_synthetic_worker_armed) return SyntheticWorkerClaim::kNotAuthorized;
    if (g_synthetic_worker_claimed) return SyntheticWorkerClaim::kRejected;
    if (target != g_synthetic_worker_auth.target || context != g_synthetic_worker_auth.context ||
        dispatch_generation != g_synthetic_worker_auth.dispatch_generation ||
        target < g_synthetic_worker_auth.code_base || target >= g_synthetic_worker_auth.code_end)
        return SyntheticWorkerClaim::kRejected;
    g_synthetic_worker_claimed = true;
    g_synthetic_worker_callback_entered = false;
    g_synthetic_worker_owner = Current();
    return SyntheticWorkerClaim::kClaimed;
}

bool SyntheticWorkerClaimHeld() {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_worker_claimed && g_synthetic_worker_owner == Current();
}

bool SyntheticWorkerContinuationAuthorized(uint32_t target, uint64_t dispatch_generation) {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_worker_claimed && g_synthetic_worker_owner == Current() &&
           g_synthetic_worker_auth.continuation_target && target == g_synthetic_worker_auth.continuation_target &&
           dispatch_generation == g_synthetic_worker_auth.dispatch_generation &&
           target >= g_synthetic_worker_auth.code_base && target < g_synthetic_worker_auth.code_end;
}

SyntheticContinuationStatus LastSyntheticContinuationStatusForTest() {
    return g_synthetic_continuation_status.load();
}

void SetLastSyntheticContinuationStatusForTest(SyntheticContinuationStatus status) {
    g_synthetic_continuation_status.store(status);
}

void SetSyntheticContinuationFaultForTest(SyntheticContinuationFault fault) {
    g_synthetic_continuation_fault = fault;
}

SyntheticContinuationFault SyntheticContinuationFaultForTest() { return g_synthetic_continuation_fault; }

void SetSyntheticContinuationAdmissionHookForTest(SyntheticContinuationAdmissionHook hook) {
    std::lock_guard lock(g_synthetic_worker_lock);
    g_synthetic_continuation_hook = hook;
}

void CallSyntheticContinuationAdmissionHookForTest() {
    SyntheticContinuationAdmissionHook hook = nullptr;
    {
        std::lock_guard lock(g_synthetic_worker_lock);
        hook = g_synthetic_continuation_hook;
    }
    if (hook) hook();
}

bool SyntheticWorkerHasNestedCallback() {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_worker_armed && g_synthetic_worker_auth.callback_ticket.target;
}

bool SyntheticWorkerAuthorizesNestedCallback(uint32_t target, uint64_t dispatch_generation) {
    std::lock_guard lock(g_synthetic_worker_lock);
    const GuestCallbackTicket& ticket = g_synthetic_worker_auth.callback_ticket;
    if (!g_synthetic_worker_claimed || g_synthetic_worker_owner != Current() || g_synthetic_worker_callback_entered ||
        ticket.target != target || ticket.callback_class != GuestCallbackClass::kThreadStart ||
        ticket.dispatch_generation != dispatch_generation ||
        dispatch_generation != g_synthetic_worker_auth.dispatch_generation || target < g_synthetic_worker_auth.code_base ||
        target >= g_synthetic_worker_auth.code_end)
        return false;
    g_synthetic_worker_callback_entered = true;
    return true;
}

void FinishSyntheticWorker() {
    std::lock_guard lock(g_synthetic_worker_lock);
    g_synthetic_worker_claimed = false;
    g_synthetic_worker_callback_entered = false;
    g_synthetic_worker_owner = nullptr;
    g_synthetic_worker_armed = false;
    g_synthetic_worker_auth = {};
    ClearSyntheticSessionLocked();
}

bool SyntheticWorkerExitMatches(uint32_t code) {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_worker_claimed && code == g_synthetic_worker_auth.expected_exit;
}

const char* SyntheticCallbackAdmissionName(SyntheticCallbackAdmission admission) {
    switch (admission) {
        case SyntheticCallbackAdmission::kAdmitted: return "admitted";
        case SyntheticCallbackAdmission::kNoSession: return "no active session";
        case SyntheticCallbackAdmission::kInvalidOwner: return "not the session owner";
        case SyntheticCallbackAdmission::kStaleMemory: return "stale memory generation";
        case SyntheticCallbackAdmission::kStaleDispatch: return "stale dispatch generation";
        case SyntheticCallbackAdmission::kUnknownEntry: return "call site is not in the session policy";
        case SyntheticCallbackAdmission::kSlotChanged: return "callback slot changed";
        case SyntheticCallbackAdmission::kUnauthorizedTarget: return "target is not in the session policy";
        case SyntheticCallbackAdmission::kInvalidPolicy: return "invalid session policy";
        case SyntheticCallbackAdmission::kAbiMismatch: return "callback ABI does not match the policy";
        case SyntheticCallbackAdmission::kDepthExceeded: return "callback nesting limit reached";
    }
    return "unknown admission status";
}

bool ArmSyntheticSession(const SyntheticSessionPolicy& policy) {
    if (policy.image_identity != kSyntheticWorkerImageIdentity || !policy.code_base ||
        policy.code_end <= policy.code_base || !policy.dispatch_generation || !policy.max_depth ||
        policy.max_depth > kMaximumSyntheticSessionDepth || !policy.slot_count ||
        policy.slot_count > kMaximumSyntheticSessionSlots) {
        LOGE("session: basic policy rejected");
        return false;
    }
    for (uint32_t i = 0; i < policy.slot_count; ++i) {
        const SyntheticCallbackSlotPolicy& slot = policy.slots[i];
        if (!slot.target || slot.target < policy.code_base || slot.target >= policy.code_end || !slot.call_pc ||
            slot.call_pc < policy.code_base || slot.call_pc >= policy.code_end || slot.argument_count > 4) {
            LOGE("session: slot %u range rejected", i);
            return false;
        }
        if (slot.entry == SyntheticCallbackEntry::kIndirectSlot) {
            if (!slot.slot_address) {
                LOGE("session: slot %u indirect address rejected", i);
                return false;
            }
        } else if (slot.slot_address) {
            LOGE("session: slot %u address rejected", i);
            return false;
        }
        for (uint32_t j = 0; j < i; ++j) {
            const SyntheticCallbackSlotPolicy& other = policy.slots[j];
            if (other.call_pc == slot.call_pc && other.target == slot.target) {
                LOGE("session: slot %u duplicate", i);
                return false;
            }
        }
    }
    if (!mem::IsInitialized() || !mem::Generation()) {
        LOGE("session: memory not initialized");
        return false;
    }
    std::lock_guard lock(g_synthetic_worker_lock);
    if (g_synthetic_session_armed) {
        LOGE("session: already armed");
        return false;
    }
    if (!g_synthetic_worker_armed && !g_synthetic_worker_claimed) {
        LOGE("session: no worker authorization");
        return false;
    }
    if (policy.dispatch_generation != g_synthetic_worker_auth.dispatch_generation ||
        policy.code_base != g_synthetic_worker_auth.code_base || policy.code_end != g_synthetic_worker_auth.code_end) {
        LOGE("session: worker authorization mismatch %llX/%llX %X/%X %X/%X",
             (unsigned long long)policy.dispatch_generation, (unsigned long long)g_synthetic_worker_auth.dispatch_generation,
             policy.code_base, g_synthetic_worker_auth.code_base, policy.code_end, g_synthetic_worker_auth.code_end);
        return false;
    }
    for (uint32_t i = 0; i < policy.slot_count; ++i) {
        const SyntheticCallbackSlotPolicy& slot = policy.slots[i];
        if (slot.entry != SyntheticCallbackEntry::kIndirectSlot) continue;
        uint32_t value = 0;
        if (mem::Read(slot.slot_address, &value, sizeof(value)) != mem::AccessStatus::kOk || value != slot.target) {
            LOGE("session: slot %u contents %08X expected %08X", i, value, slot.target);
            return false;
        }
    }
    g_synthetic_session_policy = policy;
    g_synthetic_session_memory_generation = mem::Generation();
    g_synthetic_session_depth = 0;
    g_synthetic_session_admissions = 0;
    g_synthetic_session_max_depth = 0;
    g_synthetic_session_hook = nullptr;
    g_synthetic_session_armed = true;
    return true;
}

void DisarmSyntheticSession() {
    std::lock_guard lock(g_synthetic_worker_lock);
    ClearSyntheticSessionLocked();
}

bool SyntheticSessionArmed() {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_session_armed;
}

SyntheticCallbackAdmission AdmitSyntheticCallback(SyntheticCallbackEntry entry_kind, uint32_t slot_address,
                                                  uint32_t target, uint32_t call_pc, uint64_t dispatch_generation,
                                                  uint32_t esp, uint32_t* expected_esp) {
    if (expected_esp) *expected_esp = esp;
    SyntheticCallbackSlotPolicy entry;
    SyntheticSessionAdmissionHook hook = nullptr;
    {
        std::lock_guard lock(g_synthetic_worker_lock);
        if (!g_synthetic_session_armed) return SyntheticCallbackAdmission::kNoSession;
        if (!g_synthetic_worker_claimed || g_synthetic_worker_owner != Current())
            return SyntheticCallbackAdmission::kInvalidOwner;
        if (dispatch_generation != g_synthetic_session_policy.dispatch_generation)
            return SyntheticCallbackAdmission::kStaleDispatch;
        if (mem::Generation() != g_synthetic_session_memory_generation)
            return SyntheticCallbackAdmission::kStaleMemory;
        if (g_synthetic_session_depth >= g_synthetic_session_policy.max_depth)
            return SyntheticCallbackAdmission::kDepthExceeded;
        bool found = false;
        bool abi_mismatch = false;
        for (uint32_t i = 0; i < g_synthetic_session_policy.slot_count; ++i) {
            const SyntheticCallbackSlotPolicy& candidate = g_synthetic_session_policy.slots[i];
            if (candidate.entry != entry_kind || candidate.target != target) continue;
            if (entry_kind == SyntheticCallbackEntry::kIndirectSlot && candidate.slot_address != slot_address) continue;
            if (candidate.call_pc != call_pc) {
                abi_mismatch = true;
                continue;
            }
            entry = candidate;
            found = true;
            break;
        }
        if (!found) return abi_mismatch ? SyntheticCallbackAdmission::kAbiMismatch
                                        : SyntheticCallbackAdmission::kUnauthorizedTarget;
        if (entry.entry == SyntheticCallbackEntry::kIndirectSlot) {
            if (slot_address != entry.slot_address) return SyntheticCallbackAdmission::kUnknownEntry;
            uint32_t value = 0;
            if (mem::Read(entry.slot_address, &value, sizeof(value)) != mem::AccessStatus::kOk || value != target)
                return SyntheticCallbackAdmission::kSlotChanged;
        } else if (slot_address) {
            return SyntheticCallbackAdmission::kUnknownEntry;
        }
        hook = g_synthetic_session_hook;
    }
    if (hook) hook();
    {
        std::lock_guard lock(g_synthetic_worker_lock);
        if (!g_synthetic_session_armed) return SyntheticCallbackAdmission::kNoSession;
        if (!g_synthetic_worker_claimed || g_synthetic_worker_owner != Current())
            return SyntheticCallbackAdmission::kInvalidOwner;
        if (dispatch_generation != g_synthetic_session_policy.dispatch_generation)
            return SyntheticCallbackAdmission::kStaleDispatch;
        if (mem::Generation() != g_synthetic_session_memory_generation)
            return SyntheticCallbackAdmission::kStaleMemory;
        if (g_synthetic_session_depth >= g_synthetic_session_policy.max_depth)
            return SyntheticCallbackAdmission::kDepthExceeded;
        if (entry.entry == SyntheticCallbackEntry::kIndirectSlot) {
            uint32_t value = 0;
            if (mem::Read(entry.slot_address, &value, sizeof(value)) != mem::AccessStatus::kOk || value != target)
                return SyntheticCallbackAdmission::kSlotChanged;
        }
        ++g_synthetic_session_depth;
        ++g_synthetic_session_admissions;
        if (g_synthetic_session_depth > g_synthetic_session_max_depth)
            g_synthetic_session_max_depth = g_synthetic_session_depth;
        if (expected_esp)
            *expected_esp = esp + (entry.callee_cleanup ? entry.argument_count * sizeof(uint32_t) : 0);
    }
    return SyntheticCallbackAdmission::kAdmitted;
}

void ReleaseSyntheticCallbackAdmission() {
    std::lock_guard lock(g_synthetic_worker_lock);
    if (g_synthetic_session_depth) --g_synthetic_session_depth;
}

uint32_t SyntheticSessionDepthForTest() {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_session_depth;
}

uint32_t SyntheticSessionAdmissionsForTest() {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_session_admissions;
}

uint32_t SyntheticSessionMaxDepthForTest() {
    std::lock_guard lock(g_synthetic_worker_lock);
    return g_synthetic_session_max_depth;
}

void SetSyntheticSessionAdmissionHookForTest(SyntheticSessionAdmissionHook hook) {
    std::lock_guard lock(g_synthetic_worker_lock);
    g_synthetic_session_hook = hook;
}

void SetSyntheticWorkerThreadIdentityForTest(Thread* thread) { t_synthetic_identity = thread; }

void SetSyntheticWorkerPreCallbackHookForTest(SyntheticWorkerPreCallbackHook hook) {
    std::lock_guard lock(g_synthetic_worker_lock);
    g_synthetic_worker_hook = hook;
}

void CallSyntheticWorkerPreCallbackHookForTest() {
    SyntheticWorkerPreCallbackHook hook = nullptr;
    {
        std::lock_guard lock(g_synthetic_worker_lock);
        hook = g_synthetic_worker_hook;
    }
    if (hook) hook();
}
#endif

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
                            GuestCallbackStatus callback_status = GuestCallbackStatus::kCancelled;
#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS
                            bool synthetic_claimed = false;
                            struct SyntheticFinishGuard {
                                bool& claimed;
                                ~SyntheticFinishGuard() {
                                    if (claimed) FinishSyntheticWorker();
                                }
                            } synthetic_finish{synthetic_claimed};
                            const uint32_t claim_target = t->system_routine ? 0 : t->start_routine;
                            const uint32_t claim_context = t->system_routine ? 0 : t->start_context;
                            const SyntheticWorkerClaim synthetic_claim = ClaimSyntheticWorker(
                                claim_target, claim_context, t->start_routine_ticket.dispatch_generation);
                            if (synthetic_claim == SyntheticWorkerClaim::kClaimed) {
                                synthetic_claimed = true;
                                CallSyntheticWorkerPreCallbackHookForTest();
                                mem::ScopedPageLease callback_slot_lease;
                                auto invoke_synthetic_callback = [&]() {
                                    try {
                                        return CallGuestCallbackForTest(
                                            ctx, t->start_routine_ticket, t->start_routine, start_arguments,
                                            stack_base, t->stack_size, code);
                                    } catch (const SyntheticGuestContinuation& continuation) {
                                        uint32_t continuation_result = 0;
                                        const SyntheticContinuationStatus status =
                                            ResumeSyntheticGuestContinuationForTest(ctx, *t, continuation,
                                                                                    continuation_result);
                                        if (status == SyntheticContinuationStatus::kResumed) {
                                            code = continuation_result;
                                            return GuestCallbackStatus::kInvoked;
                                        }
                                        return GuestCallbackStatus::kRejectedTarget;
                                    }
                                };
                                callback_status = SyntheticWorkerHasNestedCallback() &&
                                                          callback_slot_lease.Acquire(t->start_context, sizeof(uint32_t),
                                                                                      mem::kRead) != mem::AccessStatus::kOk
                                                      ? GuestCallbackStatus::kRejectedTarget
                                                      : invoke_synthetic_callback();
                                if (callback_status == GuestCallbackStatus::kInvoked &&
                                    !SyntheticWorkerExitMatches(code))
                                    callback_status = GuestCallbackStatus::kRejectedTarget;
                            } else if (synthetic_claim == SyntheticWorkerClaim::kRejected) {
                                callback_status = GuestCallbackStatus::kRejectedTarget;
                            } else {
                                callback_status =
                                    t->system_routine
                                        ? CallGuestCallback(ctx, t->system_routine_ticket, t->system_routine,
                                                            system_arguments, stack_base, t->stack_size, code)
                                        : CallGuestCallback(ctx, t->start_routine_ticket, t->start_routine,
                                                            start_arguments, stack_base, t->stack_size, code);
                            }
#else
                            callback_status =
                                t->system_routine
                                    ? CallGuestCallback(ctx, t->system_routine_ticket, t->system_routine,
                                                        system_arguments, stack_base, t->stack_size, code)
                                    : CallGuestCallback(ctx, t->start_routine_ticket, t->start_routine,
                                                        start_arguments, stack_base, t->stack_size, code);
#endif
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
