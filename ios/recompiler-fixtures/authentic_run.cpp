// Phase 2D-19 authentic-run harness implementation. See authentic_run.h for the
// design contract. This file is compiled only when the target defines
// MOSTWANTED_AUTHENTIC_RUN_HARNESS; ordinary builds do not include it.
//
// Execution model. The experiment mapping gives every reviewed guest
// instruction its own generated function (one [[function]] at = pc, end =
// pc + length). Every control transfer therefore leaves the generated function
// and re-enters through a checked dispatch boundary. The gate below owns those
// boundaries: it admits only the reviewed edge, counts one instruction per
// entry, and refuses the terminal boundary before the target executes. No
// generated function is ever returned from to fake a stop, and no per-instruction
// post-execution PC check is used.

#include "kernel/authentic_run.h"
#include "kernel/kernel_data_init.h"
#include "kernel/kernel_objects.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"

#if !defined(MOSTWANTED_AUTHENTIC_RUN_HARNESS)
#error "authentic_run.cpp requires MOSTWANTED_AUTHENTIC_RUN_HARNESS"
#endif

namespace kernel::authentic {
namespace {

constexpr uint32_t kTraceCapacity = 512;
constexpr uint32_t kKernelCapacity = 32;
constexpr uint32_t kMaximumCallDepth = 8;
constexpr uint32_t kHostCallbackReturn = 0xFFFFFFF0;
constexpr uint32_t kStopExitCode = 0x2D190000;

const char* const kImageName255 = "PsCreateSystemThreadEx";
const char* const kImageName187 = "NtClose";
const char* const kImageName277 = "RtlEnterCriticalSection";
const char* const kImageName294 = "RtlLeaveCriticalSection";
const char* const kImageName47 = "HalRegisterShutdownNotification";
const char* const kImageName107 = "KeInitializeDpc";
const char* const kImageName113 = "KeInitializeTimerEx";
constexpr uint32_t kShutdownCallbackLow = 0x00011000u;
constexpr uint32_t kShutdownCallbackHigh = 0x00380000u;

struct AuthenticRunStop final {};
struct AuthenticRunViolation final {};

struct WorkerSlot {
    std::mutex lock;
    bool armed = false;
    bool claimed = false;
    bool completed = false;
    std::atomic<bool> revoked{false};
    std::atomic<bool> cancelled{false};
    Role role = Role::kParent;
    Origin origin = Origin::kStaticPreflight;
    const RouteManifest* manifest = nullptr;
    const RouteManifest* child_manifest = nullptr;
    Thread* thread = nullptr;
    uint32_t thread_id = 0;
    uint32_t claim_id = 0;
    uint64_t dispatch_generation = 0;
    uint64_t memory_generation = 0;
    uint64_t mapping_identity = 0;
    char image_sha1[41] = {};
    char policy_identity[41] = {};
    uint32_t instruction_budget = 0;
    uint32_t transfer_budget = 0;
    uint32_t kernel_budget = 0;
    uint32_t maximum_call_depth = 0;
    uint32_t child_instruction_budget = 0;
    uint32_t child_transfer_budget = 0;
    uint32_t child_kernel_budget = 0;
    uint32_t child_maximum_call_depth = 0;

    // worker-local after claim
    uint32_t node = 0;  // index into manifest->steps, 0 = entry
    uint32_t instructions = 0;
    uint32_t transfers = 0;
    uint32_t kernel_calls = 0;
    uint32_t depth = 0;
    uint32_t maximum_depth_seen = 0;
    uint32_t call_stack[kMaximumCallDepth] = {};
    uint32_t seh_registration = 0;
    uint32_t tls = 0;
    bool seh_verified = false;
    bool tls_verified = false;
    SehCheck seh_checks[16] = {};
    uint8_t seh_check_count = 0;
    bool at_stop = false;
    bool seh_captured = false;
    uint32_t seh_call_esp = 0;
    uint32_t seh_previous_head = 0xFFFFFFFFu;
    uint32_t seh_caller_ebp = 0;
    uint32_t stop_pc = 0;
    uint32_t stop_target = 0;
    Status status = Status::kReady;
    uint64_t sequence = 0;
    TraceRecord trace[kTraceCapacity] = {};
    uint32_t trace_count = 0;
    KernelObservation kernel[kKernelCapacity] = {};
    uint32_t kernel_count = 0;
    bool finished = false;
};

WorkerSlot g_slots[2];
std::mutex g_authorization_lock;
uint64_t g_next_claim = 1;
thread_local WorkerSlot* t_active = nullptr;
thread_local KernelObservation* t_pending_kernel = nullptr;

#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
struct Injection {
    Role role;
    uint32_t pc;
    InjectionHook hook;
    bool fired;
};
Injection g_injections[8];
std::mutex g_injection_lock;
#endif

WorkerSlot& Slot(Role role) { return g_slots[role == Role::kParent ? 0 : 1]; }

void ResetSession() {
    std::lock_guard lock(g_authorization_lock);
    for (WorkerSlot& slot : g_slots) {
        slot.armed = false;
        slot.claimed = false;
        slot.completed = false;
        slot.revoked = false;
        slot.cancelled = false;
        slot.thread = nullptr;
        slot.thread_id = 0;
        slot.manifest = nullptr;
        slot.child_manifest = nullptr;
        slot.trace_count = 0;
        slot.kernel_count = 0;
        slot.status = Status::kReady;
        slot.finished = false;
        slot.at_stop = false;
        slot.claim_id = 0;
    }
}

void CopySha1(char* destination, const char* source) {
    destination[0] = 0;
    if (source) std::snprintf(destination, 41, "%s", source);
}

bool ValidManifestPtr(const RouteManifest* manifest) {
    return manifest && manifest->steps && manifest->count && manifest->entry && manifest->stop &&
           manifest->executed_instructions == manifest->count;
}

uint32_t FindNode(const RouteManifest& manifest, uint32_t pc) {
    for (uint32_t i = 0; i < manifest.count; ++i)
        if (manifest.steps[i].pc == pc) return i;
    return UINT32_MAX;
}

void TraceEvent(WorkerSlot& slot, EventKind kind, uint32_t pc, uint32_t target, uint32_t ordinal = 0,
                uint32_t arguments = 0) {
    if (slot.trace_count >= kTraceCapacity) return;
    TraceRecord& record = slot.trace[slot.trace_count++];
    record.sequence = ++slot.sequence;
    record.worker_id = slot.thread_id;
    record.claim_id = slot.claim_id;
    record.role = uint8_t(slot.role);
    record.origin = uint8_t(slot.origin);
    record.event = uint8_t(kind);
    record.status = uint8_t(slot.status);
    record.pc = pc;
    record.target = target;
    record.instructions = slot.instructions;
    record.transfers = slot.transfers;
    record.kernel_calls = slot.kernel_calls;
    record.ordinal = uint16_t(ordinal);
    record.argument_count = uint16_t(arguments);
    record.seh_registration = slot.seh_registration;
    record.tls = slot.tls;
}

thread_local X86Context* t_context = nullptr;
void TraceContext(WorkerSlot& slot) {
    if (!slot.trace_count) return;
    TraceRecord& record = slot.trace[slot.trace_count - 1];
    if (!t_context) return;
    record.esp = t_context->esp;
    record.ebp = t_context->ebp;
    record.eax = t_context->eax;
    record.ecx = t_context->ecx;
    record.edx = t_context->edx;
}

[[noreturn]] void Violation(WorkerSlot& slot, Status status, uint32_t pc, uint32_t target = 0) {
    if (!slot.finished) {
        slot.status = status;
        slot.stop_pc = pc;
        slot.stop_target = target;
        TraceEvent(slot, EventKind::kViolation, pc, target);
        TraceContext(slot);
        slot.finished = true;
        slot.completed = true;
        slot.claimed = false;
    }
    throw AuthenticRunViolation{};
}

[[noreturn]] void Stop(WorkerSlot& slot, Status status) {
    if (!slot.finished) {
        slot.status = status;
        slot.at_stop = status == Status::kStopped;
        // The stop boundary is a control-transfer boundary. The call instruction
        // itself has retired, so the stop records the complete reviewed count.
        slot.stop_pc = slot.manifest->stop;
        TraceEvent(slot, status == Status::kStopped ? EventKind::kStop : EventKind::kViolation, slot.stop_pc,
                   slot.stop_target);
        TraceContext(slot);
        slot.finished = true;
        slot.completed = true;
        slot.claimed = false;
    }
    throw AuthenticRunStop{};
}

bool Cancelled(WorkerSlot& slot) {
    return slot.revoked.load() || slot.cancelled.load() || !KernelWorkersAccepting() ||
           (Current() && Current()->stop_requested.load());
}

bool GenerationsCurrent(WorkerSlot& slot) {
    return slot.dispatch_generation != 0 && slot.dispatch_generation == dispatch::CallLease().Generation() &&
           slot.memory_generation != 0 && slot.memory_generation == mem::Generation();
}

bool ClaimSlot(WorkerSlot& slot, Thread& thread) {
    std::lock_guard lock(g_authorization_lock);
    if (!slot.armed || slot.claimed || slot.thread != &thread || slot.thread_id != thread.id) return false;
    if (slot.revoked.load() || slot.cancelled.load() || !KernelWorkersAccepting()) return false;
    if (!mem::IsInitialized() || mem::Generation() != slot.memory_generation) return false;
    if (dispatch::MappingIdentity() != slot.mapping_identity) return false;
    if (dispatch::CallLease().Generation() != slot.dispatch_generation) return false;
    if (!ImageIdentityMatches(slot.image_sha1)) return false;
    // The transfer-budget invariant is rechecked at the claim boundary so an
    // authorisation path that bypassed the policy floor still fails closed
    // before any reviewed instruction runs.
    if (!slot.manifest || !ManifestTransfersConsistent(*slot.manifest) ||
        !TransferBudgetSatisfies(*slot.manifest, slot.transfer_budget))
        return false;
    if (slot.role == Role::kParent && slot.child_manifest &&
        (!ManifestTransfersConsistent(*slot.child_manifest) ||
         !TransferBudgetSatisfies(*slot.child_manifest, slot.child_transfer_budget)))
        return false;
    slot.claimed = true;
    slot.claim_id = g_next_claim++;
    slot.completed = false;
    slot.node = 0;
    slot.instructions = 0;
    slot.transfers = 0;
    slot.kernel_calls = 0;
    slot.depth = 0;
    slot.maximum_depth_seen = 0;
    slot.status = Status::kReady;
    slot.trace_count = 0;
    slot.kernel_count = 0;
    slot.sequence = 0;
    slot.seh_captured = false;
    slot.seh_verified = false;
    slot.tls_verified = false;
    slot.seh_check_count = 0;
    for (SehCheck& check : slot.seh_checks) check = {};
    slot.seh_registration = 0;
    slot.tls = 0;
    slot.at_stop = false;
    slot.finished = false;
    return true;
}

void ReleaseThreadState() {
    t_active = nullptr;
    t_context = nullptr;
    t_pending_kernel = nullptr;
}

void EnterNode(WorkerSlot& slot, uint32_t index) {
    const RouteManifest& manifest = *slot.manifest;
    if (index >= manifest.count) Violation(slot, Status::kUnreviewedTransfer, manifest.entry);
    if (Cancelled(slot)) Stop(slot, Status::kCancelled);
    if (!GenerationsCurrent(slot)) Violation(slot, slot.memory_generation != mem::Generation()
                                                         ? Status::kStaleMemory
                                                         : Status::kStaleDispatch,
                                          manifest.steps[index].pc);
    if (slot.instructions + 1 > slot.instruction_budget)
        Violation(slot, Status::kBudgetExceeded, manifest.steps[index].pc);
    slot.node = index;
    ++slot.instructions;
    TraceEvent(slot, EventKind::kInstructionEntry, manifest.steps[index].pc, 0);
#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
    InjectionHook hook = nullptr;
    {
        std::lock_guard lock(g_injection_lock);
        for (Injection& injection : g_injections)
            if (!injection.fired && injection.role == slot.role && injection.pc == manifest.steps[index].pc) {
                injection.fired = true;
                hook = injection.hook;
            }
    }
    if (hook && t_context) hook(*t_context);
#endif
}

bool ManifestHasTrap(const RouteManifest& manifest, uint32_t pc) {
    for (size_t i = 0; i < manifest.trap_count; ++i)
        if (manifest.trap_entries[i] == pc) return true;
    return false;
}

// Captures the reviewed SEH frame before its prologue executes. E is the
// caller's ESP at the call, i.e. the slot holding the last argument.
void CaptureSeh(WorkerSlot& slot, uint32_t call_esp, uint32_t previous_head, uint32_t caller_ebp) {
    slot.seh_captured = true;
    slot.seh_call_esp = call_esp;
    slot.seh_previous_head = previous_head;
    slot.seh_caller_ebp = caller_ebp;
}

bool ReadWord(uint32_t address, uint32_t& value) {
    return mem::Read(address, &value, sizeof(value)) == mem::AccessStatus::kOk;
}

// Read-only verification of the live reviewed SEH record. This deliberately
// does not take page leases: the worker is already stopped, every read is a
// checked 4 KB read, and the frame lives on this worker's own stack. Using the
// lease-taking seh::VerifyRegistration here would self-conflict with the
// worker's code-page lease and the finalisation path.
void RecordSehCheck(WorkerSlot& slot, SehCheckKind kind, SehCheckStatus status, uint32_t expected,
                    uint32_t observed, uint32_t address) {
    if (slot.seh_check_count >= 16) return;
    SehCheck& check = slot.seh_checks[slot.seh_check_count++];
    check.kind = uint8_t(kind);
    check.status = uint8_t(status);
    check.expected = expected;
    check.observed = observed;
    check.address = address;
}

// Read-only verification of the live reviewed SEH record. Every predicate is
// evaluated and recorded, including the ones after the first failure, so a
// future run can identify the exact frame condition without executing again.
// The check never writes guest memory.
bool VerifyStopSeh(WorkerSlot& slot, Thread& thread, const RouteManifest& manifest, uint32_t& registration) {
    registration = 0;
    slot.seh_check_count = 0;
    if (!manifest.seh_call) return true;
    bool ok = true;
    auto check = [&](SehCheckKind kind, bool pass, uint32_t expected, uint32_t observed, uint32_t address) {
        RecordSehCheck(slot, kind, pass ? SehCheckStatus::kPass : SehCheckStatus::kFail, expected, observed, address);
        ok = ok && pass;
    };
    auto skip = [&](SehCheckKind kind, uint32_t expected, uint32_t observed) {
        RecordSehCheck(slot, kind, SehCheckStatus::kSkipped, expected, observed, 0);
    };
    auto unknown = [&](SehCheckKind kind, uint32_t expected, uint32_t address) {
        RecordSehCheck(slot, kind, SehCheckStatus::kUnknown, expected, 0, address);
        ok = false;
    };

    const bool captured = slot.seh_captured && t_context;
    check(SehCheckKind::kCapture, captured, 1, captured ? 1 : 0, 0);
    if (!captured) return false;
    const uint32_t e = slot.seh_call_esp;
    const bool entry_ok = e && (e & 3u) == 0 && e <= UINT32_MAX - 0x18;
    check(SehCheckKind::kEntryAddress, entry_ok, 1, e, e);
    if (!entry_ok) return false;

    const uint32_t stack_top = thread.stack_top;
    const uint32_t stack_bottom = thread.stack_top - thread.stack_size;
    const uint32_t post_prologue_esp = e - manifest.seh_scope_bytes - 0x18;
    const bool bounds_ok = stack_bottom < stack_top && e - 0x14 >= stack_bottom && e + 8 <= stack_top;
    check(SehCheckKind::kStackBounds, bounds_ok, stack_top, e, e);
    const bool esp_ok = t_context->esp <= post_prologue_esp && t_context->esp >= stack_bottom;
    check(SehCheckKind::kPostPrologueEsp, esp_ok, post_prologue_esp, t_context->esp, e);
    // The stop may be inside a nested frame (the kernel-init helper has its
    // own push ebp/mov ebp,esp after the SEH prologue). The live SEH record is
    // checked below at [e-...] and [e+4]; here only require the current frame
    // pointer to be a valid address in this worker's stack.
    const bool ebp_ok = t_context->ebp >= stack_bottom && t_context->ebp < stack_top &&
                        (t_context->ebp & 3u) == 0;
    check(SehCheckKind::kCallerEbp, ebp_ok, e + 4, t_context->ebp, e);

    struct Word {
        uint32_t address;
        uint32_t value;
    } previous{e - 0x0C, 0}, handler{e - 0x08, 0}, scope_flag{e, 0}, scope_table{e - 0x04, 0},
        saved_esp{e - 0x14, 0}, caller_ebp{e + 4, 0}, head{thread.kpcr, 0};
    auto load = [&](Word& word, SehCheckKind kind, uint32_t expected) {
        if (ReadWord(word.address, word.value)) return true;
        unknown(kind, expected, word.address);
        return false;
    };
    const bool previous_loaded = load(previous, SehCheckKind::kPreviousRecord, slot.seh_previous_head);
    const bool handler_loaded = load(handler, SehCheckKind::kHandler, manifest.seh_handler);
    const bool scope_flag_loaded = load(scope_flag, SehCheckKind::kScopeFlag, manifest.seh_scope_flag);
    const bool scope_table_loaded = load(scope_table, SehCheckKind::kScopeTable, manifest.seh_scope_table);
    const bool saved_esp_loaded = load(saved_esp, SehCheckKind::kSavedEsp, post_prologue_esp);
    const bool caller_ebp_loaded = load(caller_ebp, SehCheckKind::kCallerEbpHeld,
                                       slot.seh_caller_ebp ? slot.seh_caller_ebp : 0);
    const bool head_loaded = load(head, SehCheckKind::kChain, e - 0x0C);
    if (previous_loaded)
        check(SehCheckKind::kPreviousRecord, previous.value == slot.seh_previous_head, slot.seh_previous_head,
              previous.value, previous.address);
    if (handler_loaded)
        check(SehCheckKind::kHandler, handler.value == manifest.seh_handler, manifest.seh_handler, handler.value,
              handler.address);
    if (scope_flag_loaded)
        check(SehCheckKind::kScopeFlag, scope_flag.value == manifest.seh_scope_flag, manifest.seh_scope_flag,
              scope_flag.value, scope_flag.address);
    if (scope_table_loaded)
        check(SehCheckKind::kScopeTable, scope_table.value == manifest.seh_scope_table, manifest.seh_scope_table,
              scope_table.value, scope_table.address);
    if (saved_esp_loaded)
        check(SehCheckKind::kSavedEsp, saved_esp.value == post_prologue_esp, post_prologue_esp, saved_esp.value,
              saved_esp.address);
    if (caller_ebp_loaded) {
        if (slot.seh_caller_ebp)
            check(SehCheckKind::kCallerEbpHeld, caller_ebp.value == slot.seh_caller_ebp, slot.seh_caller_ebp,
                  caller_ebp.value, caller_ebp.address);
        else
            skip(SehCheckKind::kCallerEbpHeld, 0, caller_ebp.value);
    }
    if (head_loaded) {
        uint32_t link = head.value;
        bool found = false;
        bool chain_ok = true;
        for (uint32_t depth = 0; depth < 64 && link != 0xFFFFFFFFu; ++depth) {
            if ((link & 3u) || link < stack_bottom || link + 8 > stack_top) {
                chain_ok = false;
                break;
            }
            if (link == e - 0x0C) {
                found = true;
                break;
            }
            uint32_t next = 0;
            if (!ReadWord(link, next)) {
                chain_ok = false;
                break;
            }
            link = next;
        }
        check(SehCheckKind::kChain, chain_ok && found, e - 0x0C, head.value, head.address);
    }

    dispatch::CallLease lease;
    const dispatch::ResolvedTarget resolved = lease.Resolve(manifest.seh_handler);
    const bool handler_resolved = resolved.kind == dispatch::TargetKind::kGeneratedFunction && resolved.function;
    check(SehCheckKind::kHandlerResolve, handler_resolved, 1, uint32_t(resolved.kind), manifest.seh_handler);
    if (manifest.seh_scope_table) {
        uint32_t terminator = 0;
        if (ReadWord(manifest.seh_scope_table, terminator))
            check(SehCheckKind::kScopeTerminator, terminator == 0xFFFFFFFFu, 0xFFFFFFFFu, terminator,
                  manifest.seh_scope_table);
        else
            unknown(SehCheckKind::kScopeTerminator, 0xFFFFFFFFu, manifest.seh_scope_table);
    } else {
        skip(SehCheckKind::kScopeTerminator, 0, 0);
    }
    if (ok) registration = e - 0x0C;
    return ok;
}

void VerifyStopState(WorkerSlot& slot) {
    const RouteManifest& manifest = *slot.manifest;
    Thread* thread = Current();
    if (!thread) {
        slot.status = Status::kInternalError;
        return;
    }
    if (manifest.seh_call) {
        uint32_t registration = 0;
        slot.seh_verified = VerifyStopSeh(slot, *thread, manifest, registration);
        slot.seh_registration = slot.seh_verified ? registration : 0;
        if (!slot.seh_verified) {
            slot.status = Status::kSehViolation;
            return;
        }
    }
    if (manifest.tls_block_size) {
        uint32_t pointer = 0;
        if (!thread->tls || mem::Read(thread->tls, &pointer, sizeof(pointer)) != mem::AccessStatus::kOk) {
            slot.status = Status::kTlsViolation;
            return;
        }
        slot.tls = thread->tls;
        slot.tls_verified = pointer == thread->tls + 4;
        if (!slot.tls_verified) {
            slot.status = Status::kTlsViolation;
            return;
        }
    }
}

void KernelRecord(WorkerSlot& slot, const KernelObservation& observation) {
    if (slot.kernel_count < kKernelCapacity) slot.kernel[slot.kernel_count++] = observation;
}

bool KernelNameMatches(uint32_t ordinal) {
    const char* expected = nullptr;
    switch (ordinal) {
        case 255: expected = kImageName255; break;
        case 187: expected = kImageName187; break;
        case 277: expected = kImageName277; break;
        case 294: expected = kImageName294; break;
        case 47: expected = kImageName47; break;
        case 107: expected = kImageName107; break;
        case 113: expected = kImageName113; break;
        default: return false;
    }
    const char* name = kernel::ExportName(ordinal);
    return name && std::strcmp(name, expected) == 0 && kernel::HasExport(ordinal);
}

bool CheckedCriticalState(uint32_t cs, uint32_t& lock_count, uint32_t& recursion, uint32_t& owner,
                          uint32_t& wait_head_next, uint32_t& wait_head_previous) {
    if (mem::CheckAccess(cs, 0x1C, mem::kReadWrite) != mem::AccessStatus::kOk) return false;
    return mem::Read(cs + 0x10, &lock_count, sizeof(lock_count)) == mem::AccessStatus::kOk &&
           mem::Read(cs + 0x14, &recursion, sizeof(recursion)) == mem::AccessStatus::kOk &&
           mem::Read(cs + 0x18, &owner, sizeof(owner)) == mem::AccessStatus::kOk &&
           mem::Read(cs + 0x08, &wait_head_next, sizeof(wait_head_next)) == mem::AccessStatus::kOk &&
           mem::Read(cs + 0x0C, &wait_head_previous, sizeof(wait_head_previous)) == mem::AccessStatus::kOk;
}

enum class CriticalResult : uint8_t { kOk, kBadObject, kBadState, kContended, kMemory };

CriticalResult CheckedCriticalEnter(uint32_t cs, uint32_t expected, uint32_t ethread) {
    if (cs != expected) return CriticalResult::kBadObject;
    uint32_t lock_count = 0, recursion = 0, owner = 0, next = 0, previous = 0;
    if (!CheckedCriticalState(cs, lock_count, recursion, owner, next, previous)) return CriticalResult::kMemory;
    // The reviewed section starts free and its wait list is empty. A busy
    // section is outside this experiment: do not wait and never deliver a
    // callback from inside the kernel import.
    if (next != cs + 0x08 || previous != cs + 0x08) return CriticalResult::kBadState;
    uint32_t observed = 0;
    if (mem::AtomicFetchAdd32(cs + 0x10, 1, observed) != mem::AccessStatus::kOk ||
        !CheckedCriticalState(cs, lock_count, recursion, owner, next, previous))
        return CriticalResult::kMemory;
    if (observed == 0xFFFFFFFFu) {
        const uint32_t one = 1;
        if (mem::Write(cs + 0x18, &ethread, sizeof(ethread)) != mem::AccessStatus::kOk ||
            mem::Write(cs + 0x14, &one, sizeof(one)) != mem::AccessStatus::kOk)
            return CriticalResult::kMemory;
        return CriticalResult::kOk;
    }
    if (owner == ethread) {
        const uint32_t next_recursion = recursion + 1;
        if (mem::Write(cs + 0x14, &next_recursion, sizeof(next_recursion)) != mem::AccessStatus::kOk)
            return CriticalResult::kMemory;
        return CriticalResult::kOk;
    }
    uint32_t undone = 0;
    (void)mem::AtomicFetchAdd32(cs + 0x10, 0xFFFFFFFFu, undone);
    return CriticalResult::kContended;
}

CriticalResult CheckedCriticalLeave(uint32_t cs, uint32_t expected, uint32_t ethread) {
    if (cs != expected) return CriticalResult::kBadObject;
    uint32_t lock_count = 0, recursion = 0, owner = 0, next = 0, previous = 0;
    if (!CheckedCriticalState(cs, lock_count, recursion, owner, next, previous)) return CriticalResult::kMemory;
    if (next != cs + 0x08 || previous != cs + 0x08) return CriticalResult::kBadState;
    if (recursion != 1 || owner != ethread) return CriticalResult::kBadState;
    const uint32_t zero = 0, free_count = 0xFFFFFFFFu;
    if (mem::Write(cs + 0x18, &zero, sizeof(zero)) != mem::AccessStatus::kOk ||
        mem::Write(cs + 0x14, &zero, sizeof(zero)) != mem::AccessStatus::kOk ||
        mem::Write(cs + 0x10, &free_count, sizeof(free_count)) != mem::AccessStatus::kOk)
        return CriticalResult::kMemory;
    return CriticalResult::kOk;
}

void InvokeReviewedKernel(WorkerSlot& slot, X86Context& ctx, uint8_t* base, const route::Step& step,
                          uint32_t return_address, uint32_t call_pc, uint32_t slot_address) {
    KernelObservation observation;
    observation.ordinal = step.ordinal;
    observation.call_pc = call_pc;
    observation.return_pc = return_address;
    observation.slot = step.slot;
    observation.esp_before = ctx.esp;
    observation.status = Status::kReady;
    if (slot.kernel_calls + 1 > slot.kernel_budget)
        Violation(slot, Status::kBudgetExceeded, call_pc, step.target);
    ++slot.kernel_calls;
    const uint32_t argument_bytes = uint32_t(step.arguments) * sizeof(uint32_t);
    if (!argument_bytes || mem::CheckAccess(ctx.esp, argument_bytes, mem::kRead) != mem::AccessStatus::kOk)
        Violation(slot, Status::kKernelRejected, call_pc, step.target);
    uint32_t first_argument = 0;
    if (mem::Read(ctx.esp, &first_argument, sizeof(first_argument)) != mem::AccessStatus::kOk)
        Violation(slot, Status::kKernelRejected, call_pc, step.target);
    observation.argument = first_argument;
    if (slot_address) {
        uint32_t live = 0;
        if (mem::Read(slot_address, &live, sizeof(live)) != mem::AccessStatus::kOk ||
            live != dispatch::KernelThunkAddress(step.ordinal))
            Violation(slot, Status::kKernelRejected, call_pc, step.target);
    }
    if (!KernelNameMatches(step.ordinal)) Violation(slot, Status::kKernelRejected, call_pc, step.target);
    TraceEvent(slot, EventKind::kKernelCall, call_pc, step.target, step.ordinal, step.arguments);
    const uint32_t stack_address = ctx.esp - 4;
    x86_mem_write<uint32_t>(call_pc, stack_address, return_address);
    ctx.esp = stack_address;
    t_pending_kernel = &observation;
    if (step.ordinal == 277 || step.ordinal == 294) {
        Thread* thread = Current();
        if (!thread) Violation(slot, Status::kInternalError, call_pc, step.target);
        const CriticalResult result =
            step.ordinal == 277
                ? CheckedCriticalEnter(first_argument, slot.manifest->critical_section_object, thread->ethread)
                : CheckedCriticalLeave(first_argument, slot.manifest->critical_section_object, thread->ethread);
        ctx.eax = result == CriticalResult::kOk ? 0 : kInvalidParameter;
        // The checked implementation is a stdcall service: pop its return
        // address and its one argument exactly as the production export does.
        ctx.esp = observation.esp_before + argument_bytes;
        if (result != CriticalResult::kOk) {
            observation.status = result == CriticalResult::kContended ? Status::kKernelRejected
                                                                      : Status::kKernelAbiMismatch;
            observation.esp_after = ctx.esp;
            observation.result = ctx.eax;
            KernelRecord(slot, observation);
            t_pending_kernel = nullptr;
            Violation(slot, observation.status, call_pc, step.target);
        }
    } else {
        x86_kernel(ctx, base, step.ordinal);
    }
    t_pending_kernel = nullptr;
    const uint32_t expected_esp = observation.esp_before + argument_bytes;
    if (ctx.esp != expected_esp) {
        observation.status = Status::kKernelAbiMismatch;
        observation.esp_after = ctx.esp;
        observation.result = ctx.eax;
        KernelRecord(slot, observation);
        Violation(slot, Status::kKernelAbiMismatch, call_pc, step.target);
    }
    uint32_t actual_return = 0;
    if (mem::Read(stack_address, &actual_return, sizeof(actual_return)) != mem::AccessStatus::kOk ||
        actual_return != return_address)
        Violation(slot, Status::kKernelAbiMismatch, call_pc, step.target);
    observation.esp_after = ctx.esp;
    observation.result = ctx.eax;
    KernelRecord(slot, observation);
    TraceEvent(slot, EventKind::kKernelReturn, call_pc, step.target, step.ordinal, step.arguments);
    TraceContext(slot);
}

}  // namespace

const char* StatusName(Status status) {
    switch (status) {
        case Status::kReady: return "ready";
        case Status::kStopped: return "stopped";
        case Status::kExecutionRefused: return "execution-refused";
        case Status::kAuthenticExecutionNotAuthorised: return "authentic-execution-not-authorised";
        case Status::kSyntheticTestsDisabled: return "synthetic-tests-disabled";
        case Status::kInvalidImage: return "invalid-image";
        case Status::kInvalidRoute: return "invalid-route";
        case Status::kInvalidMapping: return "invalid-mapping";
        case Status::kInvalidPolicy: return "invalid-policy";
        case Status::kInvalidThread: return "invalid-thread";
        case Status::kStaleDispatch: return "stale-dispatch";
        case Status::kStaleMemory: return "stale-memory";
        case Status::kShutdown: return "shutdown";
        case Status::kNotClaimed: return "not-claimed";
        case Status::kWrongWorker: return "wrong-worker";
        case Status::kRevoked: return "revoked";
        case Status::kUnreviewedTransfer: return "unreviewed-transfer";
        case Status::kUnreviewedInstruction: return "unreviewed-instruction";
        case Status::kForbiddenBranch: return "forbidden-branch";
        case Status::kBudgetExceeded: return "budget-exceeded";
        case Status::kCallDepthExceeded: return "call-depth-exceeded";
        case Status::kKernelRejected: return "kernel-rejected";
        case Status::kKernelAbiMismatch: return "kernel-abi-mismatch";
        case Status::kMemoryViolation: return "memory-violation";
        case Status::kUnsupportedInstruction: return "unsupported-instruction";
        case Status::kSehViolation: return "seh-violation";
        case Status::kTlsViolation: return "tls-violation";
        case Status::kCodePageDenied: return "code-page-denied";
        case Status::kCancelled: return "cancelled";
        case Status::kInternalError: return "internal-error";
    }
    return "unknown";
}

namespace {

}  // namespace

const RouteManifest& AuthenticParentManifest() {
    static const RouteManifest manifest{
        route::kParentRoute,
        route::kParentRouteCount,
        route::kParentEntry,
        route::kParentStop,
        route::kParentExecutedInstructions,
        route::ComputeManifestIdentity(route::kParentRoute, route::kParentRouteCount, route::kParentEntry,
                                       route::kParentStop, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       route::kParentRequiredTransfers),
        route::kParentTrapEntries,
        sizeof(route::kParentTrapEntries) / sizeof(route::kParentTrapEntries[0]),
        0, 0, 0, 0, 0, 0, 0, 0, 0, route::kParentRequiredTransfers,
    };
    return manifest;
}

const RouteManifest& AuthenticChildManifest() {
    static const RouteManifest manifest{
        route::kChildRoute,
        route::kChildRouteCount,
        route::kChildEntry,
        route::kChildStop,
        route::kChildExecutedInstructions,
        route::ComputeManifestIdentity(route::kChildRoute, route::kChildRouteCount, route::kChildEntry,
                                       route::kChildStop, route::kSehCall, route::kSehEntry, route::kSehHandler,
                                       route::kSehScopeTable, route::kSehScopeBytes, route::kSehScopeFlag,
                                       route::kTlsBlockSize, route::kCriticalSectionObject,
                                       route::kChildStartRoutine, route::kChildRequiredTransfers),
        route::kChildTrapEntries,
        sizeof(route::kChildTrapEntries) / sizeof(route::kChildTrapEntries[0]),
        route::kSehCall,
        route::kSehEntry,
        route::kSehHandler,
        route::kSehScopeTable,
        route::kSehScopeBytes,
        route::kSehScopeFlag,
        route::kTlsBlockSize,
        route::kCriticalSectionObject,
        route::kChildStartRoutine,
        route::kChildRequiredTransfers,
    };
    return manifest;
}

uint64_t ManifestIdentity(const RouteManifest& manifest) {
    return route::ComputeManifestIdentity(manifest.steps, manifest.count, manifest.entry, manifest.stop,
                                          manifest.seh_call, manifest.seh_entry, manifest.seh_handler,
                                          manifest.seh_scope_table, manifest.seh_scope_bytes, manifest.seh_scope_flag,
                                          manifest.tls_block_size, manifest.critical_section_object,
                                          manifest.start_routine, manifest.required_transfers);
}

bool ManifestTransfersConsistent(const RouteManifest& manifest) {
    if (!manifest.required_transfers) return true;
    return manifest.steps && manifest.count &&
           manifest.required_transfers == route::ComputeRequiredTransfers(manifest.steps, manifest.count);
}

bool TransferBudgetSatisfies(const RouteManifest& manifest, uint32_t budget) {
    return !manifest.required_transfers || budget >= manifest.required_transfers;
}

uint64_t RuntimeMappingIdentity() { return dispatch::MappingIdentity(); }
uint32_t RuntimeMappingCount() { return dispatch::MappingCount(); }

Status PrepareAuthenticKernelData(AuthenticKernelData* data) {
    if (data) *data = AuthenticKernelData{};
    if (!mem::IsInitialized()) return Status::kShutdown;
    const uint32_t launch = RegisterData(164, "LaunchDataPage", 4);
    const uint32_t hardware = RegisterData(322, "XboxHardwareInfo", 8);
    const uint32_t version = RegisterData(324, "XboxKrnlVersion", 8);
    const uint32_t image_name = RegisterData(326, "XeImageFileName", 8 + 64);
    const uint32_t disk_model = RegisterData(41, "HalDiskModelNumber", 8 + 48);
    const uint32_t disk_serial = RegisterData(42, "HalDiskSerialNumber", 8 + 32);
    const uint32_t cache_count = RegisterData(40, "HalDiskCachePartitionCount", 4);
    const uint32_t smc_video = RegisterData(356, "HalBootSMCVideoMode", 4);
    const uint32_t ide = RegisterData(357, "IdexChannelObject", 0x40);
    const uint32_t debugger = RegisterData(89, "KdDebuggerNotPresent", 4);
    const uint32_t increment = RegisterData(157, "KeTimeIncrement", 4);
    if (!launch || !hardware || !version || !image_name || !disk_model || !disk_serial || !cache_count ||
        !smc_video || !ide || !debugger || !increment)
        return Status::kInternalError;
    const KernelDataLayout layout{launch, hardware, version, image_name, disk_model, disk_serial, cache_count,
                                  smc_video, ide, debugger, increment};
    std::string error;
    if (!InitializeKernelData(layout, error)) return Status::kInternalError;
    uint32_t flags = 0;
    uint16_t build = 0;
    if (mem::Read(hardware, &flags, sizeof(flags)) != mem::AccessStatus::kOk ||
        mem::Read(version + 4, &build, sizeof(build)) != mem::AccessStatus::kOk)
        return Status::kMemoryViolation;
    if (data) {
        data->hardware_info = hardware;
        data->kernel_version = version;
        data->hardware_flags = flags;
        data->build = build;
    }
    return Status::kReady;
}

Status AuthorizeRunForThread(Thread* thread, const AuthorizationPolicy& policy, RunAuthorization* authorization) {
    if (authorization) *authorization = RunAuthorization{};
    if (!thread || !ValidManifestPtr(policy.manifest) || !policy.image_sha1 || !*policy.image_sha1 ||
        !policy.mapping_identity || !policy.dispatch_generation || !policy.memory_generation ||
        !policy.instruction_budget || !policy.transfer_budget || !policy.kernel_budget ||
        !policy.maximum_call_depth || policy.maximum_call_depth > kMaximumCallDepth)
        return Status::kInvalidPolicy;
    if (policy.role == Role::kParent &&
        (!policy.child_manifest || !ValidManifestPtr(policy.child_manifest) ||
         !policy.child_instruction_budget || !policy.child_transfer_budget || !policy.child_kernel_budget ||
         !policy.child_maximum_call_depth))
        return Status::kInvalidPolicy;
    // The transfer budget must cover the manifest-derived total, terminal stop
    // included. Parent and child are checked independently against their own
    // manifests; a shared or per-worker mis-accounting fails here.
    if (!ManifestTransfersConsistent(*policy.manifest) ||
        !TransferBudgetSatisfies(*policy.manifest, policy.transfer_budget))
        return Status::kInvalidPolicy;
    if (policy.role == Role::kParent &&
        (!ManifestTransfersConsistent(*policy.child_manifest) ||
         !TransferBudgetSatisfies(*policy.child_manifest, policy.child_transfer_budget)))
        return Status::kInvalidPolicy;
    if (policy.origin == Origin::kAuthenticImage) return Status::kAuthenticExecutionNotAuthorised;
    if (policy.origin != Origin::kSyntheticFixture) return Status::kInvalidPolicy;
#ifndef MOSTWANTED_AUTHENTIC_RUN_TESTS
    return Status::kSyntheticTestsDisabled;
#else
    if (!mem::IsInitialized() || mem::Generation() != policy.memory_generation) return Status::kStaleMemory;
    if (!KernelWorkersAccepting()) return Status::kShutdown;
    if (dispatch::MappingIdentity() != policy.mapping_identity) return Status::kInvalidMapping;
    if (dispatch::CallLease().Generation() != policy.dispatch_generation) return Status::kStaleDispatch;
    if (!ImageIdentityMatches(policy.image_sha1)) return Status::kInvalidImage;
    WorkerSlot& slot = Slot(policy.role);
    std::lock_guard lock(g_authorization_lock);
    slot.armed = true;
    slot.claimed = false;
    slot.completed = false;
    slot.revoked = false;
    slot.cancelled = false;
    slot.role = policy.role;
    slot.origin = policy.origin;
    slot.manifest = policy.manifest;
    slot.child_manifest = policy.child_manifest;
    slot.thread = thread;
    slot.thread_id = policy.expected_thread_id ? policy.expected_thread_id : thread->id;
    if (slot.thread_id != thread->id) return Status::kInvalidThread;
    slot.dispatch_generation = policy.dispatch_generation;
    slot.memory_generation = policy.memory_generation;
    slot.mapping_identity = policy.mapping_identity;
    CopySha1(slot.image_sha1, policy.image_sha1);
    CopySha1(slot.policy_identity, policy.policy_identity);
    slot.instruction_budget = policy.instruction_budget;
    slot.transfer_budget = policy.transfer_budget;
    slot.kernel_budget = policy.kernel_budget;
    slot.maximum_call_depth = policy.maximum_call_depth;
    slot.child_instruction_budget = policy.child_instruction_budget;
    slot.child_transfer_budget = policy.child_transfer_budget;
    slot.child_kernel_budget = policy.child_kernel_budget;
    slot.child_maximum_call_depth = policy.child_maximum_call_depth;
    slot.claim_id = 0;
    slot.trace_count = 0;
    slot.kernel_count = 0;
    slot.status = Status::kReady;
    if (authorization) authorization->id_ = reinterpret_cast<uintptr_t>(&slot) ^ 0x2D190000u;
    return Status::kReady;
#endif
}

Status AuthorizeAuthenticRun(Role role, const AuthorizationPolicy& policy, RunAuthorization* authorization) {
    if (authorization) *authorization = RunAuthorization{};
    if (!policy.manifest || !ValidManifestPtr(policy.manifest) || !policy.image_sha1 ||
        std::strcmp(policy.image_sha1, kAuthenticImageSha1) != 0)
        return Status::kInvalidImage;
    if (!policy.policy_identity || std::strcmp(policy.policy_identity, kAuthenticCallbackPolicyIdentity) != 0)
        return Status::kInvalidPolicy;
    if (policy.origin != Origin::kAuthenticImage) return Status::kInvalidPolicy;
    // The authentic capability is bound to the compiled-in reviewed manifests,
    // not to any table a caller can substitute.
    const RouteManifest& reviewed_parent = AuthenticParentManifest();
    const RouteManifest& reviewed_child = AuthenticChildManifest();
    if (role == Role::kParent) {
        if (policy.manifest != &reviewed_parent || policy.manifest->identity != reviewed_parent.identity ||
            !policy.child_manifest || policy.child_manifest != &reviewed_child ||
            policy.child_manifest->identity != reviewed_child.identity)
            return Status::kInvalidRoute;
    } else if (policy.manifest != &reviewed_child) {
        return Status::kInvalidRoute;
    }
    // The reviewed manifests must declare their exact transfer totals, and the
    // configured budgets must cover them. This is checked even while execution
    // is disabled so the previous 128-transfer child budget is refused before
    // any authorisation state can be created.
    if (!ManifestTransfersConsistent(*policy.manifest) ||
        !TransferBudgetSatisfies(*policy.manifest, policy.transfer_budget))
        return Status::kInvalidPolicy;
    if (role == Role::kParent &&
        (!ManifestTransfersConsistent(*policy.child_manifest) ||
         !TransferBudgetSatisfies(*policy.child_manifest, policy.child_transfer_budget)))
        return Status::kInvalidPolicy;
    if (policy.dispatch_generation != dispatch::CallLease().Generation()) return Status::kStaleDispatch;
    if (policy.memory_generation != mem::Generation()) return Status::kStaleMemory;
    if (!KernelWorkersAccepting()) return Status::kShutdown;
    if (dispatch::MappingIdentity() != policy.mapping_identity) return Status::kInvalidMapping;
    if (!ImageIdentityMatches(kAuthenticImageSha1)) return Status::kInvalidImage;
    if (!kAuthenticExecutionAuthorised) return Status::kAuthenticExecutionNotAuthorised;
    return Status::kExecutionRefused;
}

Status RevokeRun(const RunAuthorization& authorization) {
    if (!authorization.id()) return Status::kInvalidPolicy;
    for (WorkerSlot& slot : g_slots) {
        if (authorization.id() != (reinterpret_cast<uintptr_t>(&slot) ^ 0x2D190000u)) continue;
        slot.revoked = true;
        return Status::kReady;
    }
    return Status::kInvalidPolicy;
}

void CancelAllRuns() {
    for (WorkerSlot& slot : g_slots) slot.cancelled = true;
}

bool AnyRunActive() {
    for (const WorkerSlot& slot : g_slots)
        if ((slot.armed || slot.claimed) && !slot.completed) return true;
    return false;
}

bool GateActive() { return t_active != nullptr && !t_active->finished; }

bool ThreadHasBoundRun(const Thread* thread) {
    if (!thread || !mem::IsInitialized()) return false;
    for (const WorkerSlot& slot : g_slots)
        if (slot.armed && slot.thread == thread && slot.thread_id == thread->id) return true;
    return false;
}

uint32_t LiveAuthorizationCount() {
    uint32_t count = 0;
    for (const WorkerSlot& slot : g_slots)
        if ((slot.armed || slot.claimed) && !slot.completed) ++count;
    return count;
}

void AdmitGeneratedTransfer(X86Context& ctx, uint32_t target, uint32_t return_address, uint32_t transfer_pc,
                            TransferKind kind) {
    if (!GateActive()) return;
    WorkerSlot& slot = *t_active;
    t_context = &ctx;
    const RouteManifest& manifest = *slot.manifest;
    const route::Step& step = manifest.steps[slot.node];
    if (Cancelled(slot)) Stop(slot, Status::kCancelled);
    if (!GenerationsCurrent(slot))
        Violation(slot, slot.memory_generation != mem::Generation() ? Status::kStaleMemory : Status::kStaleDispatch,
                  transfer_pc, target);
    if (kind == TransferKind::kCall) {
        if (step.kind != route::StepKind::kCall || transfer_pc != step.pc)
            Violation(slot, Status::kUnreviewedTransfer, transfer_pc, target);
        if (return_address != step.next)
            Violation(slot, Status::kUnreviewedTransfer, transfer_pc, target);
        if (target == manifest.stop) {
            // The reviewed dynamic start-routine call retires (its return
            // address is pushed) and the callee entry is refused. Zero
            // instructions of the target execute. The terminal boundary is
            // itself a transfer and must fit the configured budget.
            if (slot.transfers + 1 > slot.transfer_budget)
                Violation(slot, Status::kBudgetExceeded, transfer_pc, target);
            ++slot.transfers;
            x86_mem_write<uint32_t>(transfer_pc, ctx.esp - 4, return_address);
            ctx.esp -= 4;
            slot.stop_target = target;
            Stop(slot, Status::kStopped);
        }
        const uint32_t callee = FindNode(manifest, target);
        if (callee == UINT32_MAX || target != step.target)
            Violation(slot, Status::kUnreviewedTransfer, transfer_pc, target);
        if (slot.transfers + 1 > slot.transfer_budget)
            Violation(slot, Status::kBudgetExceeded, transfer_pc, target);
        if (slot.depth + 1 > slot.maximum_call_depth)
            Violation(slot, Status::kCallDepthExceeded, transfer_pc, target);
        slot.call_stack[slot.depth++] = slot.node;
        slot.maximum_depth_seen = std::max(slot.maximum_depth_seen, slot.depth);
        ++slot.transfers;
        TraceEvent(slot, EventKind::kCallTransfer, transfer_pc, target);
        TraceContext(slot);
        if (manifest.seh_call && transfer_pc == manifest.seh_call) {
            Thread* thread = Current();
            if (!thread) Violation(slot, Status::kInvalidThread, transfer_pc, target);
            uint32_t previous = 0;
            if (mem::Read(thread->kpcr, &previous, sizeof(previous)) != mem::AccessStatus::kOk)
                Violation(slot, Status::kSehViolation, transfer_pc, target);
            CaptureSeh(slot, ctx.esp, previous, ctx.ebp);
        }
        EnterNode(slot, callee);
        return;
    }
    // A tail transfer completes the current instruction. It may be the
    // reviewed fallthrough/conditional branch of a sequential, conditional,
    // call or kernel-call node, or an unconditional jump.
    uint32_t expected = UINT32_MAX;
    switch (step.kind) {
        case route::StepKind::kSequential:
        case route::StepKind::kKernelCall:
        case route::StepKind::kCall:
            expected = step.next;
            break;
        case route::StepKind::kConditional:
            expected = step.taken ? step.target : step.next;
            if (target != expected && (target == step.target || target == step.next))
                Violation(slot, Status::kForbiddenBranch, transfer_pc, target);
            break;
        case route::StepKind::kRet:
            break;
    }
    if (target == manifest.stop) {
        // The mandatory stop is a transfer like any other. Counting it here
        // means a budget of required_transfers - 1 refuses the stop instead of
        // reporting a completed route.
        if (slot.transfers + 1 > slot.transfer_budget)
            Violation(slot, Status::kBudgetExceeded, transfer_pc, target);
        ++slot.transfers;
        slot.stop_target = target;
        Stop(slot, Status::kStopped);
    }
    if (expected == UINT32_MAX || target != expected)
        Violation(slot, Status::kUnreviewedTransfer, transfer_pc, target);
    const uint32_t next = FindNode(manifest, target);
    if (next == UINT32_MAX) Violation(slot, Status::kUnreviewedTransfer, transfer_pc, target);
    if (slot.transfers + 1 > slot.transfer_budget)
        Violation(slot, Status::kBudgetExceeded, transfer_pc, target);
    ++slot.transfers;
    TraceEvent(slot, EventKind::kJumpTransfer, transfer_pc, target);
    TraceContext(slot);
    EnterNode(slot, next);
}

void AdmitGuestReturn(X86Context& ctx, uint32_t pc, uint32_t pop_bytes) {
    (void)pop_bytes;
    if (!GateActive()) return;
    WorkerSlot& slot = *t_active;
    t_context = &ctx;
    const RouteManifest& manifest = *slot.manifest;
    const route::Step& step = manifest.steps[slot.node];
    if (step.kind != route::StepKind::kRet || step.pc != pc)
        Violation(slot, Status::kUnreviewedTransfer, pc);
    if (pop_bytes != step.ret_pop_bytes)
        Violation(slot, Status::kUnreviewedTransfer, pc, pop_bytes);
    if (slot.depth == 0) Violation(slot, Status::kUnreviewedTransfer, pc);
    uint32_t return_address = 0;
    if (mem::Read(ctx.esp, &return_address, sizeof(return_address)) != mem::AccessStatus::kOk ||
        return_address != step.next)
        Violation(slot, Status::kUnreviewedTransfer, pc, return_address);
    const uint32_t call_node = slot.call_stack[--slot.depth];
    if (manifest.steps[call_node].next != step.next)
        Violation(slot, Status::kUnreviewedTransfer, pc, return_address);
    if (slot.transfers + 1 > slot.transfer_budget)
        Violation(slot, Status::kBudgetExceeded, pc, return_address);
    ++slot.transfers;
    TraceEvent(slot, EventKind::kReturnTransfer, pc, return_address);
    TraceContext(slot);
    slot.node = call_node;
}

void AdmitKernelCall(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t return_address, uint32_t transfer_pc,
                     uint32_t slot_address) {
    if (!GateActive()) return;
    WorkerSlot& slot = *t_active;
    t_context = &ctx;
    const RouteManifest& manifest = *slot.manifest;
    const route::Step& step = manifest.steps[slot.node];
    if (step.kind != route::StepKind::kKernelCall || step.pc != transfer_pc || step.ordinal == 0)
        Violation(slot, Status::kKernelRejected, transfer_pc, target);
    if (target != dispatch::KernelThunkAddress(step.ordinal) || return_address != step.next)
        Violation(slot, Status::kKernelRejected, transfer_pc, target);
    InvokeReviewedKernel(slot, ctx, base, step, return_address, transfer_pc, slot_address);
}

GuestCallbackStatus RunBoundWorker(X86Context& ctx, Thread& thread) {
    WorkerSlot* slot = nullptr;
    for (WorkerSlot& candidate : g_slots)
        if (candidate.armed && candidate.thread == &thread) slot = &candidate;
    if (!slot || !ClaimSlot(*slot, thread)) return GuestCallbackStatus::kCancelled;
    t_active = slot;
    t_context = &ctx;
    const RouteManifest& manifest = *slot->manifest;
    // Pin the reviewed code run and the worker's stack for the whole
    // experiment. A page mutation then fails closed instead of changing code
    // or thread state under a transfer the gate has already admitted.
    mem::ScopedPageLease code_lease;
    if (code_lease.AcquireExecutableRun(manifest.entry, dispatch::CallLease().CodeEnd()) != mem::AccessStatus::kOk) {
        Violation(*slot, Status::kCodePageDenied, manifest.entry);
    }
    // Pin the whole worker stack for the run. A decommit, free or protection
    // change from another thread must wait until the route and the read-only
    // stop verification finish. The verifier uses checked reads only, so it
    // does not take a conflicting lease.
    mem::ScopedPageLease stack_lease;
    const uint32_t stack_base = thread.stack_top - thread.stack_size;
    if (stack_lease.Acquire(stack_base, thread.stack_size, mem::kReadWrite) != mem::AccessStatus::kOk) {
        Violation(*slot, Status::kMemoryViolation, stack_base);
    }
    TraceEvent(*slot, EventKind::kWorkerStart, manifest.entry, 0);
    TraceContext(*slot);
    try {
        if (slot->role == Role::kChild) {
            // The reviewed thread-start callback frame: return sentinel, then
            // the system routine's two arguments. This is the same frame the
            // validated callback path writes.
            const uint32_t initial = ctx.esp;
            const uint32_t stack_bottom = thread.stack_top - thread.stack_size;
            if (initial < stack_bottom + 0x100 || initial > thread.stack_top - 16)
                return GuestCallbackStatus::kInvalidStack;
            const uint32_t frame = initial - 12;
            const uint32_t words[3] = {kHostCallbackReturn, thread.start_routine, thread.start_context};
            if (mem::Write(frame, words, sizeof(words)) != mem::AccessStatus::kOk)
                return GuestCallbackStatus::kStackWriteDenied;
            ctx.esp = frame;
        }
        EnterNode(*slot, 0);
        dispatch::CallLease lease;
        const dispatch::ResolvedTarget resolved = lease.Resolve(manifest.entry);
        if (resolved.kind != dispatch::TargetKind::kGeneratedFunction || !resolved.function)
            Violation(*slot, Status::kInvalidMapping, manifest.entry);
        resolved.function(ctx, mem::Base());
        if (!slot->finished)
            Violation(*slot, Status::kUnreviewedTransfer, manifest.entry, 0);
    } catch (const AuthenticRunStop&) {
    } catch (const AuthenticRunViolation&) {
    } catch (const X86MemoryFault& fault) {
        if (!slot->finished) {
            slot->status = Status::kMemoryViolation;
            slot->stop_pc = fault.instruction;
            TraceEvent(*slot, EventKind::kViolation, fault.instruction, fault.address);
            TraceContext(*slot);
            TraceEvent(*slot, EventKind::kFinalize, fault.instruction, fault.address);
            slot->finished = true;
            slot->completed = true;
            slot->claimed = false;
        }
    } catch (const X86Resume&) {
        if (!slot->finished) {
            slot->status = Status::kSehViolation;
            slot->stop_pc = manifest.stop;
            TraceEvent(*slot, EventKind::kViolation, manifest.stop, 0);
            slot->finished = true;
            slot->completed = true;
            slot->claimed = false;
        }
    } catch (...) {
        if (!slot->finished) {
            slot->status = Status::kInternalError;
            slot->finished = true;
            slot->completed = true;
            slot->claimed = false;
        }
        ReleaseThreadState();
        throw;
    }
    // The stop is final: no guest instruction can execute after it. Release
    // the executable-run lease before the SEH/TLS verification so a pending
    // page mutation can proceed instead of waiting on a lease this worker can
    // no longer need.
    code_lease.Release();
    if (slot->status == Status::kStopped) {
        VerifyStopState(*slot);
        if (slot->status != Status::kStopped) {
            // A verification failure is a violation even though the control
            // boundary itself was the reviewed stop.
            slot->at_stop = false;
            TraceEvent(*slot, EventKind::kViolation, slot->stop_pc, slot->stop_target);
            TraceContext(*slot);
        }
    }
    TraceEvent(*slot, EventKind::kFinalize, slot->stop_pc, slot->stop_target);
    TraceContext(*slot);
    slot->finished = true;
    slot->completed = true;
    slot->claimed = false;
    const Status status = slot->status;
    const bool stopped = status == Status::kStopped;
    ReleaseThreadState();
    return stopped ? GuestCallbackStatus::kInvoked : GuestCallbackStatus::kCancelled;
}

Status BindChildThreadForRequest(const AuthenticSystemThreadRequest& request, const ThreadRef& child,
                                 uint32_t call_pc) {
    WorkerSlot* parent = t_active;
    if (!parent || parent->role != Role::kParent || !parent->child_manifest)
        return Status::kInvalidPolicy;
    if (call_pc != route::kOrdinal255Call && parent->origin == Origin::kAuthenticImage)
        return Status::kInvalidRoute;
    const RouteManifest& child_manifest = *parent->child_manifest;
    if (request.thread_extension_size != 0 || request.reserved || (request.create_flags & 0xFFu) ||
        request.tls_size != child_manifest.tls_block_size || request.start_routine != child_manifest.start_routine ||
        request.system_routine != child_manifest.entry || request.start_context != 0)
        return Status::kInvalidPolicy;
    if (!request.handle_out || mem::CheckAccess(request.handle_out, 4, mem::kRead | mem::kWrite) !=
                                  mem::AccessStatus::kOk)
        return Status::kInvalidPolicy;
    if (request.thread_id_out &&
        mem::CheckAccess(request.thread_id_out, 4, mem::kRead | mem::kWrite) != mem::AccessStatus::kOk)
        return Status::kInvalidPolicy;
    if (!child) return Status::kInvalidThread;
    if (parent->origin == Origin::kAuthenticImage) {
        // Reuse the production-facing assessment as well as the core admission:
        // it prepares and cancels its own candidate and returns evidence only.
        AuthenticSystemThreadRequest assessment = request;
        assessment.policy_identity = parent->policy_identity;
        assessment.parent_id = Current() ? Current()->id : 0;
        assessment.dispatch_generation = parent->dispatch_generation;
        assessment.memory_generation = parent->memory_generation;
        if (AssessAuthenticSystemThreadRequest(assessment).status !=
            AuthenticAdmissionStatus::kValidatedButExecutionProhibited)
            return Status::kKernelRejected;
    }
    SystemRoutineAdmissionSpec spec;
    spec.expected_image_sha1 = parent->image_sha1;
    spec.expected_parent_id = Current() ? Current()->id : 0;
    spec.expected_start_routine = request.start_routine;
    spec.expected_system_routine = request.system_routine;
    spec.dispatch_generation = parent->dispatch_generation;
    spec.memory_generation = parent->memory_generation;
    const SystemRoutineAdmissionResult admission = ValidateSystemRoutineAdmission(*child, spec);
    if (admission.status != SystemRoutineAdmission::kAdmitted) return Status::kInvalidThread;
    // The child's prepared TLS block must be the reviewed size and the child's
    // start/system routines must equal the child manifest endpoints.
    if (uint64_t(child->stack_top) - child->tls != child_manifest.tls_block_size)
        return Status::kTlsViolation;
    if (child->start_routine != child_manifest.start_routine || child->system_routine != child_manifest.entry)
        return Status::kInvalidRoute;
    RunAuthorization authorization;
    AuthorizationPolicy policy;
    policy.origin = parent->origin;
    policy.role = Role::kChild;
    policy.manifest = &child_manifest;
    policy.image_sha1 = parent->image_sha1;
    policy.mapping_identity = parent->mapping_identity;
    policy.dispatch_generation = parent->dispatch_generation;
    policy.memory_generation = parent->memory_generation;
    policy.instruction_budget = parent->child_instruction_budget;
    policy.transfer_budget = parent->child_transfer_budget;
    policy.kernel_budget = parent->child_kernel_budget;
    policy.maximum_call_depth = parent->child_maximum_call_depth;
    const Status status = AuthorizeRunForThread(child.get(), policy, &authorization);
    if (status != Status::kReady) return status;
    {
        std::lock_guard lock(g_authorization_lock);
        WorkerSlot& slot = Slot(Role::kChild);
        slot.thread = child.get();
        slot.thread_id = child->id;
        slot.armed = true;
    }
    return Status::kReady;
}

bool EvidenceForRole(Role role, RunEvidence* evidence) {
    if (!evidence) return false;
    const WorkerSlot& slot = Slot(role);
    if (!slot.manifest || !slot.trace_count) return false;
    evidence->role = uint32_t(role);
    evidence->origin = uint32_t(slot.origin);
    evidence->thread_id = slot.thread_id;
    evidence->claim_id = slot.claim_id;
    evidence->status = slot.status;
    evidence->stop_pc = slot.stop_pc;
    evidence->stop_target = slot.stop_target;
    evidence->instructions = slot.instructions;
    evidence->transfers = slot.transfers;
    evidence->required_transfers = slot.manifest->required_transfers;
    evidence->kernel_calls = slot.kernel_calls;
    evidence->maximum_depth = slot.maximum_depth_seen;
    evidence->seh_registration = slot.seh_registration;
    evidence->tls = slot.tls;
    evidence->seh_verified = slot.seh_verified;
    evidence->tls_verified = slot.tls_verified;
    evidence->at_stop = slot.at_stop;
    evidence->trace_count = slot.trace_count;
    evidence->kernel_count = slot.kernel_count;
    evidence->seh_check_count = slot.seh_check_count;
    for (size_t i = 0; i < slot.seh_check_count && i < 16; ++i) evidence->seh_checks[i] = slot.seh_checks[i];
    return true;
}

const TraceRecord* TraceForRole(Role role, size_t index) {
    const WorkerSlot& slot = Slot(role);
    return index < slot.trace_count ? &slot.trace[index] : nullptr;
}

const KernelObservation* KernelObservationForRole(Role role, size_t index) {
    const WorkerSlot& slot = Slot(role);
    return index < slot.kernel_count ? &slot.kernel[index] : nullptr;
}

bool PendingKernelCall(uint32_t ordinal, uint32_t& call_pc, uint32_t& return_pc) {
    if (!t_pending_kernel || t_pending_kernel->ordinal != ordinal) return false;
    call_pc = t_pending_kernel->call_pc;
    return_pc = t_pending_kernel->return_pc;
    return true;
}

namespace {

void HarnessCreateThread(X86Context& ctx, uint8_t*) {
    uint32_t call_pc = 0, return_pc = 0;
    if (!PendingKernelCall(255, call_pc, return_pc) || Current() == nullptr) {
        Return(ctx, kInvalidParameter, 10);
        return;
    }
    std::array<uint32_t, 10> args{};
    if (mem::Read(ctx.esp + 4, args.data(), sizeof(args)) != mem::AccessStatus::kOk) {
        Return(ctx, kAccessViolation, 10);
        return;
    }
    AuthenticSystemThreadRequest request;
    request.handle_out = args[0];
    request.thread_extension_size = args[1];
    request.stack_size = args[2];
    request.tls_size = args[3];
    request.thread_id_out = args[4];
    request.start_routine = args[5];
    request.start_context = args[6];
    request.create_flags = args[7];
    request.reserved = args[8];
    request.system_routine = args[9];
    request.call_pc = call_pc;
    request.argument_count = 10;
    request.parent_id = Current()->id;
    request.dispatch_generation = dispatch::CallLease().Generation();
    request.memory_generation = mem::Generation();
    ThreadRef child;
    const uint32_t status = PrepareSystemThreadEx(request.handle_out, request.stack_size ? request.stack_size : 0x10000,
                                                  request.tls_size, request.thread_id_out, request.start_routine,
                                                  request.start_context, (request.create_flags & 0xFF) != 0,
                                                  request.system_routine, child);
    if (status != kSuccess || !child) {
        if (child) CancelPreparedThread(child);
        Return(ctx, status != kSuccess ? status : kNoMemory, 10);
        return;
    }
    const Status bound = BindChildThreadForRequest(request, child, call_pc);
    if (bound != Status::kReady) {
        CancelPreparedThread(child);
        Return(ctx, kInvalidParameter, 10);
        return;
    }
    if (!StartThread(child)) {
        CancelPreparedThread(child);
        Return(ctx, kNoMemory, 10);
        return;
    }
    Return(ctx, kSuccess, 10);
}

void HarnessClose(X86Context& ctx, uint8_t*) {
    uint32_t handle = 0;
    if (mem::Read(ctx.esp + 4, &handle, sizeof(handle)) != mem::AccessStatus::kOk) {
        Return(ctx, kAccessViolation, 1);
        return;
    }
    Return(ctx, CloseHandle(handle) ? kSuccess : kInvalidHandle, 1);
}

void HarnessCriticalEnter(X86Context& ctx, uint8_t*) {
    Thread* thread = Current();
    uint32_t object = 0;
    if (!thread || mem::Read(ctx.esp + 4, &object, sizeof(object)) != mem::AccessStatus::kOk) {
        Return(ctx, kAccessViolation, 1);
        return;
    }
    Return(ctx, CheckedCriticalEnter(object, 0x0046C8B0u, thread->ethread) == CriticalResult::kOk ? kSuccess
                                                                                            : kInvalidParameter,
           1);
}

void HarnessCriticalLeave(X86Context& ctx, uint8_t*) {
    Thread* thread = Current();
    uint32_t object = 0;
    if (!thread || mem::Read(ctx.esp + 4, &object, sizeof(object)) != mem::AccessStatus::kOk) {
        Return(ctx, kAccessViolation, 1);
        return;
    }
    Return(ctx, CheckedCriticalLeave(object, 0x0046C8B0u, thread->ethread) == CriticalResult::kOk ? kSuccess
                                                                                            : kInvalidParameter,
           1);
}

[[noreturn]] void RejectKernelObject(WorkerSlot& slot, const char* reason) {
    (void)reason;
    Violation(slot, Status::kKernelRejected, t_pending_kernel ? t_pending_kernel->call_pc : 0);
}

// The harness versions of ordinals 47/107/113 are thin adapters over the
// production checked implementations. They exist because the authentic
// experiment target links the harness, not the full xboxkrnl export table.
void HarnessShutdownRegistration(X86Context& ctx, uint8_t*) {
    uint32_t object = 0, action = 0;
    if (mem::Read(ctx.esp + 4, &object, sizeof(object)) != mem::AccessStatus::kOk ||
        mem::Read(ctx.esp + 8, &action, sizeof(action)) != mem::AccessStatus::kOk) {
        if (t_active) RejectKernelObject(*t_active, "shutdown registration arguments");
        Return(ctx, kAccessViolation, 2);
        return;
    }
    Thread* thread = Current();
    if (!t_active || !thread) {
        Return(ctx, kInvalidParameter, 2);
        return;
    }
    const KernelObjectStatus status = RegisterShutdownNotificationChecked(
        object, action, thread->id, t_active->image_sha1, kShutdownCallbackLow, kShutdownCallbackHigh);
    if (status != KernelObjectStatus::kOk) RejectKernelObject(*t_active, "shutdown registration");
    Return(ctx, 0, 2);
}

void HarnessInitializeDpc(X86Context& ctx, uint8_t*) {
    uint32_t arguments[3] = {};
    if (mem::Read(ctx.esp + 4, arguments, sizeof(arguments)) != mem::AccessStatus::kOk) {
        if (t_active) RejectKernelObject(*t_active, "initialise DPC arguments");
        Return(ctx, kAccessViolation, 3);
        return;
    }
    const KernelObjectStatus status =
        InitializeDpcChecked(arguments[0], arguments[1], arguments[2]);
    if (status != KernelObjectStatus::kOk) {
        if (t_active) RejectKernelObject(*t_active, "initialise DPC");
        Return(ctx, kInvalidParameter, 3);
        return;
    }
    Return(ctx, 0, 3);
}

void HarnessInitializeTimerEx(X86Context& ctx, uint8_t*) {
    uint32_t arguments[2] = {};
    if (mem::Read(ctx.esp + 4, arguments, sizeof(arguments)) != mem::AccessStatus::kOk) {
        if (t_active) RejectKernelObject(*t_active, "initialise timer arguments");
        Return(ctx, kAccessViolation, 2);
        return;
    }
    const KernelObjectStatus status = InitializeTimerExChecked(arguments[0], arguments[1]);
    if (status != KernelObjectStatus::kOk) {
        if (t_active) RejectKernelObject(*t_active, "initialise timer");
        Return(ctx, kInvalidParameter, 2);
        return;
    }
    Return(ctx, 0, 2);
}

}  // namespace

Status InstallHarnessKernelExports() {
    RegisterExport(255, kImageName255, HarnessCreateThread);
    RegisterExport(187, kImageName187, HarnessClose);
    RegisterExport(277, kImageName277, HarnessCriticalEnter);
    RegisterExport(294, kImageName294, HarnessCriticalLeave);
    RegisterExport(47, kImageName47, HarnessShutdownRegistration);
    RegisterExport(107, kImageName107, HarnessInitializeDpc);
    RegisterExport(113, kImageName113, HarnessInitializeTimerEx);
    if (!HasExport(255) || !HasExport(187) || !HasExport(277) || !HasExport(294) || !HasExport(47) ||
        !HasExport(107) || !HasExport(113))
        return Status::kInternalError;
    return Status::kReady;
}

#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS

Status AuthorizeSyntheticRouteForThread(Thread* thread, const RouteManifest& manifest, const char* image_sha1,
                                        uint64_t mapping_identity, uint64_t dispatch_generation,
                                        uint64_t memory_generation, uint32_t instruction_budget,
                                        uint32_t transfer_budget, uint32_t kernel_budget, uint32_t maximum_call_depth,
                                        RunAuthorization* authorization) {
    ResetSession();
    AuthorizationPolicy policy;
    policy.origin = Origin::kSyntheticFixture;
    policy.role = Role::kParent;
    policy.manifest = &manifest;
    policy.child_manifest = &manifest;
    policy.child_instruction_budget = 1;
    policy.child_transfer_budget = 1;
    policy.child_kernel_budget = 1;
    policy.child_maximum_call_depth = 1;
    policy.image_sha1 = image_sha1;
    policy.mapping_identity = mapping_identity;
    policy.dispatch_generation = dispatch_generation;
    policy.memory_generation = memory_generation;
    policy.instruction_budget = instruction_budget;
    policy.transfer_budget = transfer_budget;
    policy.kernel_budget = kernel_budget;
    policy.maximum_call_depth = maximum_call_depth;
    return AuthorizeRunForThread(thread, policy, authorization);
}

Status StartParentWorkerForTest(const ParentStartRequest& request, RunAuthorization* authorization) {
    ResetSession();
    if (!request.manifest || !request.child_manifest || !request.image_sha1 ||
        !ValidManifestPtr(request.manifest) || !ValidManifestPtr(request.child_manifest))
        return Status::kInvalidPolicy;
    const Status installed = InstallHarnessKernelExports();
    if (installed != Status::kReady) return installed;
    ThreadRef thread;
    const uint32_t prepared = PrepareSystemThreadEx(
        request.handle_out, request.stack_size ? request.stack_size : 0x10000, request.tls_size, request.id_out,
        request.manifest->entry, 0, false, 0, thread);
    if (prepared != kSuccess || !thread) {
        if (thread) CancelPreparedThread(thread);
        return Status::kInvalidThread;
    }
    AuthorizationPolicy policy;
    policy.origin = Origin::kSyntheticFixture;
    policy.role = Role::kParent;
    policy.manifest = request.manifest;
    policy.child_manifest = request.child_manifest;
    policy.image_sha1 = request.image_sha1;
    policy.mapping_identity = request.mapping_identity ? request.mapping_identity : dispatch::MappingIdentity();
    policy.dispatch_generation = request.dispatch_generation ? request.dispatch_generation
                                                             : dispatch::CallLease().Generation();
    policy.memory_generation = request.memory_generation ? request.memory_generation : mem::Generation();
    policy.instruction_budget = request.instruction_budget;
    policy.transfer_budget = request.transfer_budget;
    policy.kernel_budget = request.kernel_budget;
    policy.maximum_call_depth = request.maximum_call_depth;
    policy.child_instruction_budget = request.child_instruction_budget;
    policy.child_transfer_budget = request.child_transfer_budget;
    policy.child_kernel_budget = request.child_kernel_budget;
    policy.child_maximum_call_depth = request.child_maximum_call_depth;
    RunAuthorization token;
    const Status authorized = AuthorizeRunForThread(thread.get(), policy, &token);
    if (authorized != Status::kReady) {
        CancelPreparedThread(thread);
        return authorized;
    }
    if (!StartThread(thread)) {
        CancelPreparedThread(thread);
        return Status::kInvalidThread;
    }
    if (authorization) *authorization = std::move(token);
    if (request.prepared_thread) *request.prepared_thread = thread;
    return Status::kReady;
}

void SetRunCancelForTest(Role role) { Slot(role).cancelled = true; }

void SetInjectionHookForTest(Role role, uint32_t at_pc, InjectionHook hook) {
    std::lock_guard lock(g_injection_lock);
    for (Injection& injection : g_injections)
        if (!injection.hook) {
            injection = {role, at_pc, hook, false};
            return;
        }
}

void ClearInjectionHooksForTest() {
    std::lock_guard lock(g_injection_lock);
    for (Injection& injection : g_injections) injection = {};
}

#endif  // MOSTWANTED_AUTHENTIC_RUN_TESTS


namespace {

void RecordHarnessFault(Status status, uint32_t pc, uint32_t address) {
    if (!t_active || t_active->finished) return;
    WorkerSlot& slot = *t_active;
    slot.status = status;
    slot.stop_pc = pc;
    slot.stop_target = address;
    TraceEvent(slot, EventKind::kViolation, pc, address);
    TraceContext(slot);
    TraceEvent(slot, EventKind::kFinalize, pc, address);
    slot.finished = true;
    slot.completed = true;
    slot.claimed = false;
}

}  // namespace

}  // namespace kernel::authentic

// The harness replaces the aborting production fault/trap sinks so a checked
// memory violation or unsupported instruction becomes a stop report. This is
// only linked into the gated harness target; ordinary builds keep
// cpu/memory_fault.cpp and its aborting contract.
void x86_memory_fault(X86Context& ctx, const X86MemoryFault& fault) {
    (void)ctx;
    kernel::authentic::RecordHarnessFault(kernel::authentic::Status::kMemoryViolation, fault.instruction,
                                          fault.address);
    throw kernel::authentic::AuthenticRunViolation{};
}

void x86_checked_trap(X86Context& ctx, uint32_t va, const char* what) {
    (void)ctx;
    (void)what;
    kernel::authentic::RecordHarnessFault(kernel::authentic::Status::kUnsupportedInstruction, va, 0);
    throw kernel::authentic::AuthenticRunViolation{};
}
