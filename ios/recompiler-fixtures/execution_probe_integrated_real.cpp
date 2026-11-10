// Phase 2C-35 integrated real-XBE root and secondary-thread probe.
//
// One combined generated mapping carries both frozen routes.  The root route
// runs first and retains the ThreadRef created by the test-only ordinal-255
// service; the ordinal-187 service closes only the guest handle.  The same
// thread, stack, KPCR, ETHREAD and TLS then enter the secondary system routine.
// This file is implementation, preflight and synthetic-test only.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace integrated_probe {
bool ValidateMapping(std::string& error);
bool ValidateMappingTextFromStrings(const std::string& header, const std::string& source,
                                    const std::string& mapping, std::string& error);
}  // namespace integrated_probe

// The root probe's own translation unit is reused so both frozen routes share
// its checked state, hooks, import bridges and image preparation.  Its main is
// renamed; this file supplies the integrated entry point.
#define main root_probe_unused_main
#include "execution_probe.cpp"
#undef main

#include "execution_probe_integrated_secondary_route.h"
#include "kernel/guest_callback.h"

namespace {

namespace sec = execution_probe_integrated_secondary;

constexpr uint32_t kIntegratedRootInstructions = 72;
constexpr uint32_t kIntegratedSecondaryInstructions = 71;
constexpr uint32_t kIntegratedTotalInstructions = kIntegratedRootInstructions + kIntegratedSecondaryInstructions;
constexpr uint32_t kIntegratedDirectCalls = 4;
constexpr uint32_t kIntegratedKernelCalls = 4;
constexpr uint32_t kIntegratedImportReads = 4;

struct SecondaryFacts {
    uint32_t stack_base = 0, stack_top = 0, initial_esp = 0, entry_esp = 0;
    uint32_t stop_esp = 0, stop_ebp = 0, registration = 0;
    uint32_t kpcr = 0, ethread = 0, tls = 0, fs0 = 0;
    uint32_t tls_source = 0, tls_end = 0, tls_copy = 0, tls_zero = 0;
    uint32_t handle = 0, id = 0;
    uint32_t critical_object = 0, critical_head = 0, critical_lock = 0;
    uint32_t critical_recursion = 0, critical_owner = 0;
    uint32_t worker_baseline = 0;
    std::vector<uint32_t> snapshot_pages;
};

struct SecondaryState {
    bool running = false;
    bool in_step = false;
    bool first_step = true;
    bool have_previous = false;
    uint32_t previous_pc = 0;
    uint32_t executed = 0;
    uint32_t direct_calls = 0;
    uint32_t kernel_calls = 0;
    uint32_t import_reads = 0;
    uint32_t worker_baseline = 0;
    uint32_t instruction_budget = sec::kMaximumInstructions;
    uint32_t stack_base = 0;
    uint32_t stack_top = 0;
    uint32_t kpcr = 0;
    uint32_t ethread = 0;
    uint32_t tls = 0;
    std::chrono::steady_clock::time_point deadline{};
    bool deadline_set = false;
    std::vector<StepRecord> trace;
    StepRecord stop{};
    bool have_stop = false;
    bool stop_frame_clear = false;
};

SecondaryState g_secondary;
bool g_integrated_fail_secondary_step = false;

bool ValidateCriticalSectionInitial(const SecondaryFacts& facts, std::string& error);
bool VerifyCriticalSectionFinal(const SecondaryFacts& facts, std::string& error);
bool PrepareCriticalSectionBridge(const SecondaryFacts& facts, std::string& error);
void ReleaseCriticalSectionBridge();
bool PatchCriticalSectionImports(std::string& error);
bool RestoreCriticalSectionImports();

const sec::RouteStep* FindSecondaryStep(uint32_t pc) {
    for (const sec::RouteStep& step : sec::kRoute)
        if (step.pc == pc) return &step;
    return nullptr;
}

constexpr uint32_t kCriticalListBranch = 0x002B59A6;

enum class SecondaryBranchCondition : uint8_t { kUnreviewed, kZero, kNotZero };

SecondaryBranchCondition BranchConditionFor(uint32_t pc) {
    if (pc == sec::kTlsFillBranch) return SecondaryBranchCondition::kZero;
    if (pc == kCriticalListBranch) return SecondaryBranchCondition::kNotZero;
    return SecondaryBranchCondition::kUnreviewed;
}

struct ReachableInstruction {
    uint32_t pc;
    const char* text;
    bool secondary;
    char operation;
};

char GeneratedOperation(const char* text) {
    const std::string t(text);
    if (t.rfind("call [", 0) == 0) return 'i';
    if (t.rfind("call ", 0) == 0) return 'c';
    if (t.rfind("ret", 0) == 0) return 'r';
    if (t.rfind("rep movs", 0) == 0) return 's';
    if (t.rfind("rep stos", 0) == 0) return 'z';
    if (t.rfind("lea ", 0) == 0) return 0;
    if (t.rfind("push", 0) == 0) return 'w';
    if (t.rfind("pop", 0) == 0) return 'p';
    if (t.find('[') != std::string::npos) return 'm';
    return 0;
}

const execution_probe::RouteStep* FindRootStep(uint32_t pc) {
    for (const execution_probe::RouteStep& step : execution_probe::kRoute)
        if (step.pc == pc) return &step;
    return nullptr;
}

bool RootBranchTaken(uint32_t pc, bool& taken) {
    switch (pc) {
        case 0x002B392A: taken = true; return true;
        case 0x002B5CAF: taken = false; return true;
        case 0x002B5CEA: taken = true; return true;
        case 0x002B3973: taken = true; return true;
        case 0x002B2765: taken = false; return true;
        default: return false;
    }
}

bool SecondaryBranchTaken(uint32_t pc, bool& taken) {
    if (pc == sec::kTlsFillBranch || pc == kCriticalListBranch) {
        taken = false;
        return true;
    }
    return false;
}

bool CollectReachable(std::vector<ReachableInstruction>& out, std::string& error) {
    uint32_t pc = execution_probe::kEntry;
    size_t walked = 0;
    while (true) {
        const execution_probe::RouteStep* step = FindRootStep(pc);
        if (!step) { error = "root route has no reviewed step for a reachable PC"; return false; }
        if (step->kind == execution_probe::StepKind::kStop) {
            // the root RET executes and returns through the host sentinel frame
            const size_t index = size_t(step - execution_probe::kRoute);
            out.push_back({pc, execution_probe::kExpectedRouteText[index], false,
                           GeneratedOperation(execution_probe::kExpectedRouteText[index])});
            break;
        }
        if (step->kind == execution_probe::StepKind::kForbidden) {
            error = "reachable root walk entered a forbidden instruction";
            return false;
        }
        const size_t index = size_t(step - execution_probe::kRoute);
        out.push_back({pc, execution_probe::kExpectedRouteText[index], false,
                       GeneratedOperation(execution_probe::kExpectedRouteText[index])});
        if (step->kind == execution_probe::StepKind::kConditional) {
            bool taken = false;
            if (!RootBranchTaken(pc, taken)) { error = "root conditional outcome is not reviewed"; return false; }
            pc = taken ? step->alternate : step->next;
        } else {
            pc = step->next;
        }
        if (++walked > 4 * execution_probe::kRouteCount) { error = "root route walk did not terminate"; return false; }
    }
    if (out.size() != kIntegratedRootInstructions) {
        error = "reachable root instruction count is not the reviewed 72";
        return false;
    }
    const size_t root_count = out.size();
    pc = sec::kEntry;
    walked = 0;
    while (true) {
        const sec::RouteStep* step = FindSecondaryStep(pc);
        if (!step) { error = "secondary route has no reviewed step for a reachable PC"; return false; }
        if (step->kind == sec::StepKind::kStop) break;  // pre-instruction stop, never executed
        if (step->kind == sec::StepKind::kForbidden) {
            error = "reachable secondary walk entered a forbidden instruction";
            return false;
        }
        const size_t index = size_t(step - sec::kRoute);
        out.push_back(
            {pc, sec::kExpectedRouteText[index], true, GeneratedOperation(sec::kExpectedRouteText[index])});
        if (step->kind == sec::StepKind::kConditional) {
            bool taken = false;
            if (!SecondaryBranchTaken(pc, taken)) {
                error = "secondary conditional outcome is not reviewed";
                return false;
            }
            pc = taken ? step->alternate : step->next;
        } else {
            pc = step->next;
        }
        if (++walked > 4 * sec::kRouteCount) {
            error = "secondary route walk did not terminate";
            return false;
        }
    }
    if (out.size() - root_count != kIntegratedSecondaryInstructions) {
        error = "reachable secondary instruction count is not the reviewed 71";
        return false;
    }
    return true;
}

bool AuditReachableGenerated(const std::string& source, size_t& supported, std::string& error) {
    struct ExactBodyCheck {
        uint32_t pc;
        const char* needle;
        const char* name;
    };
    static constexpr ExactBodyCheck kExactBodyChecks[] = {
        {0x002B59AF, "ctx.edi = (uint32_t)(v)", "POP EDI"},
        {0x002B59B0, "ctx.esi = (uint32_t)(v)", "POP ESI"},
        {0x002B59B1, "ctx.ebx = (uint32_t)(v)", "POP EBX"},
        {0x002B59B2, "x86_checked_ret(ctx, 0x2B59B2u, 4)", "RET 0x04"},
    };
    std::vector<ReachableInstruction> reachable;
    if (!CollectReachable(reachable, error)) return false;
    auto has = [](const std::string& text, const char* needle) {
        return text.find(needle) != std::string::npos;
    };
    for (const ReachableInstruction& insn : reachable) {
        char marker[32], hook[64], detail[160];
        std::snprintf(marker, sizeof(marker), "// %08X ", insn.pc);
        std::snprintf(hook, sizeof(hook), "%sStep_0x%08X(ctx, base);", insn.secondary ? "Secondary" : "Probe",
                      insn.pc);
        const size_t at = source.find(marker);
        if (at == std::string::npos) {
            std::snprintf(detail, sizeof(detail), "reachable instruction %08X has no generated body", insn.pc);
            error = detail;
            return false;
        }
        if (!has(source, hook)) {
            std::snprintf(detail, sizeof(detail), "reachable instruction %08X has no generated hook", insn.pc);
            error = detail;
            return false;
        }
        const char* next_hook = insn.secondary ? "\n\tSecondaryStep_" : "\n\tProbeStep_";
        const size_t body_at = at + std::string(marker).size();
        size_t end = source.find(next_hook, body_at);
        const size_t function_end = source.find("\n\t} catch", body_at);
        if (function_end < end) end = function_end;
        const std::string body = source.substr(at, end == std::string::npos ? std::string::npos : end - at);
        // semantic traps (idiv #DE, decoded-code ends) are not unsupported-instruction fallbacks;
        // CodeGen::Unimpl always emits an "unimplemented:" message
        if (has(body, "unimplemented:") || has(body, "x86_trap(")) {
            std::snprintf(detail, sizeof(detail), "reachable instruction %08X is emitted as an unsupported trap",
                          insn.pc);
            error = detail;
            return false;
        }
        bool operation_ok = true;
        switch (insn.operation) {
            case 'c': operation_ok = has(body, "x86_checked_call"); break;
            case 'i': operation_ok = has(body, "x86_checked_indirect_call"); break;
            case 'r': operation_ok = has(body, "x86_checked_ret"); break;
            case 's': operation_ok = has(body, "x86_rep_movs_checked"); break;
            case 'z': operation_ok = has(body, "x86_rep_stos_checked"); break;
            case 'w': operation_ok = has(body, "x86_mem_write"); break;
            case 'p': operation_ok = has(body, "x86_mem_read"); break;
            case 'm': operation_ok = has(body, "x86_mem_read") || has(body, "x86_mem_write"); break;
            default: break;
        }
        if (!operation_ok) {
            std::snprintf(detail, sizeof(detail), "reachable instruction %08X is missing its checked operation",
                          insn.pc);
            error = detail;
            return false;
        }
        for (const ExactBodyCheck& check : kExactBodyChecks) {
            if (insn.pc != check.pc) continue;
            if (!has(body, check.needle)) {
                std::snprintf(detail, sizeof(detail), "reachable instruction %08X is not the reviewed %s",
                              insn.pc, check.name);
                error = detail;
                return false;
            }
        }
    }
    supported = reachable.size();
    return true;
}

struct SecondaryInStep {
    SecondaryInStep() { g_secondary.in_step = true; }
    ~SecondaryInStep() { g_secondary.in_step = false; }
};

void RecordSecondaryStep(uint32_t pc, X86Context& ctx) {
    StepRecord record{};
    record.pc = pc;
    record.eax = ctx.eax;
    record.ecx = ctx.ecx;
    record.edx = ctx.edx;
    record.ebx = ctx.ebx;
    record.esp = ctx.esp;
    record.ebp = ctx.ebp;
    record.esi = ctx.esi;
    record.edi = ctx.edi;
    record.cf = ctx.cf;
    record.zf = ctx.zf;
    record.sf = ctx.sf;
    record.of = ctx.of;
    record.pf = ctx.pf;
    record.af = ctx.af;
    // the successor hook records the edge actually taken; a conditional's
    // outcome is not its zf alone (jz and jnz read the same flag)
    g_secondary.trace.push_back(record);
}

enum class CriticalFailMode : uint8_t { kNone, kEnterFail, kLeaveFail, kBadCleanup, kBadReturn, kWrongOwner };

uint32_t g_critical_owner_ethread = 0;
uint32_t g_critical_actual_ethread = 0;
uint32_t g_critical_held_lock = 0;
uint32_t g_critical_held_recursion = 0;
uint32_t g_critical_held_owner = 0;
uint32_t g_critical_after_lock = 0;
uint32_t g_critical_after_recursion = 0;
uint32_t g_critical_after_owner = 0;
bool g_critical_held_seen = false;
uint32_t g_critical_enter_calls = 0;
uint32_t g_critical_leave_calls = 0;
uint32_t g_critical_enter_esp = 0;
uint32_t g_critical_leave_esp = 0;
uint32_t g_critical_enter_return = 0;
uint32_t g_critical_leave_return = 0;
uint32_t g_critical_enter_arg = 0;
uint32_t g_critical_leave_arg = 0;
CriticalFailMode g_critical_fail_mode = CriticalFailMode::kNone;
bool g_critical_restore_fail = false;

bool WalkCriticalListForTest(uint32_t head, uint32_t sentinel, uint32_t span_begin, uint32_t span_end,
                             uint32_t& target, uint32_t& nodes, std::string& error) {
    target = 0;
    nodes = 0;
    uint32_t visited[sec::kCriticalMaxListNodes]{};
    while (head != sentinel) {
        if (nodes >= sec::kCriticalMaxListNodes) {
            error = "critical-section list traversal budget exhausted";
            return false;
        }
        if ((head & 3) != 0 || head < span_begin || uint64_t(head) + 8 > span_end) {
            error = "critical-section list node is unaligned or outside the reviewed span";
            return false;
        }
        for (uint32_t i = 0; i < nodes; ++i)
            if (visited[i] == head) {
                error = "critical-section list cycle detected";
                return false;
            }
        visited[nodes] = head;
        uint32_t next = 0, callback = 0;
        if (mem::Read(head, &next, sizeof(next)) != mem::AccessStatus::kOk ||
            mem::Read(head + 8, &callback, sizeof(callback)) != mem::AccessStatus::kOk) {
            error = "critical-section list node is not readable";
            return false;
        }
        ++nodes;
        if (nodes == 1) target = callback;
        if (next == sentinel) {
            return true;
        }
        if ((next & 3) != 0 || next < span_begin || uint64_t(next) + 8 > span_end) {
            error = "critical-section list next pointer is unaligned or outside the reviewed span";
            return false;
        }
        head = next;
    }
    return true;
}

void ProbeCriticalEnter(X86Context& ctx, uint8_t*) {
    ++g_critical_enter_calls;
    g_critical_enter_esp = ctx.esp;
    g_critical_enter_return = kernel::R32(ctx.esp);
    uint32_t object = 0, lock = 0, recursion = 0, owner = 0;
    const bool arg_ok = mem::Read(ctx.esp + 4, &object, sizeof(object)) == mem::AccessStatus::kOk;
    g_critical_enter_arg = object;
    if (!arg_ok || object != sec::kCriticalSection) {
        kernel::Return(ctx, kernel::kInvalidParameter, 1);
        return;
    }
    const bool state_ok =
        mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalOwner, &owner, sizeof(owner)) == mem::AccessStatus::kOk;
    if (!state_ok || lock != 0xFFFFFFFF || recursion != 0 || owner != 0 ||
        g_critical_owner_ethread == 0 || g_critical_actual_ethread == 0 ||
        g_critical_owner_ethread != g_critical_actual_ethread ||
        g_critical_fail_mode == CriticalFailMode::kEnterFail) {
        kernel::Return(ctx, kernel::kInvalidParameter, 1);
        return;
    }
    if (g_critical_fail_mode == CriticalFailMode::kBadCleanup) {
        ctx.eax = kernel::kSuccess;
        ctx.esp += 4;
        return;
    }
    g_critical_held_seen = false;
    lock = 0;
    recursion = 1;
    owner = g_critical_actual_ethread;
    if (mem::Write(sec::kCriticalLockCount, &lock, sizeof(lock)) != mem::AccessStatus::kOk ||
        mem::Write(sec::kCriticalRecursion, &recursion, sizeof(recursion)) != mem::AccessStatus::kOk ||
        mem::Write(sec::kCriticalOwner, &owner, sizeof(owner)) != mem::AccessStatus::kOk) {
        kernel::Return(ctx, kernel::kAccessViolation, 1);
        return;
    }
    uint32_t held_lock = 1, held_recursion = 0, held_owner = 1;
    const bool held_ok =
        mem::Read(sec::kCriticalLockCount, &held_lock, sizeof(held_lock)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalRecursion, &held_recursion, sizeof(held_recursion)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalOwner, &held_owner, sizeof(held_owner)) == mem::AccessStatus::kOk;
    g_critical_held_lock = held_lock;
    g_critical_held_recursion = held_recursion;
    g_critical_held_owner = held_owner;
    g_critical_held_seen = held_ok && held_lock == 0 && held_recursion == 1 &&
                           held_owner == g_critical_actual_ethread;
    if (!g_critical_held_seen) {
        kernel::Return(ctx, kernel::kAccessViolation, 1);
        return;
    }
    kernel::Return(ctx, kernel::kSuccess, 1);
    if (g_critical_fail_mode == CriticalFailMode::kBadReturn) {
        const uint32_t invalid = 0xDEADBEEF;
        mem::Write(ctx.esp - 8, &invalid, sizeof(invalid));
    }
}

void ProbeCriticalLeave(X86Context& ctx, uint8_t*) {
    ++g_critical_leave_calls;
    g_critical_leave_esp = ctx.esp;
    g_critical_leave_return = kernel::R32(ctx.esp);
    uint32_t object = 0, lock = 0, recursion = 0, owner = 0;
    const bool arg_ok = mem::Read(ctx.esp + 4, &object, sizeof(object)) == mem::AccessStatus::kOk;
    g_critical_leave_arg = object;
    if (!arg_ok || object != sec::kCriticalSection) {
        kernel::Return(ctx, kernel::kInvalidParameter, 1);
        return;
    }
    const bool state_ok =
        mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalOwner, &owner, sizeof(owner)) == mem::AccessStatus::kOk;
    if (!state_ok || g_critical_actual_ethread == 0 || g_critical_enter_calls != 1 || lock != 0 ||
        recursion != 1 || owner != g_critical_actual_ethread || !g_critical_held_seen ||
        g_critical_held_lock != 0 || g_critical_held_recursion != 1 ||
        g_critical_held_owner != g_critical_actual_ethread ||
        g_critical_fail_mode == CriticalFailMode::kLeaveFail ||
        g_critical_fail_mode == CriticalFailMode::kWrongOwner) {
        kernel::Return(ctx, kernel::kInvalidParameter, 1);
        return;
    }
    if (g_critical_fail_mode == CriticalFailMode::kBadCleanup) {
        ctx.eax = kernel::kSuccess;
        ctx.esp += 4;
        return;
    }
    lock = 0xFFFFFFFF;
    recursion = 0;
    owner = 0;
    if (mem::Write(sec::kCriticalLockCount, &lock, sizeof(lock)) != mem::AccessStatus::kOk ||
        mem::Write(sec::kCriticalRecursion, &recursion, sizeof(recursion)) != mem::AccessStatus::kOk ||
        mem::Write(sec::kCriticalOwner, &owner, sizeof(owner)) != mem::AccessStatus::kOk) {
        kernel::Return(ctx, kernel::kAccessViolation, 1);
        return;
    }
    uint32_t after_lock = 1, after_recursion = 1, after_owner = 1;
    const bool after_ok =
        mem::Read(sec::kCriticalLockCount, &after_lock, sizeof(after_lock)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalRecursion, &after_recursion, sizeof(after_recursion)) == mem::AccessStatus::kOk &&
        mem::Read(sec::kCriticalOwner, &after_owner, sizeof(after_owner)) == mem::AccessStatus::kOk;
    g_critical_after_lock = after_lock;
    g_critical_after_recursion = after_recursion;
    g_critical_after_owner = after_owner;
    if (!after_ok || after_lock != 0xFFFFFFFF || after_recursion != 0 || after_owner != 0) {
        kernel::Return(ctx, kernel::kAccessViolation, 1);
        return;
    }
    kernel::Return(ctx, kernel::kSuccess, 1);
    if (g_critical_fail_mode == CriticalFailMode::kBadReturn) {
        const uint32_t invalid = 0xDEADBEEF;
        mem::Write(ctx.esp - 8, &invalid, sizeof(invalid));
    }
}

void SecondaryStep(uint32_t pc, X86Context& ctx) {
    if (!g_secondary.running) Fail("secondary instruction outside its segment", pc);
    if (g_secondary.in_step) Fail("re-entry during a secondary step", pc);
    if (g_integrated_fail_secondary_step) Fail("injected secondary execution failure", pc);
    SecondaryInStep guard;

    if (pc == sec::kStartRoutine) Fail("start routine entry is forbidden", pc);
    if (pc == sec::kStackHelper) Fail("stack helper entry is forbidden", pc);
    if (pc == sec::kSehHandler) Fail("exception handler entry is forbidden", pc);

    const sec::RouteStep* step = FindSecondaryStep(pc);
    if (!step) Fail("secondary PC is not in the reviewed route", pc);
    if (g_secondary.deadline_set && std::chrono::steady_clock::now() > g_secondary.deadline)
        Fail("secondary instruction deadline exceeded", pc);
    if (mem::CheckAccess(pc, step->length, mem::kExecute) != mem::AccessStatus::kOk)
        Fail("secondary code page is not executable", pc);

    if (g_secondary.first_step) {
        if (pc != sec::kEntry) Fail("first secondary instruction is not the reviewed entry", pc);
        g_secondary.first_step = false;
    } else {
        const sec::RouteStep* previous = FindSecondaryStep(g_secondary.previous_pc);
        const bool next_edge = previous && pc == previous->next;
        const bool alternate_edge = previous && previous->kind == sec::StepKind::kConditional &&
                                    pc == previous->alternate;
        if (!next_edge && !alternate_edge) Fail("unexpected secondary control transfer", pc);
        if (previous && previous->kind == sec::StepKind::kConditional) {
            if (g_secondary.trace.empty() || g_secondary.trace.back().pc != previous->pc)
                Fail("secondary conditional has no trace record", pc);
            const SecondaryBranchCondition condition = BranchConditionFor(previous->pc);
            if (condition == SecondaryBranchCondition::kUnreviewed)
                Fail("secondary conditional instruction is not reviewed", pc);
            const bool taken = alternate_edge;
            const bool flag_taken = condition == SecondaryBranchCondition::kZero
                                        ? g_secondary.trace.back().zf != 0
                                        : g_secondary.trace.back().zf == 0;
            if (flag_taken != taken) Fail("secondary branch outcome mismatch", pc);
            g_secondary.trace.back().branch_valid = true;
            g_secondary.trace.back().branch_taken = taken;
        }
    }

    if (step->kind == sec::StepKind::kForbidden)
        Fail("unreviewed dynamic list callback target", pc);

    if (step->kind == sec::StepKind::kCall) {
        if (g_secondary.direct_calls >= sec::kMaximumDirectCalls) Fail("secondary direct call budget exhausted", pc);
        const dispatch::ResolvedTarget target = dispatch::ResolveTarget(step->next);
        if (target.kind != dispatch::TargetKind::kGeneratedFunction || !target.function)
            Fail("secondary direct call target is not a reviewed generated function", pc);
        if (mem::CheckAccess(step->next, 1, mem::kExecute) != mem::AccessStatus::kOk)
            Fail("secondary direct call target page is not executable", pc);
        ++g_secondary.direct_calls;
    }

    if (step->kind == sec::StepKind::kKernelCall) {
        if (g_secondary.kernel_calls >= sec::kMaximumKernelCalls) Fail("secondary kernel call budget exhausted", pc);
        const bool enter = pc == sec::kCriticalEnterCall;
        const bool leave = pc == sec::kCriticalLeaveCall;
        if (!enter && !leave) Fail("secondary kernel call is not a reviewed import", pc);
        const uint32_t slot = enter ? sec::kCriticalEnterSlot : sec::kCriticalLeaveSlot;
        const uint32_t ordinal = enter ? sec::kCriticalEnterOrdinal : sec::kCriticalLeaveOrdinal;
        uint32_t target = 0;
        if (mem::Read(slot, &target, sizeof(target)) != mem::AccessStatus::kOk ||
            target != dispatch::KernelThunkAddress(ordinal))
            Fail("secondary critical-section import slot is not the reviewed thunk", pc);
        if (mem::CheckAccess(step->next, 1, mem::kExecute) != mem::AccessStatus::kOk)
            Fail("secondary kernel call return PC is not executable", pc);
        ++g_secondary.kernel_calls;
    }

    if (step->kind == sec::StepKind::kRet) {
        if (ctx.checked_call == nullptr) Fail("secondary checked RET has no active call frame", pc);
        if (ctx.checked_call->return_address != step->next)
            Fail("secondary checked RET frame does not expect the reviewed return", pc);
    }

    if (step->kind == sec::StepKind::kStop) {
        if (g_secondary.direct_calls != sec::kMaximumDirectCalls ||
            g_secondary.kernel_calls != sec::kMaximumKernelCalls)
            Fail("secondary stop reached without the reviewed calls", pc);
        g_secondary.import_reads = g_probe.import_reads;
        if (g_secondary.import_reads != sec::kMaximumImportReads)
            Fail("secondary stop reached without the reviewed import reads", pc);
        if (!kernel::Threads().empty()) Fail("a guest thread became active during the secondary segment", pc);
        if (kernel::ThreadWorkerLaunchCountForTest() != g_secondary.worker_baseline)
            Fail("a kernel worker was activated during the secondary segment", pc);
        g_secondary.stop.pc = pc;
        g_secondary.stop.eax = ctx.eax;
        g_secondary.stop.ecx = ctx.ecx;
        g_secondary.stop.edx = ctx.edx;
        g_secondary.stop.ebx = ctx.ebx;
        g_secondary.stop.esp = ctx.esp;
        g_secondary.stop.ebp = ctx.ebp;
        g_secondary.stop.esi = ctx.esi;
        g_secondary.stop.edi = ctx.edi;
        g_secondary.stop.cf = ctx.cf;
        g_secondary.stop.zf = ctx.zf;
        g_secondary.stop.sf = ctx.sf;
        g_secondary.stop.of = ctx.of;
        g_secondary.stop.pf = ctx.pf;
        g_secondary.stop.af = ctx.af;
        g_secondary.stop_frame_clear = ctx.checked_call == nullptr;
        g_secondary.have_stop = true;
        throw ProbeNormalStop{};
    }

    if (g_secondary.executed >= g_secondary.instruction_budget)
        Fail("secondary instruction budget exhausted", pc);
    RecordSecondaryStep(pc, ctx);
    ++g_secondary.executed;
    g_secondary.previous_pc = pc;
    g_secondary.have_previous = true;
}

}  // namespace

#define DEFINE_SECONDARY_STEP(pc, len, kind, next, alt) \
    void SecondaryStep_##pc(X86Context& ctx, uint8_t*) { SecondaryStep(pc, ctx); }
EXECUTION_PROBE_INTEGRATED_SECONDARY_ROUTE(DEFINE_SECONDARY_STEP)
#undef DEFINE_SECONDARY_STEP

void SecondaryStep_0x002B38A2(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B38A2, ctx); }
void SecondaryStep_0x002B58B8(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B58B8, ctx); }

namespace {

size_t Occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
    return count;
}

bool ValidateSecondaryRouteText(const xbe::Image& image, std::string& error) {
    static_assert(sec::kExpectedRouteTextCount == sec::kRouteCount);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC, ZYAN_FALSE);

    uint64_t hash = 0xcbf29ce484222325ULL;
    size_t route_bytes = 0;
    for (size_t i = 0; i < sec::kRouteCount; ++i) {
        const sec::RouteStep& step = sec::kRoute[i];
        const int64_t offset = image.Offset(step.pc);
        const size_t span = image.FileSpan(step.pc);
        if (offset < 0 || span == 0) {
            error = "secondary route PC is not file-backed";
            return false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction,
                                                 operands))) {
            error = "secondary route instruction did not decode";
            return false;
        }
        char text[256];
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, text,
                                        sizeof(text), step.pc, nullptr);
        if (std::strcmp(text, sec::kExpectedRouteText[i]) != 0 || instruction.length != step.length) {
            error = std::string("secondary route text/length mismatch at ") + std::to_string(step.pc);
            return false;
        }
        if (step.kind == sec::StepKind::kCall || step.kind == sec::StepKind::kConditional) {
            const uint32_t expected = step.kind == sec::StepKind::kCall ? step.next : step.alternate;
            if (operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                uint32_t(step.pc + instruction.length + operands[0].imm.value.s) != expected) {
                error = std::string("secondary route control-flow target mismatch at ") + std::to_string(step.pc);
                return false;
            }
        }
        if (step.kind == sec::StepKind::kRet) {
            if (instruction.mnemonic != ZYDIS_MNEMONIC_RET) {
                error = "secondary route RET is not a return";
                return false;
            }
            if (step.pc == 0x002B59B2) {
                if (instruction.operand_count_visible != 1 ||
                    operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || operands[0].imm.value.u != 4) {
                    error = "secondary helper RET is not RET 0x04";
                    return false;
                }
            } else if (instruction.operand_count_visible != 0) {
                error = "secondary route RET is not a plain return";
                return false;
            }
        }
        for (uint32_t j = 0; j < step.length; ++j) {
            hash ^= image.file[size_t(offset) + j];
            hash *= 0x100000001b3ULL;
        }
        route_bytes += step.length;
    }
    if (route_bytes != sec::kExpectedRouteBytes || hash != sec::kExpectedRouteHash) {
        error = "secondary route byte hash mismatch";
        return false;
    }
    for (size_t i = 0; i < sec::kTrapCount; ++i) {
        const uint32_t pc = sec::kTrapPcs[i];
        const int64_t offset = image.Offset(pc);
        const size_t span = image.FileSpan(pc);
        if (offset < 0 || span == 0) {
            error = "secondary trap boundary is not file-backed";
            return false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction,
                                                 operands))) {
            error = "secondary trap boundary did not decode";
            return false;
        }
        char text[256];
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, text,
                                        sizeof(text), pc, nullptr);
        if (std::strcmp(text, sec::kExpectedTrapText[i]) != 0 ||
            instruction.length != (pc == sec::kNextHelper ? 1 : 5)) {
            error = std::string("secondary trap boundary text/length mismatch at ") + std::to_string(pc);
            return false;
        }
        if (pc == sec::kStartRoutine &&
            (instruction.mnemonic != ZYDIS_MNEMONIC_CALL || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
             uint32_t(pc + instruction.length + operands[0].imm.value.s) != sec::kStackHelper)) {
            error = "start routine entry is not the reviewed call";
            return false;
        }
    }
    return true;
}

bool ReadGenerated(const char* name, std::string& text, std::string& error) {
    const std::string path = std::string(EXECUTION_PROBE_REAL_GENERATED_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "generated integrated mapping file is missing: " + path;
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(file), {});
    return true;
}

bool ValidateSecondaryContext(const kernel::ThreadRef& thread, const X86Context& ctx, std::string& error) {
    if (!thread || thread->state.load() != kernel::ThreadState::kPrepared ||
        thread->suspend.load() != 0 || !kernel::Threads().empty()) {
        error = "retained thread is not a fresh inactive thread";
        return false;
    }
    if (thread->system_routine != sec::kSystemRoutine || thread->start_routine != sec::kStartRoutine ||
        thread->start_context != sec::kStartContext) {
        error = "retained-thread routine arguments are not authentic";
        return false;
    }
    const uint32_t expected_esp = (thread->tls & ~15u) - 16;
    if (ctx.esp != expected_esp || ctx.esp % 16 != 0) {
        error = "secondary initial ESP does not match InitContext";
        return false;
    }
    if (ctx.fs_base != thread->kpcr) {
        error = "secondary FS base is not the retained KPCR";
        return false;
    }
    if (ctx.fcw != 0x27F || ctx.mxcsr != 0x1F80 || ctx.thread_handle != 0) {
        error = "secondary x87/SSE or thread-handle context is not the production value";
        return false;
    }
    const uint32_t stack_base = thread->stack_top - thread->stack_size;
    if (!thread->stack_top || !thread->stack_size ||
        ctx.esp < stack_base + sec::kCallbackFrameBytes + 0x40 || ctx.esp > thread->stack_top) {
        error = "secondary initial ESP is outside the retained stack";
        return false;
    }
    if (thread->tls < stack_base || thread->tls + sec::kTlsBlockSize > thread->stack_top) {
        error = "secondary TLS block is not inside the retained stack";
        return false;
    }
    if (kernel::R32(thread->kpcr + 0x00) != 0xFFFFFFFF) {
        error = "retained fs:[0] exception list is not the reviewed terminator";
        return false;
    }
    if (kernel::R32(thread->kpcr + 0x04) != thread->stack_top ||
        kernel::R32(thread->kpcr + 0x08) != stack_base ||
        kernel::R32(thread->kpcr + 0x28) != thread->ethread ||
        kernel::R32(thread->kpcr + 0x20) != thread->kpcr + 0x28) {
        error = "retained KPCR stack/ETHREAD fields are not the production values";
        return false;
    }
    if (kernel::R32(thread->ethread + 0x1C) != thread->stack_top ||
        kernel::R32(thread->ethread + 0x20) != stack_base ||
        kernel::R32(thread->ethread + 0x28) != thread->tls ||
        kernel::R32(thread->ethread + 0x12C) != thread->id) {
        error = "retained ETHREAD stack/TLS/id fields are not the production values";
        return false;
    }
    if (mem::CheckAccess(thread->kpcr, 4, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = "retained KPCR is not writable for the SEH registration";
        return false;
    }
    if (mem::CheckAccess(stack_base, mem::kPage, mem::kReadWrite) != mem::AccessStatus::kOk ||
        !mem::IsCommitted(thread->stack_top - 1)) {
        error = "retained stack is not committed";
        return false;
    }
    if (kernel::R32(thread->tls) != 0) {
        error = "retained TLS pointer slot is not the fresh zero value";
        return false;
    }
    if (thread->ctx != &ctx) {
        error = "retained thread does not point at the secondary context";
        return false;
    }
    return true;
}

bool InitializeSecondaryContext(const kernel::ThreadRef& thread, X86Context& ctx, SecondaryFacts& facts,
                                std::string& error) {
    if (!kernel::InitializePreparedThreadContextForTest(thread, ctx)) {
        error = "prepared-thread context initialization failed";
        return false;
    }
    if (!ValidateSecondaryContext(thread, ctx, error)) return false;
    facts.stack_base = thread->stack_top - thread->stack_size;
    facts.stack_top = thread->stack_top;
    facts.initial_esp = ctx.esp;
    facts.entry_esp = ctx.esp - sec::kCallbackFrameBytes;
    facts.stop_esp = facts.entry_esp - 0x38;
    facts.stop_ebp = facts.entry_esp - 4;
    facts.registration = facts.entry_esp - 0x14;
    facts.kpcr = thread->kpcr;
    facts.ethread = thread->ethread;
    facts.tls = thread->tls;
    facts.fs0 = kernel::R32(thread->kpcr);
    facts.handle = thread->published_handle;
    facts.id = thread->id;
    facts.worker_baseline = kernel::ThreadWorkerLaunchCountForTest();
    if (mem::Read(sec::kTlsGlobalSource, &facts.tls_source, sizeof(facts.tls_source)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kTlsGlobalEnd, &facts.tls_end, sizeof(facts.tls_end)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kTlsGlobalZeroFill, &facts.tls_zero, sizeof(facts.tls_zero)) != mem::AccessStatus::kOk) {
        error = "could not read the authentic TLS globals";
        return false;
    }
    if (facts.tls_end < facts.tls_source) {
        error = "authentic TLS source range is inverted";
        return false;
    }
    facts.tls_copy = facts.tls_end - facts.tls_source;
    if (facts.tls_copy != sec::kAuthenticCopyBytes || facts.tls_zero != sec::kAuthenticZeroFillBytes) {
        error = "authentic TLS globals are not copy 0 / zero-fill 0x90";
        return false;
    }
    if (uint64_t(sec::kTlsPointerBytes) + facts.tls_copy + facts.tls_zero > sec::kTlsBlockSize) {
        error = "authentic TLS copy and zero-fill exceed the TLS block";
        return false;
    }
    facts.snapshot_pages = {uint32_t((thread->stack_top - 1) & ~(mem::kPage - 1)),
                            thread->kpcr & ~(mem::kPage - 1),
                            kRealRdataPage,
                            kRealImageBase,
                            sec::kCriticalSection & ~(mem::kPage - 1)};
    if (!ValidateCriticalSectionInitial(facts, error)) return false;
    return true;
}

void ConfigureSecondaryMemoryPolicy(const SecondaryFacts& facts) {
    g_probe.memory.clear();
    g_probe.import_reads = 0;
    g_probe.import_reads_255 = 0;
    g_probe.import_reads_187 = 0;
    g_probe.options.allowed_spans = {
        {facts.kpcr, facts.kpcr + 4, true},
        {facts.kpcr + 0x28, facts.kpcr + 0x2C, false},
        {facts.ethread + 0x28, facts.ethread + 0x2C, false},
        {sec::kTlsGlobalSource, sec::kTlsGlobalSource + 4, false},
        {sec::kTlsGlobalEnd, sec::kTlsGlobalEnd + 4, false},
        {sec::kTlsGlobalZeroFill, sec::kTlsGlobalZeroFill + 4, false},
        {facts.tls, facts.tls + sec::kTlsBlockSize, true},
        {facts.stack_base, facts.stack_top, true},
        {sec::kCriticalSection, sec::kCriticalSection + 0x24, true},
    };
    g_probe.options.snapshot_pages = facts.snapshot_pages;
    g_probe.options.allow_kernel_call = true;
    g_probe.options.kernel_call_budget = sec::kMaximumKernelCalls;
    g_probe.options.allow_call_target = true;
    g_probe.options.direct_call_budget = sec::kMaximumDirectCalls;
    g_probe.options.instruction_budget = sec::kMaximumInstructions;
    g_probe.running = true;
}

bool ValidateRetainedThread(const RealFacts& root_facts, const kernel::ThreadRef& thread, bool require_closed,
                            std::string& error) {
    if (!thread) {
        error = "no retained ThreadRef";
        return false;
    }
    if (thread->state.load() != kernel::ThreadState::kPrepared) {
        error = "retained ThreadRef is not kPrepared";
        return false;
    }
    if (thread->suspend.load() != 0 || !kernel::Threads().empty()) {
        error = "retained ThreadRef is suspended or active";
        return false;
    }
    const uint32_t handle = thread->published_handle;
    if (!handle) {
        error = "retained ThreadRef has no published handle";
        return false;
    }
    if (g_kernel255_handle && handle != g_kernel255_handle) {
        error = "retained ThreadRef does not carry the root-published handle";
        return false;
    }
    if (thread->system_routine != root_facts.import_args[9] ||
        thread->start_routine != root_facts.import_args[5] ||
        thread->start_context != root_facts.import_args[6]) {
        error = "retained ThreadRef routines do not match the root arguments";
        return false;
    }
    uint32_t published = 0;
    if (mem::Read(root_facts.import_args[0], &published, sizeof(published)) != mem::AccessStatus::kOk ||
        published != handle) {
        error = "root handle output is not the retained thread's handle";
        return false;
    }
    const auto resolved = kernel::FromHandle(handle);
    if (require_closed && resolved) {
        error = "guest handle still resolves through FromHandle";
        return false;
    }
    if (!require_closed && resolved.get() != thread.get()) {
        error = "guest handle does not resolve to the retained ThreadRef";
        return false;
    }
    return true;
}

bool PrepareCandidateThread(const RealFacts& facts, kernel::ThreadRef& thread, std::string& error) {
    if (g_prepared_thread) {
        error = "a retained thread already exists; second preparation is forbidden";
        return false;
    }
    const uint32_t stack = facts.import_args[2] ? facts.import_args[2] : 0x10000;
    const uint32_t status = kernel::PrepareSystemThreadEx(
        facts.import_args[0], stack, facts.import_args[3], facts.import_args[4], facts.import_args[5],
        facts.import_args[6], (facts.import_args[7] & 0xFF) != 0, facts.import_args[9], thread);
    if (status != kernel::kSuccess || !thread) {
        error = "fresh preflight thread preparation failed";
        return false;
    }
    return true;
}

bool CancelCandidateAtOutputs(kernel::ThreadRef& thread, uint32_t handle_out, uint32_t id_out, std::string& error) {
    if (!thread) {
        error = "no candidate thread to cancel";
        return false;
    }
    thread->ctx = nullptr;
    const uint32_t handle = thread->published_handle;
    const uint32_t stack_top = thread->stack_top;
    const bool cancelled = kernel::CancelPreparedThread(thread);
    uint32_t cleared = 1, id_cleared = 1;
    const bool output_cleared = mem::Read(handle_out, &cleared, sizeof(cleared)) == mem::AccessStatus::kOk &&
                                cleared == 0;
    const bool id_ok = id_out == 0 ||
                       (mem::Read(id_out, &id_cleared, sizeof(id_cleared)) == mem::AccessStatus::kOk &&
                        id_cleared == 0);
    const bool stack_freed = !mem::IsCommitted(stack_top - 1);
    const bool ok = cancelled && output_cleared && id_ok && stack_freed && !kernel::FromHandle(handle) &&
                    thread->state.load() == kernel::ThreadState::kCancelled && thread->published_handle == 0 &&
                    kernel::Threads().empty();
    if (!ok) error = "candidate-thread cancellation did not roll back all resources";
    thread.reset();
    return ok;
}

bool VerifySecondarySeh(const SecondaryFacts& facts, std::string& error) {
    auto read = [](uint32_t address, const char* what, uint32_t& value, std::string& error) {
        if (mem::Read(address, &value, sizeof(value)) != mem::AccessStatus::kOk) {
            error = std::string("could not read ") + what;
            return false;
        }
        return true;
    };
    uint32_t exception_list = 0, next = 0, handler = 0, scope = 0, scope_flags = 0, saved_ebp = 0;
    uint32_t saved_esp = 0, saved_edi = 0, saved_esi = 0, saved_ebx = 0, local_eax = 0, local_edx = 0;
    uint32_t local_ebx = 0;
    const uint32_t S = facts.entry_esp;
    if (!read(facts.kpcr, "fs:[0]", exception_list, error) ||
        !read(S - 0x14, "SEH next", next, error) ||
        !read(S - 0x10, "SEH handler", handler, error) ||
        !read(S - 0x0C, "SEH scope table", scope, error) ||
        !read(S - 0x08, "SEH scope flags", scope_flags, error) ||
        !read(S - 0x04, "saved EBP", saved_ebp, error) ||
        !read(S - 0x1C, "saved ESP", saved_esp, error) ||
        !read(S - 0x38, "saved EDI", saved_edi, error) ||
        !read(S - 0x34, "saved ESI", saved_esi, error) ||
        !read(S - 0x30, "saved EBX", saved_ebx, error) ||
        !read(S - 0x24, "local EAX", local_eax, error) ||
        !read(S - 0x28, "local EDX", local_edx, error) ||
        !read(S - 0x2C, "local EBX", local_ebx, error))
        return false;
    if (exception_list != facts.registration) {
        error = "fs:[0] does not point at the secondary SEH registration";
        return false;
    }
    if (next != 0xFFFFFFFF || handler != sec::kSehHandler) {
        error = "secondary SEH registration record is not the reviewed pair";
        return false;
    }
    if (scope != sec::kScopeTable || scope_flags != 0) {
        error = "secondary SEH scope table or flag is not the reviewed value";
        return false;
    }
    if (saved_ebp != 0 || saved_edi != 0 || saved_esi != 0 || saved_ebx != 0) {
        error = "secondary SEH saved registers or EBP were corrupted";
        return false;
    }
    if (saved_esp != S - 0x38) {
        error = "secondary SEH saved ESP is not the post-prolog stack";
        return false;
    }
    if (local_eax != facts.ethread || local_edx != facts.tls + 4 || local_ebx > sec::kTlsBlockSize) {
        error = "secondary SEH local frame does not hold the reviewed TLS values";
        return false;
    }
    return true;
}

bool VerifySecondaryTls(const SecondaryFacts& facts, std::string& error) {
    uint32_t pointer = 0;
    if (mem::Read(facts.tls, &pointer, sizeof(pointer)) != mem::AccessStatus::kOk) {
        error = "could not read the secondary TLS pointer slot";
        return false;
    }
    if (pointer != facts.tls + sec::kTlsPointerBytes) {
        error = "secondary TLS pointer slot does not point at TLS+4";
        return false;
    }
    for (uint32_t i = 0; i < facts.tls_zero; ++i) {
        uint8_t value = 0;
        if (mem::Read(facts.tls + sec::kTlsPointerBytes + i, &value, sizeof(value)) != mem::AccessStatus::kOk) {
            error = "could not read the secondary TLS zero-fill destination";
            return false;
        }
        if (value != 0) {
            error = "secondary TLS zero-fill mismatch";
            return false;
        }
    }
    return true;
}

unsigned CountSecondaryOps(uint32_t pc, uint32_t address, bool write) {
    unsigned count = 0;
    for (const MemoryRecord& record : g_probe.memory)
        if (record.pc == pc && record.address == address && record.write == write && record.size == 4) ++count;
    return count;
}

bool VerifySecondaryMemoryOperations(const SecondaryFacts& facts, std::string& error) {
    if (CountSecondaryOps(0x00319809, facts.kpcr, false) != 1 ||
        CountSecondaryOps(0x00319838, facts.kpcr, true) != 1) {
        error = "secondary KPCR exception-list access counts are not one each";
        return false;
    }
    if (CountSecondaryOps(0x002B5C1F, facts.kpcr + 0x28, false) != 1 ||
        CountSecondaryOps(0x002B5C28, facts.ethread + 0x28, false) != 1) {
        error = "secondary KPCR/ETHREAD TLS-pointer read counts are not one";
        return false;
    }
    if (CountSecondaryOps(0x002B5C34, sec::kTlsGlobalEnd, false) != 1 ||
        CountSecondaryOps(0x002B5C3A, sec::kTlsGlobalSource, false) != 1 ||
        CountSecondaryOps(0x002B5C57, sec::kTlsGlobalZeroFill, false) != 1) {
        error = "secondary TLS-global read counts are not one";
        return false;
    }
    if (CountSecondaryOps(0x002B5C31, facts.tls, true) != 1) {
        error = "secondary TLS pointer publication count is not one";
        return false;
    }
    for (const MemoryRecord& record : g_probe.memory)
        if ((record.pc == 0x002B5C4E || record.pc == 0x002B5C55) && record.write) {
            error = "the authentic copy length zero still wrote TLS data";
            return false;
        }
    unsigned fill = 0;
    for (uint32_t offset = 0; offset < facts.tls_zero; offset += 4)
        fill += CountSecondaryOps(0x002B5C6B, facts.tls + sec::kTlsPointerBytes + offset, true);
    if (fill != facts.tls_zero / 4) {
        error = "secondary TLS zero-fill dword count is not 0x24";
        return false;
    }
    if (CountSecondaryOps(0x002B5C72, facts.tls + sec::kTlsPointerBytes + facts.tls_zero, true) != 0) {
        error = "the zero-remainder REP STOSB unexpectedly wrote";
        return false;
    }
    if (CountSecondaryOps(0x002B598C, sec::kCriticalListHead, false) != 1) {
        error = "critical-section list-head read count is not one";
        return false;
    }
    unsigned import_records = 0;
    for (const MemoryRecord& record : g_probe.memory) {
        if (record.address < 0x003C9000 || record.address >= 0x003CA000) continue;
        const bool enter = record.pc == sec::kCriticalEnterCall && record.address == sec::kCriticalEnterSlot;
        const bool leave = record.pc == sec::kCriticalLeaveCall && record.address == sec::kCriticalLeaveSlot;
        if ((!enter && !leave) || record.write || record.size != 4) {
            error = "an unreviewed import-page access was recorded in the secondary segment";
            return false;
        }
        ++import_records;
    }
    if (import_records != 2 || g_probe.import_reads != 2) {
        error = "critical-section import operand read counts are not one per slot";
        return false;
    }
    for (const MemoryRecord& record : g_probe.memory) {
        if (record.address >= 0x003C9000 && record.address < 0x003CA000) continue;
        const bool external =
            (record.address == facts.kpcr && (record.pc == 0x00319809 || record.pc == 0x00319838)) ||
            (record.address == facts.kpcr + 0x28 && record.pc == 0x002B5C1F) ||
            (record.address == facts.ethread + 0x28 && record.pc == 0x002B5C28) ||
            (record.address == sec::kTlsGlobalEnd && record.pc == 0x002B5C34) ||
            (record.address == sec::kTlsGlobalSource && record.pc == 0x002B5C3A) ||
            (record.address == sec::kTlsGlobalZeroFill && record.pc == 0x002B5C57) ||
            (record.address == sec::kCriticalListHead && record.pc == 0x002B598C);
        if (external) continue;
        if (record.address >= facts.tls &&
            record.address + record.size <= uint64_t(facts.tls) + sec::kTlsBlockSize)
            continue;
        if (record.address >= facts.stack_base && record.address + record.size <= uint64_t(facts.tls)) continue;
        error = "a secondary memory access escaped the stack/KPCR/ETHREAD/TLS spans";
        return false;
    }
    return true;
}

bool VerifySecondaryPostconditions(const SecondaryFacts& facts, std::string& error) {
    if (!g_secondary.have_stop || g_secondary.stop.pc != sec::kStopBefore) {
        error = "secondary mandatory stop is not 002B5C7B";
        return false;
    }
    if (g_secondary.executed != kIntegratedSecondaryInstructions ||
        g_secondary.trace.size() != kIntegratedSecondaryInstructions) {
        error = "expected 71 secondary instructions";
        return false;
    }
    if (g_secondary.trace.empty() || g_secondary.trace.back().pc != 0x002B59B2) {
        error = "the last secondary PC is not 002B59B2";
        return false;
    }
    if (g_secondary.direct_calls != 2 || g_secondary.kernel_calls != 2 || g_secondary.import_reads != 2) {
        error = "secondary call or import counts are not the reviewed set";
        return false;
    }
    if (!g_secondary.stop_frame_clear) {
        error = "the secondary checked call frame was not clear at the stop";
        return false;
    }
    if (g_secondary.stop.esp != facts.stop_esp || g_secondary.stop.ebp != facts.stop_ebp) {
        error = "secondary SEH prolog stack or EBP contract mismatch";
        return false;
    }
    if (g_secondary.stop.eax != 0 || g_secondary.stop.ecx != 0 || g_secondary.stop.edx != 0x90 ||
        g_secondary.stop.ebx != 0 || g_secondary.stop.esi != 0 ||
        g_secondary.stop.edi != facts.tls + sec::kTlsBlockSize) {
        error = "secondary post-TLS register state mismatch";
        return false;
    }
    if (g_secondary.stop.zf != 1) {
        error = "secondary stop flags are not the reviewed result";
        return false;
    }
    uint32_t pushed = 0;
    if (mem::Read(g_secondary.stop.esp, &pushed, sizeof(pushed)) != mem::AccessStatus::kOk || pushed != 0) {
        error = "the secondary stop stack slot is not the saved EDI value";
        return false;
    }
    const StepRecord* branch = nullptr;
    const StepRecord* list_branch = nullptr;
    for (const StepRecord& step : g_secondary.trace) {
        if (step.pc == sec::kTlsFillBranch) branch = &step;
        if (step.pc == 0x002B59A6) list_branch = &step;
    }
    if (!branch || !branch->branch_valid || branch->branch_taken) {
        error = "secondary TLS zero-fill branch is not the authentic not-taken path";
        return false;
    }
    if (!list_branch || !list_branch->branch_valid || list_branch->branch_taken) {
        error = "secondary critical-section list branch is not the empty-list path";
        return false;
    }
    if (!VerifySecondarySeh(facts, error)) return false;
    if (!VerifySecondaryTls(facts, error)) return false;
    if (!VerifyCriticalSectionFinal(facts, error)) return false;
    if (!VerifySecondaryMemoryOperations(facts, error)) return false;
    if (!kernel::Threads().empty() || kernel::ThreadWorkerLaunchCountForTest() != facts.worker_baseline) {
        error = "a guest thread or worker became active during the secondary segment";
        return false;
    }
    return true;
}

bool VerifySecondarySnapshot(const SecondaryFacts& facts, const std::vector<SnapshotPage>& before,
                             std::string& error) {
    for (const SnapshotPage& snapshot : before) {
        if (!snapshot.valid) {
            error = "a secondary snapshot page was not readable";
            return false;
        }
        std::array<uint8_t, mem::kPage> after{};
        if (mem::Read(snapshot.page, after.data(), after.size()) != mem::AccessStatus::kOk) {
            error = "a secondary snapshot page became unreadable";
            return false;
        }
        unsigned changes = 0;
        uint32_t changed_offset = 0, changed_value = 0;
        for (uint32_t offset = 0; offset + 4 <= mem::kPage; offset += 4) {
            uint32_t old_value = 0, new_value = 0;
            std::memcpy(&old_value, snapshot.bytes.data() + offset, 4);
            std::memcpy(&new_value, after.data() + offset, 4);
            if (old_value == new_value) continue;
            ++changes;
            changed_offset = offset;
            changed_value = new_value;
        }
        const bool is_stack = snapshot.page == uint32_t((facts.stack_top - 1) & ~(mem::kPage - 1));
        const bool is_kpcr = snapshot.page == (facts.kpcr & ~(mem::kPage - 1));
        if (is_stack) {
            if (!changes) {
                error = "the retained stack page did not record the secondary writes";
                return false;
            }
        } else if (is_kpcr) {
            if (changes != 1 || changed_offset != 0 || changed_value != facts.registration) {
                error = "the retained KPCR page changed outside fs:[0]";
                return false;
            }
        } else if (changes != 0) {
            error = "a read-only secondary snapshot page changed";
            return false;
        }
    }
    return true;
}

void PrintSecondaryFailure(const ProbeFailure& failure) {
    std::printf("RESULT fail reason=\"%s\" pc=%08X", failure.reason.c_str(), failure.pc);
    if (failure.has_address) std::printf(" address=%08X", failure.address);
    std::printf(" instructions=%u direct_calls=%u kernel_calls=%u import_reads=%u\n", g_secondary.executed,
                g_secondary.direct_calls, g_secondary.kernel_calls, g_secondary.import_reads);
}

bool RunSecondarySegment(const kernel::ThreadRef& thread, SecondaryFacts& facts, std::string& error) {
    X86Context context{};
    if (!InitializeSecondaryContext(thread, context, facts, error)) return false;
    if (!PrepareCriticalSectionBridge(facts, error)) return false;
    ConfigureSecondaryMemoryPolicy(facts);
    const std::vector<SnapshotPage> snapshot = Snapshot();
    kernel::GuestCallbackTicket ticket;
    if (kernel::PrepareGuestCallback(sec::kSystemRoutine, kernel::GuestCallbackClass::kSystemRoutine, ticket) !=
        kernel::GuestCallbackStatus::kReady) {
        error = "system-routine callback ticket was rejected";
        return false;
    }
    if (ticket.target != sec::kSystemRoutine || ticket.callback_class != kernel::GuestCallbackClass::kSystemRoutine ||
        ticket.dispatch_generation == 0) {
        error = "system-routine callback ticket does not describe the reviewed target";
        return false;
    }
    g_secondary = SecondaryState{};
    g_secondary.running = true;
    g_secondary.stack_base = facts.stack_base;
    g_secondary.stack_top = facts.stack_top;
    g_secondary.kpcr = facts.kpcr;
    g_secondary.ethread = facts.ethread;
    g_secondary.tls = facts.tls;
    g_secondary.worker_baseline = facts.worker_baseline;
    const std::array<uint32_t, 2> arguments{sec::kStartRoutine, sec::kStartContext};
    uint32_t result = 0;
    try {
        (void)kernel::CallGuestCallbackForTest(context, ticket, sec::kSystemRoutine, arguments, facts.stack_base,
                                               thread->stack_size, result);
        error = "callback returned without the mandatory secondary stop";
        g_secondary.running = false;
        return false;
    } catch (const ProbeNormalStop&) {
    } catch (const ProbeFailure& failure) {
        g_secondary.running = false;
        g_probe.running = false;
        PrintSecondaryFailure(failure);
        error = failure.reason;
        return false;
    } catch (const X86MemoryFault& fault) {
        g_secondary.running = false;
        g_probe.running = false;
        ProbeFailure failure;
        failure.reason = std::string("guest memory fault: ") + mem::StatusName(fault.status);
        failure.pc = fault.instruction;
        failure.address = fault.address;
        failure.has_address = true;
        PrintSecondaryFailure(failure);
        error = failure.reason;
        return false;
    }
    g_secondary.running = false;
    g_probe.running = false;
    if (!g_secondary.have_stop) {
        error = "secondary callback did not stop normally";
        return false;
    }
    for (const StepRecord& step : g_secondary.trace) {
        std::printf("STEP %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
                    "flags=%u%u%u%u%u%u",
                    step.pc, step.eax, step.ecx, step.edx, step.ebx, step.esp, step.ebp, step.esi, step.edi, step.cf,
                    step.zf, step.sf, step.of, step.pf, step.af);
        if (step.branch_valid) std::printf(" branch=%s", step.branch_taken ? "taken" : "not-taken");
        std::printf("\n");
    }
    const StepRecord& stop = g_secondary.stop;
    std::printf("STOP %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
                "flags=%u%u%u%u%u%u frame=%s\n",
                stop.pc, stop.eax, stop.ecx, stop.edx, stop.ebx, stop.esp, stop.ebp, stop.esi, stop.edi, stop.cf,
                stop.zf, stop.sf, stop.of, stop.pf, stop.af, g_secondary.stop_frame_clear ? "clear" : "active");
    PrintMemoryDiff(snapshot);
    if (!VerifySecondaryPostconditions(facts, error)) return false;
    if (!VerifySecondarySnapshot(facts, snapshot, error)) return false;
    std::printf("ABI critical-section: enter_esp=%08X leave_esp=%08X enter_return=%08X leave_return=%08X arg=%08X\n",
                g_critical_enter_esp, g_critical_leave_esp, g_critical_enter_return, g_critical_leave_return,
                g_critical_enter_arg);
    std::printf("CRITICAL owner=%08X held=%08X/%u/%08X released=%08X/%u/%08X\n", g_critical_actual_ethread,
                g_critical_held_lock, g_critical_held_recursion, g_critical_held_owner, g_critical_after_lock,
                g_critical_after_recursion, g_critical_after_owner);
    return true;
}

bool VerifyImportRestoration(const RealFacts& facts, std::string& error) {
    uint32_t first = 0, second = 0;
    if (mem::Read(kImportSlot, &first, sizeof(first)) != mem::AccessStatus::kOk ||
        mem::Read(kCloseImportSlot, &second, sizeof(second)) != mem::AccessStatus::kOk) {
        error = "could not read the restored import slots";
        return false;
    }
    if (first != facts.import_authentic || second != facts.close_import_authentic) {
        error = "an import slot was not restored to its authentic value";
        return false;
    }
    if (mem::CheckAccess(kRealImportPage, 1, mem::kWrite) == mem::AccessStatus::kOk ||
        mem::CheckAccess(kRealImportPage, 1, mem::kExecute) == mem::AccessStatus::kOk) {
        error = "the import page is writable or executable after restoration";
        return false;
    }
    return true;
}

RealImportPatch g_critical_imports[] = {
    {sec::kCriticalEnterSlot, 0, 0, sec::kCriticalEnterOrdinal, sec::kCriticalEnterEncoding, false},
    {sec::kCriticalLeaveSlot, 0, 0, sec::kCriticalLeaveOrdinal, sec::kCriticalLeaveEncoding, false},
};

bool PatchCriticalSectionImports(std::string& error) {
    if (!ProtectRange(kRealImportPage, mem::kPage, mem::kRead | mem::kWrite)) {
        error = "could not open the import page for critical-section patches";
        return false;
    }
    bool ok = true;
    for (RealImportPatch& patch : g_critical_imports) {
        uint32_t authentic = 0;
        if (mem::Read(patch.slot, &authentic, sizeof(authentic)) != mem::AccessStatus::kOk ||
            authentic != patch.encoding) {
            error = patch.ordinal == sec::kCriticalEnterOrdinal
                        ? "enter-critical-section slot is not the authentic ordinal-277 encoding"
                        : "leave-critical-section slot is not the authentic ordinal-294 encoding";
            ok = false;
            break;
        }
        const uint32_t target = dispatch::KernelThunkAddress(patch.ordinal);
        if (!target || mem::Write(patch.slot, &target, sizeof(target)) != mem::AccessStatus::kOk) {
            error = "could not patch a critical-section import slot";
            ok = false;
            break;
        }
        patch.original = authentic;
        patch.target = target;
        patch.active = true;
    }
    const bool read_only = ProtectRange(kRealImportPage, mem::kPage, mem::kRead);
    if (!read_only || !ok) {
        if (error.empty()) error = "could not protect the import page read-only";
        return false;
    }
    for (const RealImportPatch& patch : g_critical_imports) {
        uint32_t readback = 0;
        if (!patch.active || mem::Read(patch.slot, &readback, sizeof(readback)) != mem::AccessStatus::kOk ||
            readback != patch.target) {
            error = "critical-section import patch did not read back";
            return false;
        }
    }
    return true;
}

bool RestoreCriticalSectionImports() {
    bool active = false;
    for (const RealImportPatch& patch : g_critical_imports) active |= patch.active;
    if (!active) return true;
    bool wrote = ProtectRange(kRealImportPage, mem::kPage, mem::kRead | mem::kWrite);
    for (RealImportPatch& patch : g_critical_imports) {
        if (!patch.active) continue;
        if (wrote) wrote = mem::Write(patch.slot, &patch.original, sizeof(patch.original)) == mem::AccessStatus::kOk;
        if (wrote && g_critical_restore_fail) {
            const uint32_t wrong = patch.original ^ 0xFFFFFFFFu;
            mem::Write(patch.slot, &wrong, sizeof(wrong));
        }
    }
    const bool read_only = ProtectRange(kRealImportPage, mem::kPage, mem::kRead);
    bool ok = wrote && read_only;
    for (RealImportPatch& patch : g_critical_imports) {
        if (!patch.active) continue;
        patch.active = false;
        uint32_t restored = 0;
        const bool readback =
            mem::Read(patch.slot, &restored, sizeof(restored)) == mem::AccessStatus::kOk && restored == patch.original;
        ok = ok && readback;
        std::printf("IMPORT critical slot=%08X restored=%08X ok=%d\n", patch.slot, restored, int(readback));
    }
    std::printf("IMPORT critical restored=%d read_only=%d\n", int(ok), int(read_only));
    return ok;
}

bool PrepareCriticalSectionBridge(const SecondaryFacts& facts, std::string& error) {
    if (!facts.ethread || !facts.kpcr || kernel::R32(facts.kpcr + 0x28) != facts.ethread) {
        error = "critical-section owner ETHREAD is zero or does not match the retained KPCR";
        return false;
    }
    kernel::RegisterExport(sec::kCriticalEnterOrdinal, "RtlEnterCriticalSection", ProbeCriticalEnter);
    kernel::RegisterExport(sec::kCriticalLeaveOrdinal, "RtlLeaveCriticalSection", ProbeCriticalLeave);
    if (!dispatch::SetKernel277FixtureBridge(true) || !dispatch::SetKernel294FixtureBridge(true)) {
        dispatch::SetKernel277FixtureBridge(false);
        dispatch::SetKernel294FixtureBridge(false);
        error = "critical-section test bridges were not authorized";
        return false;
    }
    g_critical_owner_ethread = facts.ethread;
    g_critical_actual_ethread = facts.ethread;
    g_critical_held_lock = 0;
    g_critical_held_recursion = 0;
    g_critical_held_owner = 0;
    g_critical_after_lock = 0;
    g_critical_after_recursion = 0;
    g_critical_after_owner = 0;
    g_critical_held_seen = false;
    g_critical_enter_calls = 0;
    g_critical_leave_calls = 0;
    g_critical_enter_esp = 0;
    g_critical_leave_esp = 0;
    g_critical_enter_return = 0;
    g_critical_leave_return = 0;
    g_critical_enter_arg = 0;
    g_critical_leave_arg = 0;
    g_critical_fail_mode = CriticalFailMode::kNone;
    return true;
}

void ReleaseCriticalSectionBridge() {
    dispatch::SetKernel277FixtureBridge(false);
    dispatch::SetKernel294FixtureBridge(false);
}

bool ValidateCriticalSectionInitial(const SecondaryFacts& facts, std::string& error) {
    uint32_t header = 0, lock = 0, recursion = 0, owner = 0, head = 0, prev = 0;
    if (mem::Read(sec::kCriticalSection, &header, sizeof(header)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalOwner, &owner, sizeof(owner)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalListHead, &head, sizeof(head)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalListHead + 4, &prev, sizeof(prev)) != mem::AccessStatus::kOk) {
        error = "could not read the reviewed critical-section object";
        return false;
    }
    if (header != 0x00040001 || lock != 0xFFFFFFFF || recursion != 0 || owner != 0) {
        error = "critical-section object is not in the authentic initialized state";
        return false;
    }
    if (head != sec::kCriticalListSentinel || prev != sec::kCriticalListSentinel) {
        error = "critical-section list is not empty";
        return false;
    }
    if (mem::CheckAccess(sec::kCriticalSection, 0x24, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = "critical-section object is not writable";
        return false;
    }
    (void)facts;
    return true;
}

bool VerifyCriticalSectionFinal(const SecondaryFacts& facts, std::string& error) {
    uint32_t header = 0, lock = 0, recursion = 0, owner = 0, head = 0, prev = 0;
    if (mem::Read(sec::kCriticalSection, &header, sizeof(header)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalOwner, &owner, sizeof(owner)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalListHead, &head, sizeof(head)) != mem::AccessStatus::kOk ||
        mem::Read(sec::kCriticalListHead + 4, &prev, sizeof(prev)) != mem::AccessStatus::kOk) {
        error = "could not read the final critical-section state";
        return false;
    }
    if (header != 0x00040001 || lock != 0xFFFFFFFF || recursion != 0 || owner != 0) {
        error = "critical-section object did not return to its initialized state";
        return false;
    }
    if (head != sec::kCriticalListSentinel || prev != sec::kCriticalListSentinel) {
        error = "critical-section list changed on the empty path";
        return false;
    }
    if (g_critical_enter_calls != 1 || g_critical_leave_calls != 1) {
        error = "critical-section enter/leave counts are not one each";
        return false;
    }
    if (g_critical_actual_ethread == 0 || g_critical_actual_ethread != facts.ethread ||
        g_critical_owner_ethread != g_critical_actual_ethread ||
        kernel::R32(facts.kpcr + 0x28) != g_critical_actual_ethread) {
        error = "critical-section owner does not match the retained thread ETHREAD";
        return false;
    }
    if (!g_critical_held_seen || g_critical_held_lock != 0 || g_critical_held_recursion != 1 ||
        g_critical_held_owner != g_critical_actual_ethread || g_critical_after_lock != 0xFFFFFFFF ||
        g_critical_after_recursion != 0 || g_critical_after_owner != 0) {
        error = "critical-section held/released owner state is not the reviewed transition";
        return false;
    }
    if (g_critical_enter_esp != facts.entry_esp - 0x54 || g_critical_enter_return != sec::kCriticalEnterReturn ||
        g_critical_enter_arg != sec::kCriticalSection ||
        // enter's stdcall cleanup removes its argument; the helper's 002B59A8
        // push ebx restores the same pre-call level before the leave call
        g_critical_leave_esp != facts.entry_esp - 0x54 || g_critical_leave_return != sec::kCriticalLeaveReturn ||
        g_critical_leave_arg != sec::kCriticalSection) {
        char detail[384];
        std::snprintf(detail, sizeof(detail),
                      "critical-section ABI mismatch: enter esp=%08X expected=%08X return=%08X expected=%08X "
                      "arg=%08X expected=%08X; leave esp=%08X expected=%08X return=%08X expected=%08X "
                      "arg=%08X expected=%08X",
                      g_critical_enter_esp, facts.entry_esp - 0x54, g_critical_enter_return,
                      sec::kCriticalEnterReturn, g_critical_enter_arg, sec::kCriticalSection, g_critical_leave_esp,
                      facts.entry_esp - 0x54, g_critical_leave_return, sec::kCriticalLeaveReturn, g_critical_leave_arg,
                      sec::kCriticalSection);
        error = detail;
        return false;
    }
    return true;
}

bool CleanupIntegrated(Options& options) {
    bool thread_ok = kernel::Threads().empty();
    if (g_prepared_thread) {
        const kernel::ThreadRef thread = g_prepared_thread;
        g_prepared_thread.reset();
        thread->ctx = nullptr;
        const uint32_t handle = thread->published_handle;
        const uint32_t handle_out = thread->handle_out;
        const uint32_t id_out = thread->id_out;
        const uint32_t stack_top = thread->stack_top;
        const bool cancelled = kernel::CancelPreparedThread(thread);
        uint32_t cleared_handle = 1, cleared_id = 1;
        const bool handle_output_ok =
            handle_out == 0 ||
            (mem::Read(handle_out, &cleared_handle, sizeof(cleared_handle)) == mem::AccessStatus::kOk &&
             cleared_handle == 0);
        const bool id_output_ok =
            id_out == 0 ||
            (mem::Read(id_out, &cleared_id, sizeof(cleared_id)) == mem::AccessStatus::kOk && cleared_id == 0);
        const bool handle_closed = handle == 0 || !kernel::FromHandle(handle);
        const bool stack_freed = stack_top == 0 || !mem::IsCommitted(stack_top - 1);
        const bool state_ok = thread->state.load() == kernel::ThreadState::kCancelled &&
                              thread->published_handle == 0;
        const bool no_registration = kernel::Threads().empty();
        thread_ok = thread_ok && cancelled && handle_output_ok && id_output_ok && handle_closed && stack_freed &&
                    state_ok && no_registration;
        std::printf("CLEANUP thread=%p cancelled=%d handle_closed=%d outputs=%d stack_freed=%d state=%u "
                    "registered=%zu\n",
                    static_cast<const void*>(thread.get()), int(cancelled), int(handle_closed),
                    int(handle_output_ok && id_output_ok), int(stack_freed), unsigned(thread->state.load()),
                    kernel::Threads().size());
    }
    const bool imports_restored = RestoreImportSlots();
    const bool critical_restored = RestoreCriticalSectionImports();
    ReleaseCriticalSectionBridge();
    const bool teardown_ok = TeardownReal(options);
    const bool ok = thread_ok && imports_restored && critical_restored && teardown_ok;
    if (!ok) std::printf("RESULT fail reason=\"integrated cleanup did not complete\"\n");
    return ok;
}

void PrintIntegratedFacts(const RealFacts& root_facts, const SecondaryFacts& secondary) {
    std::printf("PREFLIGHT identity: base=%08X image=%X header=%X sections=%u\n", root_facts.base,
                root_facts.image_size, root_facts.header_size, root_facts.sections);
    std::printf("PREFLIGHT root route: %zu/%zu instructions, bytes=217 hash=6FBF924DD2C56F08 stop=%08X\n",
                execution_probe::kRouteCount, execution_probe::kRouteCount, execution_probe::kStopBefore);
    std::printf("PREFLIGHT root import: slot=%08X authentic=%08X close=%08X authentic=%08X\n",
                kImportSlot, root_facts.import_authentic, kCloseImportSlot, root_facts.close_import_authentic);
    std::printf("PREFLIGHT root stack: base=%08X top=%08X esp=%08X entry_branch=%s helper_branch=%s\n",
                root_facts.stack_base, root_facts.stack_top, root_facts.esp,
                root_facts.entry_taken ? "taken" : "not-taken", "not-taken");
    std::printf("PREFLIGHT integrated: root=%u secondary=%u total=%u direct_calls=%u kernel_calls=%u imports=%u\n",
                kIntegratedRootInstructions, kIntegratedSecondaryInstructions, kIntegratedTotalInstructions,
                kIntegratedDirectCalls, kIntegratedKernelCalls, kIntegratedImportReads);
    std::printf("PREFLIGHT combined mapping: functions=11 continuations=0 hooks=%zu\n",
                size_t(execution_probe::kRouteCount) + sec::kRouteCount + sec::kTrapCount);
    std::printf("PREFLIGHT handoff: handle_out=%08X handle=%08X state=kPrepared routines=%08X/%08X/%08X "
                "close_slot=%08X\n",
                root_facts.import_args[0], secondary.handle, sec::kSystemRoutine, sec::kStartRoutine,
                sec::kStartContext, kCloseImportSlot);
    std::printf("PREFLIGHT secondary stack: base=%08X top=%08X initial_esp=%08X entry_esp=%08X stop_esp=%08X "
                "stop_ebp=%08X\n",
                secondary.stack_base, secondary.stack_top, secondary.initial_esp, secondary.entry_esp,
                secondary.stop_esp, secondary.stop_ebp);
    std::printf("PREFLIGHT secondary context: kpcr=%08X ethread=%08X tls=%08X fs0=%08X registration=%08X\n",
                secondary.kpcr, secondary.ethread, secondary.tls, secondary.fs0, secondary.registration);
    std::printf("PREFLIGHT critical owner: ethread=%08X kpcr_ethread=%08X held=0/1/ethread released=FFFFFFFF/0/0\n",
                secondary.ethread, kernel::R32(secondary.kpcr + 0x28));
    std::printf("PREFLIGHT secondary tls: source=%08X end=%08X copy=%u zero=%u\n", secondary.tls_source,
                secondary.tls_end, secondary.tls_copy, secondary.tls_zero);
    std::printf("PREFLIGHT critical: object=%08X list=%08X lock=%08X recursion=%u owner=%08X "
                "enter=%08X leave=%08X\n",
                sec::kCriticalSection, sec::kCriticalListSentinel, 0xFFFFFFFFu, 0u, 0u, sec::kCriticalEnterSlot,
                sec::kCriticalLeaveSlot);
    std::printf("PREFLIGHT cleanup: one retained ThreadRef, one cancellation, four restored imports, zero workers, "
                "zero resources\n");
    std::printf("PREFLIGHT PASS\n");
}

bool DriveSecondaryBranch(uint32_t predecessor, uint32_t conditional, uint32_t successor, bool zf,
                          uint32_t& failing_pc, std::string& failure) {
    g_secondary = SecondaryState{};
    g_secondary.running = true;
    g_secondary.first_step = false;
    g_secondary.have_previous = true;
    g_secondary.previous_pc = predecessor;
    X86Context conditional_ctx{};
    conditional_ctx.zf = zf ? 1u : 0u;
    X86Context successor_ctx{};
    successor_ctx.zf = conditional_ctx.zf;
    bool accepted = true;
    try {
        SecondaryStep(conditional, conditional_ctx);
        SecondaryStep(successor, successor_ctx);
    } catch (const ProbeFailure& thrown) {
        failing_pc = thrown.pc;
        failure = thrown.reason;
        accepted = false;
    }
    g_secondary.running = false;
    return accepted;
}

struct CriticalSectionAbiDrive {
    bool enter_ok = false, leave_ok = false, pops_ok = false;
    uint32_t enter_esp = 0, leave_esp = 0;
    uint32_t enter_return = 0, leave_return = 0;
    uint32_t enter_arg = 0, leave_arg = 0;
    uint32_t final_esp = 0;
    uint32_t restored_ebx = 0, restored_esi = 0, restored_edi = 0;
};

bool DriveCriticalSectionHelperAbi(const SecondaryFacts& facts, uint32_t enter_return, uint32_t enter_arg,
                                   uint32_t leave_return, uint32_t leave_arg, int32_t leave_delta,
                                   CriticalSectionAbiDrive& out, std::string& error) {
    const uint32_t helper_entry = facts.entry_esp - 0x40;
    const uint32_t saved_ebx = 0x11112222, saved_esi = 0x33334444, saved_edi = 0x55556666;
    const uint32_t helper_return = sec::kStopBefore, helper_arg = 1;
    const uint32_t enter_esp = helper_entry - 0x14;
    const uint32_t leave_esp = uint32_t(int32_t(enter_esp) + leave_delta);
    auto write = [](uint32_t address, uint32_t value) {
        return mem::Write(address, &value, sizeof(value)) == mem::AccessStatus::kOk;
    };
    if (!write(helper_entry - 4, saved_ebx) || !write(helper_entry - 8, saved_esi) ||
        !write(helper_entry - 12, saved_edi) || !write(helper_entry, helper_return) ||
        !write(helper_entry + 4, helper_arg) || !write(enter_esp, enter_return) ||
        !write(enter_esp + 4, enter_arg)) {
        error = "could not write the reviewed helper ABI stack";
        return false;
    }
    X86Context ctx{};
    ctx.esp = enter_esp;
    ctx.ebx = 0xAAAA0001;
    ctx.esi = 0xAAAA0002;
    ctx.edi = 0xAAAA0003;
    ProbeCriticalEnter(ctx, nullptr);
    out.enter_esp = g_critical_enter_esp;
    out.enter_return = g_critical_enter_return;
    out.enter_arg = g_critical_enter_arg;
    out.enter_ok = ctx.eax == kernel::kSuccess && ctx.esp == helper_entry - 0x0C && ctx.ebx == 0xAAAA0001 &&
                   ctx.esi == 0xAAAA0002 && ctx.edi == 0xAAAA0003;
    // the authentic enter cleanup returns to the pre-call level; the helper's
    // 002B59A8 push ebx then writes the leave frame at the same address
    if (!write(leave_esp, leave_return) || !write(leave_esp + 4, leave_arg)) {
        error = "could not write the reviewed helper leave frame";
        return false;
    }
    ctx.esp = leave_esp;
    ProbeCriticalLeave(ctx, nullptr);
    out.leave_esp = g_critical_leave_esp;
    out.leave_return = g_critical_leave_return;
    out.leave_arg = g_critical_leave_arg;
    out.leave_ok = ctx.eax == kernel::kSuccess && ctx.esp == leave_esp + 8 && ctx.ebx == 0xAAAA0001 &&
                   ctx.esi == 0xAAAA0002 && ctx.edi == 0xAAAA0003;
    if (leave_delta != 0) return true;
    const uint32_t saved_at = ctx.esp;
    uint32_t return_address = 0;
    if (mem::Read(saved_at, &out.restored_edi, sizeof(out.restored_edi)) != mem::AccessStatus::kOk ||
        mem::Read(saved_at + 4, &out.restored_esi, sizeof(out.restored_esi)) != mem::AccessStatus::kOk ||
        mem::Read(saved_at + 8, &out.restored_ebx, sizeof(out.restored_ebx)) != mem::AccessStatus::kOk ||
        mem::Read(saved_at + 12, &return_address, sizeof(return_address)) != mem::AccessStatus::kOk) {
        error = "could not read the reviewed helper saved registers";
        return false;
    }
    out.final_esp = helper_entry + 8;
    out.pops_ok = out.restored_edi == saved_edi && out.restored_esi == saved_esi && out.restored_ebx == saved_ebx &&
                  return_address == sec::kStopBefore && out.final_esp == facts.stop_esp;
    return true;
}

int RunIntegratedPreflight(const std::string& scenario) {
    alarm(12);
    if (!mem::Init()) {
        std::printf("SETUP fail memory init\n");
        return 4;
    }
    if (!kernel::RestartKernelWorkers()) {
        std::printf("SETUP fail callback gate\n");
        mem::Shutdown();
        return 4;
    }

    std::string error;
    xbe::Image image;
    Options options;
    RealFacts facts;
    bool ok = false;
    int exit_code = 5;

    auto finish = [&](const char* failure, bool expected) {
        if (failure) std::printf("RESULT fail reason=\"%s\"\n", failure);
        const bool teardown_ok = CleanupIntegrated(options);
        if (!teardown_ok) return 5;
        return failure ? (expected ? 3 : 5) : 0;
    };

    if (scenario == "preflight") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!ValidateSecondaryRouteText(image, error)) return finish(error.c_str(), false);
        std::string generated;
        size_t supported = 0;
        if (!ReadGenerated("x86_recomp.0.cpp", generated, error) ||
            !AuditReachableGenerated(generated, supported, error))
            return finish(error.c_str(), false);
        std::printf("PREFLIGHT reachable generated instructions: %zu/%zu supported\n", supported, supported);
        std::printf("PREFLIGHT reachable coverage: root=%u secondary=%u stop=%08X forbidden_arms=excluded\n",
                    kIntegratedRootInstructions, kIntegratedSecondaryInstructions, sec::kStopBefore);
        kernel::ThreadRef candidate;
        if (!PrepareCandidateThread(facts, candidate, error)) return finish(error.c_str(), false);
        X86Context context{};
        SecondaryFacts secondary;
        if (!InitializeSecondaryContext(candidate, context, secondary, error)) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish(error.c_str(), false);
        }
        if (!ValidateRetainedThread(facts, candidate, false, error)) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish(error.c_str(), false);
        }
        if (!secondary.ethread || candidate->ethread != secondary.ethread || !secondary.kpcr ||
            kernel::R32(secondary.kpcr + 0x28) != secondary.ethread) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish("critical-section owner ETHREAD does not match the retained KPCR", false);
        }
        if (!PatchCriticalSectionImports(error)) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish(error.c_str(), false);
        }
        if (!RestoreCriticalSectionImports()) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish("critical-section import restoration failed", false);
        }
        PrintIntegratedFacts(facts, secondary);
        candidate->ctx = nullptr;
        if (!CancelCandidateAtOutputs(candidate, facts.import_args[0], facts.import_args[4], error))
            return finish(error.c_str(), false);
        return finish(nullptr, false);
    }
    if (scenario == "owner-prepared-thread" || scenario == "owner-zero-rejected" ||
        scenario == "owner-other-thread-rejected" || scenario == "owner-bad-kpcr") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        kernel::ThreadRef candidate;
        if (!PrepareCandidateThread(facts, candidate, error)) return finish(error.c_str(), false);
        X86Context context{};
        SecondaryFacts secondary;
        if (!InitializeSecondaryContext(candidate, context, secondary, error)) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish(error.c_str(), false);
        }
        const uint32_t ethread = candidate->ethread;
        bool valid = ethread != 0 && secondary.ethread == ethread && secondary.kpcr != 0 &&
                     kernel::R32(secondary.kpcr + 0x28) == ethread;
        std::string reason;
        if (scenario == "owner-prepared-thread") {
            const uint32_t page = 0x60010000;
            const uint32_t return_address = 0x11111111, arg = sec::kCriticalSection;
            bool drove = mem::Commit(page, mem::kPage);
            if (!drove) reason = "could not commit the critical-section owner test page";
            if (drove) {
                mem::Write(page, &return_address, sizeof(return_address));
                mem::Write(page + 4, &arg, sizeof(arg));
                mem::Write(page + 0x10, &return_address, sizeof(return_address));
                mem::Write(page + 0x14, &arg, sizeof(arg));
                drove = PrepareCriticalSectionBridge(secondary, reason);
                if (drove) {
                    uint32_t lock = 1, recursion = 0, owner = 1;
                    X86Context ctx{};
                    ctx.esp = page;
                    ProbeCriticalEnter(ctx, nullptr);
                    mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock));
                    mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion));
                    mem::Read(sec::kCriticalOwner, &owner, sizeof(owner));
                    drove = ctx.eax == kernel::kSuccess && ctx.esp == page + 8 && lock == 0 && recursion == 1 &&
                            owner == ethread && g_critical_held_seen && g_critical_held_lock == 0 &&
                            g_critical_held_recursion == 1 && g_critical_held_owner == ethread;
                    ctx.esp = page + 0x10;
                    ProbeCriticalLeave(ctx, nullptr);
                    mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock));
                    mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion));
                    mem::Read(sec::kCriticalOwner, &owner, sizeof(owner));
                    drove = drove && ctx.eax == kernel::kSuccess && ctx.esp == page + 0x18 &&
                            lock == 0xFFFFFFFF && recursion == 0 && owner == 0 &&
                            g_critical_after_lock == 0xFFFFFFFF && g_critical_after_recursion == 0 &&
                            g_critical_after_owner == 0;
                }
                ReleaseCriticalSectionBridge();
                mem::Decommit(page, mem::kPage);
            }
            valid = valid && drove;
        } else if (scenario == "owner-zero-rejected") {
            SecondaryFacts zero = secondary;
            zero.ethread = 0;
            valid = !PrepareCriticalSectionBridge(zero, error) &&
                    error.find("zero or does not match") != std::string::npos;
        } else if (scenario == "owner-other-thread-rejected") {
            SecondaryFacts other = secondary;
            other.ethread = ethread ^ 0x400;
            error.clear();
            valid = !PrepareCriticalSectionBridge(other, error) &&
                    error.find("zero or does not match") != std::string::npos;
        } else {
            const uint32_t mismatch = ethread ^ 0x400;
            mem::Write(secondary.kpcr + 0x28, &mismatch, sizeof(mismatch));
            error.clear();
            valid = !PrepareCriticalSectionBridge(secondary, error) &&
                    error.find("zero or does not match") != std::string::npos;
            mem::Write(secondary.kpcr + 0x28, &ethread, sizeof(ethread));
        }
        candidate->ctx = nullptr;
        valid = kernel::CancelPreparedThread(candidate) && valid && kernel::Threads().empty();
        candidate.reset();
        if (!valid) {
            if (!reason.empty()) error = reason;
            else if (error.empty()) error = "critical-section owner contract mismatch";
            return finish(error.c_str(), false);
        }
        std::printf("RESULT ok %s ethread=%08X kpcr=%08X held=%08X/%u/%08X after=%08X/%u/%08X\n",
                    scenario.c_str(), ethread, secondary.kpcr, g_critical_held_lock, g_critical_held_recursion,
                    g_critical_held_owner, g_critical_after_lock, g_critical_after_recursion,
                    g_critical_after_owner);
        return finish(nullptr, false);
    }
    if (scenario == "abi-correct" || scenario == "abi-enter-return-mismatch" ||
        scenario == "abi-enter-arg-mismatch" || scenario == "abi-leave-return-mismatch" ||
        scenario == "abi-leave-arg-mismatch" || scenario == "abi-leave-esp-mismatch") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        const bool cleanup_case = scenario == "abi-leave-esp-mismatch";
        kernel::ThreadRef candidate;
        if (cleanup_case) {
            if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        } else if (!PrepareCandidateThread(facts, candidate, error)) {
            return finish(error.c_str(), false);
        }
        const kernel::ThreadRef thread = cleanup_case ? g_prepared_thread : candidate;
        if (cleanup_case && !PatchCriticalSectionImports(error)) {
            thread->ctx = nullptr;
            CleanupIntegrated(options);
            return finish(error.c_str(), false);
        }
        X86Context context{};
        SecondaryFacts secondary;
        if (!InitializeSecondaryContext(thread, context, secondary, error)) {
            thread->ctx = nullptr;
            if (cleanup_case) CleanupIntegrated(options);
            else {
                kernel::CancelPreparedThread(thread);
                candidate.reset();
            }
            return finish(error.c_str(), false);
        }
        if (!PrepareCriticalSectionBridge(secondary, error)) {
            thread->ctx = nullptr;
            if (cleanup_case) CleanupIntegrated(options);
            else {
                kernel::CancelPreparedThread(thread);
                candidate.reset();
            }
            return finish(error.c_str(), false);
        }
        uint32_t enter_return = sec::kCriticalEnterReturn, enter_arg = sec::kCriticalSection;
        uint32_t leave_return = sec::kCriticalLeaveReturn, leave_arg = sec::kCriticalSection;
        int32_t leave_delta = 0;
        bool expect_enter_ok = true, expect_leave_ok = true;
        if (scenario == "abi-enter-return-mismatch") {
            enter_return = 0xDEADBEEF;
        } else if (scenario == "abi-enter-arg-mismatch") {
            enter_arg = sec::kCriticalListHead;
            expect_enter_ok = false;
            expect_leave_ok = false;
        } else if (scenario == "abi-leave-return-mismatch") {
            leave_return = 0xDEADBEEF;
        } else if (scenario == "abi-leave-arg-mismatch") {
            leave_arg = sec::kCriticalListHead;
            expect_leave_ok = false;
        } else if (scenario == "abi-leave-esp-mismatch") {
            leave_delta = -4;
        }
        CriticalSectionAbiDrive drive;
        std::string drive_error;
        const bool driven = DriveCriticalSectionHelperAbi(secondary, enter_return, enter_arg, leave_return, leave_arg,
                                                          leave_delta, drive, drive_error);
        bool accepted = false;
        if (driven) accepted = VerifyCriticalSectionFinal(secondary, error);
        else error = drive_error;
        bool valid = driven && drive.enter_ok == expect_enter_ok && drive.leave_ok == expect_leave_ok;
        if (scenario == "abi-correct") {
            valid = valid && drive.pops_ok && accepted && drive.enter_esp == secondary.entry_esp - 0x54 &&
                    drive.leave_esp == secondary.entry_esp - 0x54 && drive.enter_esp == drive.leave_esp &&
                    drive.enter_return == sec::kCriticalEnterReturn &&
                    drive.leave_return == sec::kCriticalLeaveReturn && drive.enter_arg == sec::kCriticalSection &&
                    drive.leave_arg == sec::kCriticalSection && drive.final_esp == secondary.stop_esp;
        } else {
            valid = valid && !accepted;
            if (drive.enter_ok && drive.leave_ok && drive.pops_ok)
                valid = valid && drive.enter_esp == secondary.entry_esp - 0x54 &&
                        drive.leave_esp == secondary.entry_esp - 0x54 && drive.final_esp == secondary.stop_esp;
        }
        ReleaseCriticalSectionBridge();
        thread->ctx = nullptr;
        if (cleanup_case) {
            valid = valid && CleanupIntegrated(options);
        } else {
            valid = valid && kernel::CancelPreparedThread(thread) && kernel::Threads().empty();
            candidate.reset();
        }
        if (!valid) {
            if (error.empty()) error = "critical-section ABI regression mismatch";
            return finish(error.c_str(), false);
        }
        if (scenario == "abi-correct")
            std::printf("RESULT ok abi-correct enter_esp=%08X leave_esp=%08X enter_return=%08X leave_return=%08X "
                        "arg=%08X final_esp=%08X owner=%08X\n",
                        drive.enter_esp, drive.leave_esp, drive.enter_return, drive.leave_return, drive.enter_arg,
                        drive.final_esp, g_critical_actual_ethread);
        else
            std::printf("RESULT ok %s rejected reason=\"%s\"\n", scenario.c_str(), error.c_str());
        return finish(nullptr, false);
    }
    if (scenario == "branch-jz-not-taken" || scenario == "branch-jz-taken" ||
        scenario == "branch-jnz-not-taken" || scenario == "branch-jnz-taken" ||
        scenario == "branch-bad-successor" || scenario == "branch-flag-mismatch" ||
        scenario == "branch-jz-flag-mismatch") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        uint32_t predecessor = 0x002B59A4, conditional = kCriticalListBranch, successor = 0x002B59A8;
        bool zf = true, expect_accept = true, expect_taken = false;
        const char* expect_reason = nullptr;
        if (scenario == "branch-jz-not-taken") {
            predecessor = 0x002B5C5D;
            conditional = sec::kTlsFillBranch;
            successor = 0x002B5C61;
            zf = false;
        } else if (scenario == "branch-jz-taken") {
            predecessor = 0x002B5C5D;
            conditional = sec::kTlsFillBranch;
            successor = 0x002B5C74;
            zf = true;
            expect_taken = true;
        } else if (scenario == "branch-jnz-taken") {
            successor = 0x002B5999;
            zf = false;
            expect_taken = true;
        } else if (scenario == "branch-bad-successor") {
            successor = 0x002B5986;
            expect_accept = false;
            expect_reason = "unexpected secondary control transfer";
        } else if (scenario == "branch-flag-mismatch") {
            successor = 0x002B5999;
            expect_accept = false;
            expect_reason = "secondary branch outcome mismatch";
        } else if (scenario == "branch-jz-flag-mismatch") {
            predecessor = 0x002B5C5D;
            conditional = sec::kTlsFillBranch;
            successor = 0x002B5C74;
            zf = false;
            expect_accept = false;
            expect_reason = "secondary branch outcome mismatch";
        }
        uint32_t failing_pc = 0;
        std::string failure;
        const bool accepted = DriveSecondaryBranch(predecessor, conditional, successor, zf, failing_pc, failure);
        const size_t expected_records = expect_accept ? 2u : 1u;
        const bool trace_ok = g_secondary.trace.size() == expected_records &&
                              g_secondary.executed == expected_records &&
                              g_secondary.trace.front().pc == conditional &&
                              g_secondary.trace.front().branch_valid == expect_accept &&
                              g_secondary.trace.front().branch_taken == (expect_accept && expect_taken);
        const bool rejection_ok =
            expect_accept || (failing_pc == successor && expect_reason &&
                              failure.find(expect_reason) != std::string::npos);
        if (accepted != expect_accept || !trace_ok || !rejection_ok) {
            std::printf("RESULT fail %s accepted=%d trace_ok=%d failing_pc=%08X reason=\"%s\"\n",
                        scenario.c_str(), int(accepted), int(trace_ok), failing_pc, failure.c_str());
            return finish("conditional branch observation contract mismatch", false);
        }
        if (expect_accept)
            std::printf("RESULT ok %s branch_valid=1 branch_taken=%d\n", scenario.c_str(), int(expect_taken));
        else
            std::printf("RESULT ok %s rejected pc=%08X reason=\"%s\"\n", scenario.c_str(), failing_pc,
                        failure.c_str());
        return finish(nullptr, false);
    }
    if (scenario == "branch-failure-cleanup") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        if (!PatchCriticalSectionImports(error)) return finish(error.c_str(), false);
        uint32_t failing_pc = 0;
        std::string failure;
        const bool accepted = DriveSecondaryBranch(0x002B59A4, kCriticalListBranch, 0x002B5999, true,
                                                   failing_pc, failure);
        const bool branch_rejected = !accepted && failing_pc == 0x002B5999 &&
                                     failure.find("secondary branch outcome mismatch") != std::string::npos &&
                                     g_secondary.trace.size() == 1 && g_secondary.executed == 1;
        if (g_prepared_thread) g_prepared_thread->ctx = nullptr;
        const bool cleanup_ok = CleanupIntegrated(options);
        if (!branch_rejected || !cleanup_ok) return finish("branch failure containment mismatch", false);
        std::printf("RESULT ok branch-failure-cleanup rejected pc=%08X reason=\"%s\" imports=restored "
                    "thread=released workers=0\n",
                    failing_pc, failure.c_str());
        return 0;
    }
    if (scenario == "generated-audit-unsupported" || scenario == "generated-audit-missing") {
        std::string source;
        if (!ReadGenerated("x86_recomp.0.cpp", source, error)) return finish(error.c_str(), false);
        const std::string marker = "// 002B59B1 pop ebx";
        const size_t at = source.find(marker);
        if (at == std::string::npos) return finish("reviewed POP EBX body was not found", false);
        if (scenario == "generated-audit-unsupported") {
            source.insert(at + marker.size(),
                          "\n\tx86_checked_trap(ctx, 0x2B59B1, \"unimplemented: injected\");");
        } else {
            source.erase(at, marker.size());
        }
        size_t supported = 0;
        if (AuditReachableGenerated(source, supported, error))
            return finish("injected generated-code defect was accepted", false);
        std::printf("RESULT ok %s %s\n", scenario.c_str(), error.c_str());
        return finish(nullptr, false);
    }
    if (scenario == "identity-missing") {
        if (ValidateRealIdentity("/tmp/mostwanted-integrated-does-not-exist.xbe", image, error))
            return finish("missing real XBE was accepted", false);
        return finish("real XBE is missing", true);
    }
    if (scenario == "identity-mismatch") {
        const std::string path = TempRealCopy("integrated-identity");
        const bool copied = CopyAndPatch(EXECUTION_PROBE_REAL_XBE, path, 0x100, 0xFF);
        const bool accepted = copied && ValidateRealIdentity(path, image, error);
        std::remove(path.c_str());
        if (accepted || !copied) return finish("patched identity was accepted", false);
        return finish("real XBE SHA-1 mismatch", true);
    }
    if (scenario == "root-route-mismatch" || scenario == "secondary-route-mismatch") {
        const std::string path = TempRealCopy(scenario.c_str());
        bool copied = ValidateRealIdentity(EXECUTION_PROBE_REAL_XBE, image, error);
        const int64_t offset =
            copied ? image.Offset(scenario == "root-route-mismatch" ? kEntry : sec::kEntry) : -1;
        copied = copied && CopyAndPatch(EXECUTION_PROBE_REAL_XBE, path, offset, 0x01);
        xbe::Image patched;
        std::vector<uint8_t> bytes;
        std::string parse_error;
        const bool parsed = copied && ReadFile(path.c_str(), bytes) && patched.Parse(std::move(bytes), parse_error);
        bool accepted = false;
        if (parsed) {
            if (scenario == "root-route-mismatch") accepted = ValidateRealRouteText(patched, parse_error);
            else accepted = ValidateSecondaryRouteText(patched, parse_error);
        }
        std::remove(path.c_str());
        if (accepted || !parsed) return finish("patched route was accepted", false);
        return finish(parse_error.c_str(), true);
    }
    if (scenario == "root-hook-missing" || scenario == "secondary-hook-missing") {
        std::string header, source, mapping;
        if (!ReadGenerated("x86_recomp_shared.h", header, error) ||
            !ReadGenerated("x86_recomp.0.cpp", source, error) ||
            !ReadGenerated("x86_func_mapping.cpp", mapping, error))
            return finish(error.c_str(), false);
        const std::string needle = scenario == "root-hook-missing" ? "ProbeStep_0x002B3916(ctx, base);"
                                                                  : "SecondaryStep_0x002B5C0F(ctx, base);";
        const size_t at = source.find(needle);
        if (at == std::string::npos) return finish("reviewed hook was not found", false);
        source.erase(at, needle.size());
        const bool accepted = integrated_probe::ValidateMappingTextFromStrings(header, source, mapping, error);
        if (accepted) return finish("hook removal was accepted", false);
        return finish(error.c_str(), true);
    }
    if (scenario == "root-failure-no-thread" || scenario == "root-failure-after-prep") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (scenario == "root-failure-after-prep") {
            if (!PrepareCandidateThread(facts, g_prepared_thread, error))
                return finish(error.c_str(), false);
            X86Context context{};
            if (!kernel::InitializePreparedThreadContextForTest(g_prepared_thread, context))
                return finish("could not initialize the retained context", false);
            g_prepared_thread->ctx = nullptr;
        }
        return finish(scenario == "root-failure-no-thread" ? "root failure before thread preparation"
                                                           : "root failure after thread preparation",
                      true);
    }
    if (scenario == "handoff-no-thread" || scenario == "handoff-active-thread" ||
        scenario == "handoff-stale-handle") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        kernel::ThreadRef candidate;
        if (scenario != "handoff-no-thread") {
            if (!PrepareCandidateThread(facts, candidate, error)) return finish(error.c_str(), false);
        }
        if (scenario == "handoff-active-thread") candidate->state = kernel::ThreadState::kActive;
        const bool valid = ValidateRetainedThread(facts, candidate, scenario == "handoff-stale-handle", error);
        if (scenario == "handoff-active-thread") candidate->state = kernel::ThreadState::kPrepared;
        if (candidate) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
        }
        if (valid) return finish("corrupted handoff was accepted", false);
        return finish(error.c_str(), true);
    }
    if (scenario == "handoff-second-thread") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        kernel::ThreadRef candidate;
        if (!PrepareCandidateThread(facts, candidate, error)) return finish(error.c_str(), false);
        g_prepared_thread = candidate;
        kernel::ThreadRef second;
        const bool refused = !PrepareCandidateThread(facts, second, error) && !second;
        g_prepared_thread.reset();
        candidate->ctx = nullptr;
        kernel::CancelPreparedThread(candidate);
        if (!refused) return finish("a second preparation was accepted", false);
        return finish("second thread preparation was refused", true);
    }
    if (scenario == "handoff-bad-fs" || scenario == "handoff-bad-kpcr" || scenario == "handoff-bad-ethread" ||
        scenario == "handoff-bad-tls") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        kernel::ThreadRef candidate;
        if (!PrepareCandidateThread(facts, candidate, error)) return finish(error.c_str(), false);
        X86Context context{};
        SecondaryFacts secondary;
        if (!InitializeSecondaryContext(candidate, context, secondary, error)) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish(error.c_str(), false);
        }
        if (scenario == "handoff-bad-fs") {
            context.fs_base = 0x12345000;
        } else if (scenario == "handoff-bad-kpcr") {
            const uint32_t bad = 0x12345678;
            mem::Write(secondary.kpcr, &bad, sizeof(bad));
        } else if (scenario == "handoff-bad-ethread") {
            const uint32_t bad = 0x12345000;
            mem::Write(secondary.ethread + 0x28, &bad, sizeof(bad));
        } else {
            const uint32_t bad = 0x12345678;
            mem::Write(secondary.tls, &bad, sizeof(bad));
        }
        const bool valid = ValidateSecondaryContext(candidate, context, error);
        candidate->tls = secondary.tls;
        if (scenario == "handoff-bad-tls") {
            const uint32_t zero = 0;
            mem::Write(secondary.tls, &zero, sizeof(zero));
        }
        candidate->ctx = nullptr;
        kernel::CancelPreparedThread(candidate);
        if (valid) return finish("corrupted secondary context was accepted", false);
        return finish(error.c_str(), true);
    }
    if (scenario == "handoff-seh" || scenario == "handoff-tls-contents") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        kernel::ThreadRef candidate;
        if (!PrepareCandidateThread(facts, candidate, error)) return finish(error.c_str(), false);
        X86Context context{};
        SecondaryFacts secondary;
        if (!InitializeSecondaryContext(candidate, context, secondary, error)) {
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            return finish(error.c_str(), false);
        }
        bool valid = false;
        if (scenario == "handoff-seh") {
            const uint32_t S = secondary.entry_esp;
            const uint32_t zero = 0, next = 0xFFFFFFFF, handler = sec::kSehHandler, scope = sec::kScopeTable;
            const uint32_t saved_esp = S - 0x38, ethread = secondary.ethread, tls4 = secondary.tls + 4;
            mem::Write(secondary.kpcr, &secondary.registration, sizeof(secondary.registration));
            mem::Write(secondary.registration, &next, sizeof(next));
            mem::Write(secondary.registration + 4, &handler, sizeof(handler));
            mem::Write(S - 0x0C, &scope, sizeof(scope));
            mem::Write(S - 0x08, &zero, sizeof(zero));
            mem::Write(S - 0x04, &zero, sizeof(zero));
            mem::Write(S - 0x1C, &saved_esp, sizeof(saved_esp));
            mem::Write(S - 0x38, &zero, sizeof(zero));
            mem::Write(S - 0x34, &zero, sizeof(zero));
            mem::Write(S - 0x30, &zero, sizeof(zero));
            mem::Write(S - 0x24, &ethread, sizeof(ethread));
            mem::Write(S - 0x28, &tls4, sizeof(tls4));
            mem::Write(S - 0x2C, &zero, sizeof(zero));
            const uint32_t wrong = 0xDEADBEEF;
            mem::Write(S - 0x04, &wrong, sizeof(wrong));
            valid = VerifySecondarySeh(secondary, error);
        } else {
            const uint32_t pointer = secondary.tls + sec::kTlsPointerBytes;
            mem::Write(secondary.tls, &pointer, sizeof(pointer));
            mem::Fill(secondary.tls + sec::kTlsPointerBytes, 0, 0x90);
            const uint8_t wrong = 1;
            mem::Write(secondary.tls + 8, &wrong, sizeof(wrong));
            valid = VerifySecondaryTls(secondary, error);
        }
        candidate->ctx = nullptr;
        kernel::CancelPreparedThread(candidate);
        if (valid) return finish("corrupted secondary state was accepted", false);
        return finish(error.c_str(), true);
    }
    if (scenario == "critical-services" || scenario == "critical-service-failure" ||
        scenario == "critical-abi-bad-cleanup") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        const uint32_t page = 0x60010000;
        if (!mem::Commit(page, mem::kPage)) return finish("could not commit the critical-section test page", false);
        uint32_t return_address = 0x11111111, arg = sec::kCriticalSection;
        mem::Write(page, &return_address, sizeof(return_address));
        mem::Write(page + 4, &arg, sizeof(arg));
        mem::Write(page + 0x10, &return_address, sizeof(return_address));
        mem::Write(page + 0x14, &arg, sizeof(arg));
        g_critical_owner_ethread = 0xDEADBEEF;
        g_critical_actual_ethread = 0xDEADBEEF;
        g_critical_held_seen = false;
        g_critical_enter_calls = 0;
        g_critical_leave_calls = 0;
        g_critical_fail_mode = CriticalFailMode::kNone;
        X86Context ctx{};
        bool valid = true;
        if (scenario == "critical-services") {
            ctx.esp = page;
            ProbeCriticalEnter(ctx, nullptr);
            valid = valid && ctx.eax == kernel::kSuccess && ctx.esp == page + 8;
            uint32_t lock = 0, recursion = 0, owner = 0;
            mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock));
            mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion));
            mem::Read(sec::kCriticalOwner, &owner, sizeof(owner));
            valid = valid && lock == 0 && recursion == 1 && owner == g_critical_owner_ethread;
            valid = valid && g_critical_held_seen && g_critical_held_lock == 0 &&
                    g_critical_held_recursion == 1 && g_critical_held_owner == g_critical_actual_ethread;
            ctx.esp = page + 0x10;
            ProbeCriticalLeave(ctx, nullptr);
            valid = valid && ctx.eax == kernel::kSuccess && ctx.esp == page + 0x18;
            mem::Read(sec::kCriticalLockCount, &lock, sizeof(lock));
            mem::Read(sec::kCriticalRecursion, &recursion, sizeof(recursion));
            mem::Read(sec::kCriticalOwner, &owner, sizeof(owner));
            valid = valid && lock == 0xFFFFFFFF && recursion == 0 && owner == 0;
            valid = valid && g_critical_after_lock == 0xFFFFFFFF && g_critical_after_recursion == 0 &&
                    g_critical_after_owner == 0;
            arg = 0x0046C8B8;
            mem::Write(page + 4, &arg, sizeof(arg));
            ctx.esp = page;
            ProbeCriticalEnter(ctx, nullptr);
            valid = valid && ctx.eax == kernel::kInvalidParameter;
            arg = sec::kCriticalSection;
            mem::Write(page + 4, &arg, sizeof(arg));
            ctx.esp = page + 0x10;
            ProbeCriticalLeave(ctx, nullptr);
            valid = valid && ctx.eax == kernel::kInvalidParameter;
            ctx.esp = page;
            ProbeCriticalEnter(ctx, nullptr);
            const uint32_t wrong_owner = 0x12345678;
            mem::Write(sec::kCriticalOwner, &wrong_owner, sizeof(wrong_owner));
            ctx.esp = page + 0x10;
            ProbeCriticalLeave(ctx, nullptr);
            valid = valid && ctx.eax == kernel::kInvalidParameter;
            const uint32_t initial = 0xFFFFFFFF, zero = 0;
            mem::Write(sec::kCriticalLockCount, &initial, sizeof(initial));
            mem::Write(sec::kCriticalRecursion, &zero, sizeof(zero));
            mem::Write(sec::kCriticalOwner, &zero, sizeof(zero));
        } else if (scenario == "critical-service-failure") {
            g_critical_fail_mode = CriticalFailMode::kEnterFail;
            ctx.esp = page;
            ProbeCriticalEnter(ctx, nullptr);
            valid = valid && ctx.eax == kernel::kInvalidParameter;
            g_critical_fail_mode = CriticalFailMode::kNone;
            ctx.esp = page;
            ProbeCriticalEnter(ctx, nullptr);
            g_critical_fail_mode = CriticalFailMode::kLeaveFail;
            ctx.esp = page + 0x10;
            ProbeCriticalLeave(ctx, nullptr);
            valid = valid && ctx.eax == kernel::kInvalidParameter;
            g_critical_fail_mode = CriticalFailMode::kNone;
            const uint32_t initial = 0xFFFFFFFF, zero = 0;
            mem::Write(sec::kCriticalLockCount, &initial, sizeof(initial));
            mem::Write(sec::kCriticalRecursion, &zero, sizeof(zero));
            mem::Write(sec::kCriticalOwner, &zero, sizeof(zero));
        } else {
            g_critical_fail_mode = CriticalFailMode::kBadCleanup;
            ctx.esp = page;
            ProbeCriticalEnter(ctx, nullptr);
            valid = valid && ctx.esp == page + 4;
            g_critical_fail_mode = CriticalFailMode::kNone;
        }
        mem::Decommit(page, mem::kPage);
        if (!valid) return finish("critical-section service contract mismatch", false);
        std::printf("RESULT ok %s\n", scenario.c_str());
        return finish(nullptr, false);
    }
    if (scenario == "critical-lock-held-cleanup") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        const uint32_t page = 0x60010000;
        if (!mem::Commit(page, mem::kPage)) return finish("could not commit the lock test page", false);
        uint32_t return_address = 0x11111111, arg = sec::kCriticalSection;
        mem::Write(page, &return_address, sizeof(return_address));
        mem::Write(page + 4, &arg, sizeof(arg));
        g_critical_owner_ethread = 0xDEADBEEF;
        g_critical_actual_ethread = 0xDEADBEEF;
        g_critical_held_seen = false;
        g_critical_enter_calls = 0;
        X86Context ctx{};
        ctx.esp = page;
        ProbeCriticalEnter(ctx, nullptr);
        mem::Decommit(page, mem::kPage);
        const bool cleanup_ok = CleanupIntegrated(options);
        std::printf("RESULT %s lock-held cleanup\n", cleanup_ok ? "ok" : "fail");
        return cleanup_ok ? 0 : 5;
    }
    if (scenario == "helper-import-slot" || scenario == "helper-ret-mismatch") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        g_secondary = SecondaryState{};
        g_secondary.running = true;
        g_secondary.first_step = false;
        X86Context ctx{};
        bool rejected = false;
        if (scenario == "helper-import-slot") {
            if (!PatchCriticalSectionImports(error)) return finish(error.c_str(), false);
            const uint32_t bad = 0xDEADBEEF;
            ProtectRange(kRealImportPage, mem::kPage, mem::kRead | mem::kWrite);
            mem::Write(sec::kCriticalEnterSlot, &bad, sizeof(bad));
            ProtectRange(kRealImportPage, mem::kPage, mem::kRead);
            g_secondary.previous_pc = 0x002B5985;
            try {
                SecondaryStep(sec::kCriticalEnterCall, ctx);
            } catch (const ProbeFailure& failure) {
                rejected = std::string(failure.reason).find("not the reviewed thunk") != std::string::npos;
            }
            RestoreCriticalSectionImports();
        } else {
            X86CheckedCallFrame frame{nullptr, 0xDEADBEEF, false};
            ctx.checked_call = &frame;
            g_secondary.previous_pc = 0x002B59B1;
            try {
                SecondaryStep(0x002B59B2, ctx);
            } catch (const ProbeFailure& failure) {
                rejected = std::string(failure.reason).find("does not expect the reviewed return") !=
                           std::string::npos;
            }
        }
        g_secondary.running = false;
        if (!rejected) return finish("helper contract violation was not rejected", false);
        std::printf("RESULT ok %s\n", scenario.c_str());
        return finish(nullptr, false);
    }
    if (scenario == "critical-restore-failure") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PatchCriticalSectionImports(error)) return finish(error.c_str(), false);
        g_critical_restore_fail = true;
        const bool restored = RestoreCriticalSectionImports();
        const bool cleanup_ok = CleanupIntegrated(options);
        (void)cleanup_ok;
        if (restored) return finish("critical-section restoration failure was accepted", false);
        std::printf("RESULT ok critical-section restoration failed\n");
        return 5;
    }
    if (scenario == "list-empty" || scenario == "list-nonempty" || scenario == "list-cycle" ||
        scenario == "list-uncommitted" || scenario == "list-corrupt") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (scenario == "list-empty") {
            if (!ValidateCriticalSectionInitial(SecondaryFacts{}, error)) return finish(error.c_str(), false);
            std::printf("RESULT ok empty critical-section list\n");
            return finish(nullptr, false);
        }
        const uint32_t page = 0x60010000;
        if (!mem::Commit(page, mem::kPage)) return finish("could not commit the list test page", false);
        const uint32_t sentinel = page + 0x100;
        uint32_t next = page + 0x10, callback = 0xDEADBEEF;
        mem::Write(page, &next, sizeof(next));
        mem::Write(page + 8, &callback, sizeof(callback));
        next = sentinel;
        callback = 0;
        mem::Write(page + 0x10, &next, sizeof(next));
        mem::Write(page + 0x18, &callback, sizeof(callback));
        uint32_t target = 0, nodes = 0;
        bool walked = false;
        if (scenario == "list-nonempty") {
            walked = WalkCriticalListForTest(page, sentinel, page, page + 0x100, target, nodes, error);
            walked = walked && target == 0xDEADBEEF && nodes == 2;
        } else if (scenario == "list-cycle") {
            next = page;
            mem::Write(page, &next, sizeof(next));
            walked = !WalkCriticalListForTest(page, sentinel, page, page + 0x100, target, nodes, error);
        } else if (scenario == "list-corrupt") {
            next = page + 2;
            mem::Write(page, &next, sizeof(next));
            walked = !WalkCriticalListForTest(page, sentinel, page, page + 0x100, target, nodes, error);
        } else {
            walked = !WalkCriticalListForTest(0x60020000, sentinel, page, page + 0x100, target, nodes, error);
        }
        mem::Decommit(page, mem::kPage);
        if (!walked) return finish("critical-section list walker accepted a bad list", false);
        std::printf("RESULT ok %s\n", scenario.c_str());
        return finish(nullptr, false);
    }
    if (scenario == "list-dynamic-call") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        g_secondary = SecondaryState{};
        g_secondary.running = true;
        g_secondary.first_step = false;
        g_secondary.previous_pc = 0x002B599F;
        X86Context ctx{};
        bool rejected = false;
        try {
            SecondaryStep(0x002B59A1, ctx);
        } catch (const ProbeFailure& failure) {
            rejected = std::string(failure.reason).find("unreviewed dynamic list callback target") != std::string::npos;
        }
        g_secondary.running = false;
        if (!rejected) return finish("dynamic list callback was not rejected", false);
        std::printf("RESULT ok dynamic list callback rejected\n");
        return finish(nullptr, false);
    }
    if (scenario == "cleanup-open-handle" || scenario == "cleanup-closed-handle") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        if (scenario == "cleanup-closed-handle" &&
            !kernel::CloseHandle(g_prepared_thread->published_handle))
            return finish("could not close the candidate handle", false);
        const bool cleanup_ok = CleanupIntegrated(options);
        std::printf("RESULT %s %s\n", cleanup_ok ? "ok" : "fail", scenario.c_str());
        return cleanup_ok ? 0 : 5;
    }
    if (scenario == "cleanup-twice") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        const bool first = CleanupIntegrated(options);
        const bool second = CleanupIntegrated(options);
        std::printf("RESULT %s cleanup-twice\n", first && second ? "ok" : "fail");
        return first && second ? 0 : 5;
    }
    if (scenario == "cleanup-cancel-failure" || scenario == "cleanup-restore-failure") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        if (scenario == "cleanup-cancel-failure") g_prepared_thread->state = kernel::ThreadState::kActive;
        else g_import_restore_fail_ordinal = kImportOrdinal;
        const bool cleanup_ok = CleanupIntegrated(options);
        std::printf("RESULT %s %s\n", cleanup_ok ? "unexpected-ok" : "expected-fail", scenario.c_str());
        return cleanup_ok ? 3 : 5;
    }
    if (scenario == "secondary-context-failure" || scenario == "secondary-execution-failure") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        if (scenario == "secondary-context-failure") {
            const uint32_t bad = 0x12345678;
            mem::Write(g_prepared_thread->kpcr, &bad, sizeof(bad));
        }
        g_integrated_fail_secondary_step = true;
        SecondaryFacts secondary;
        const bool ran = RunSecondarySegment(g_prepared_thread, secondary, error);
        g_integrated_fail_secondary_step = false;
        const bool owner_ok =
            scenario == "secondary-context-failure"
                ? g_critical_actual_ethread == 0
                : (g_critical_actual_ethread != 0 && g_prepared_thread &&
                   g_critical_actual_ethread == g_prepared_thread->ethread);
        if (g_prepared_thread) g_prepared_thread->ctx = nullptr;
        const bool cleanup_ok = CleanupIntegrated(options);
        if (ran || !owner_ok) return finish("secondary owner/bridge state is unexpected", false);
        if (scenario == "secondary-context-failure")
            std::printf("RESULT ok secondary context rejected: %s\n", error.c_str());
        return cleanup_ok ? 0 : 5;
    }
    if (scenario == "owner-bridge-failure-cleanup") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        if (!PatchCriticalSectionImports(error)) return finish(error.c_str(), false);
        g_integrated_fail_secondary_step = true;
        SecondaryFacts secondary;
        const bool ran = RunSecondarySegment(g_prepared_thread, secondary, error);
        g_integrated_fail_secondary_step = false;
        const kernel::ThreadRef thread = g_prepared_thread;
        const bool bridge_owned = !ran && thread && g_critical_actual_ethread != 0 &&
                                  g_critical_actual_ethread == thread->ethread &&
                                  g_critical_enter_calls == 0 && g_critical_leave_calls == 0;
        const uint32_t ethread = thread ? thread->ethread : 0;
        if (thread) thread->ctx = nullptr;
        const bool cleanup_ok = CleanupIntegrated(options);
        if (!bridge_owned || !cleanup_ok) {
            std::printf("RESULT fail owner-bridge-failure-cleanup bridge=%d cleanup=%d\n", int(bridge_owned),
                        int(cleanup_ok));
            return 5;
        }
        std::printf("RESULT ok owner-bridge-failure-cleanup ethread=%08X imports=restored thread=released\n",
                    ethread);
        return 0;
    }
    if (scenario == "owner-context-failure-cleanup") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        if (!PrepareCandidateThread(facts, g_prepared_thread, error)) return finish(error.c_str(), false);
        if (!PatchCriticalSectionImports(error)) return finish(error.c_str(), false);
        const uint32_t bad = 0x12345678;
        mem::Write(g_prepared_thread->kpcr, &bad, sizeof(bad));
        SecondaryFacts secondary;
        const bool ran = RunSecondarySegment(g_prepared_thread, secondary, error);
        const bool bridge_never_prepared = !ran && g_critical_actual_ethread == 0;
        if (g_prepared_thread) g_prepared_thread->ctx = nullptr;
        const bool cleanup_ok = CleanupIntegrated(options);
        if (!bridge_never_prepared || !cleanup_ok) {
            std::printf("RESULT fail owner-context-failure-cleanup bridge=%d cleanup=%d\n",
                        int(bridge_never_prepared), int(cleanup_ok));
            return 5;
        }
        std::printf("RESULT ok owner-context-failure-cleanup bridge=unprepared imports=restored thread=released\n");
        return 0;
    }
    if (scenario == "forbidden-entry") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        const dispatch::ResolvedTarget start = dispatch::ResolveTarget(sec::kStartRoutine);
        const dispatch::ResolvedTarget stack = dispatch::ResolveTarget(sec::kStackHelper);
        const dispatch::ResolvedTarget next = dispatch::ResolveTarget(sec::kNextHelper);
        const dispatch::ResolvedTarget handler = dispatch::ResolveTarget(sec::kSehHandler);
        const bool mapped = start.kind == dispatch::TargetKind::kGeneratedFunction &&
                            stack.kind == dispatch::TargetKind::kGeneratedFunction &&
                            next.kind == dispatch::TargetKind::kGeneratedFunction;
        const bool handler_closed = handler.kind != dispatch::TargetKind::kGeneratedFunction &&
                                    handler.kind != dispatch::TargetKind::kContinuation &&
                                    handler.kind != dispatch::TargetKind::kKernelThunk &&
                                    handler.kind != dispatch::TargetKind::kHostThunk;
        if (!mapped || !handler_closed) return finish("forbidden boundary resolution is unexpected", false);
        return finish(nullptr, false);
    }
    if (scenario == "cancel-twice" || scenario == "close-twice") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        kernel::ThreadRef candidate;
        if (!PrepareCandidateThread(facts, candidate, error)) return finish(error.c_str(), false);
        if (scenario == "close-twice") {
            const bool first = kernel::CloseHandle(candidate->published_handle);
            const bool second = kernel::CloseHandle(candidate->published_handle);
            candidate->ctx = nullptr;
            kernel::CancelPreparedThread(candidate);
            if (!first || second) return finish("duplicate close behaviour is unexpected", false);
            return finish(nullptr, false);
        }
        candidate->ctx = nullptr;
        const bool first = kernel::CancelPreparedThread(candidate);
        const bool second = kernel::CancelPreparedThread(candidate);
        if (!first || second) return finish("duplicate cancellation behaviour is unexpected", false);
        return finish(nullptr, false);
    }
    if (scenario == "restore-failure") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) return finish(error.c_str(), false);
        g_import_restore_fail_ordinal = kImportOrdinal;
        const bool teardown_ok = TeardownReal(options);
        if (teardown_ok) return finish("restoration failure was accepted", false);
        return 5;
    }
    std::printf("SETUP fail unknown integrated scenario %s\n", scenario.c_str());
    return 4;
}

int RunIntegratedExecute() {
    if (!mem::Init()) {
        std::printf("SETUP fail memory init\n");
        return 4;
    }
    if (!kernel::RestartKernelWorkers()) {
        std::printf("SETUP fail callback gate\n");
        mem::Shutdown();
        return 4;
    }
    std::string error;
    xbe::Image image;
    Options options;
    RealFacts root_facts;
    if (!PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, root_facts, error)) {
        std::printf("SETUP fail %s\n", error.c_str());
        return CleanupIntegrated(options) ? 4 : 5;
    }
    if (!ValidateSecondaryRouteText(image, error)) {
        std::printf("SETUP fail %s\n", error.c_str());
        return CleanupIntegrated(options) ? 4 : 5;
    }
    options.allow_root_return = true;
    options.root_sentinel_at_entry = true;
    const Report report = RunProbe(options);
    if (report.failure) {
        PrintFailure(report.error);
        return CleanupIntegrated(options) ? 3 : 5;
    }
    if (!VerifyRealPostconditions(root_facts, error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        return CleanupIntegrated(options) ? 3 : 5;
    }
    const uint32_t root_instructions = g_probe.executed;
    const uint32_t root_direct = g_probe.direct_calls;
    const uint32_t root_kernel = g_probe.kernel_calls;
    const uint32_t root_imports = g_probe.import_reads;
    if (root_instructions != kIntegratedRootInstructions || root_direct != 2 || root_kernel != 2 ||
        root_imports != 2) {
        std::printf("RESULT fail reason=\"root segment totals are not the reviewed set\"\n");
        return CleanupIntegrated(options) ? 3 : 5;
    }
    if (!RestoreImportSlots()) {
        std::printf("RESULT fail reason=\"import restoration failed\"\n");
        CleanupIntegrated(options);
        return 5;
    }
    if (!VerifyImportRestoration(root_facts, error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        CleanupIntegrated(options);
        return 5;
    }
    const kernel::ThreadRef thread = g_prepared_thread;
    if (!ValidateRetainedThread(root_facts, thread, true, error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        return CleanupIntegrated(options) ? 3 : 5;
    }
    if (!PatchCriticalSectionImports(error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        return CleanupIntegrated(options) ? 3 : 5;
    }
    SecondaryFacts secondary;
    if (!RunSecondarySegment(thread, secondary, error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        if (thread) thread->ctx = nullptr;
        return CleanupIntegrated(options) ? 3 : 5;
    }
    thread->ctx = nullptr;
    if (!RestoreCriticalSectionImports()) {
        std::printf("RESULT fail reason=\"critical-section import restoration failed\"\n");
        return CleanupIntegrated(options) ? 3 : 5;
    }
    ReleaseCriticalSectionBridge();
    const uint32_t secondary_instructions = g_secondary.executed;
    const uint32_t secondary_direct = g_secondary.direct_calls;
    const uint32_t secondary_kernel = g_secondary.kernel_calls;
    const uint32_t secondary_imports = g_secondary.import_reads;
    const uint32_t combined_instructions = root_instructions + secondary_instructions;
    const uint32_t combined_direct = root_direct + secondary_direct;
    const uint32_t combined_kernel = root_kernel + secondary_kernel;
    const uint32_t combined_imports = root_imports + secondary_imports;
    if (secondary_instructions != kIntegratedSecondaryInstructions || secondary_direct != 2 ||
        secondary_kernel != 2 ||
        secondary_imports != 2 || combined_instructions != kIntegratedTotalInstructions ||
        combined_direct != kIntegratedDirectCalls || combined_kernel != kIntegratedKernelCalls ||
        combined_imports != kIntegratedImportReads) {
        std::printf("RESULT fail reason=\"combined segment totals are not the reviewed set\"\n");
        return CleanupIntegrated(options) ? 3 : 5;
    }
    if (!CancelRealPreparedThread(root_facts, error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        return CleanupIntegrated(options) ? 3 : 5;
    }
    if (!TeardownReal(options)) {
        std::printf("RESULT fail reason=\"teardown failed\"\n");
        return 5;
    }
    std::printf("INTEGRATED_REAL_EXEC ok instructions=%u root=%u secondary=%u direct_calls=%u kernel_calls=%u "
                "import_reads=%u workers=%zu last=%08X stop=%08X\n",
                combined_instructions, root_instructions, secondary_instructions, combined_direct, combined_kernel,
                combined_imports, kernel::KernelWorkerCountForTest(), uint32_t(0x002B59B2), sec::kStopBefore);
    return 0;
}

}  // namespace

namespace integrated_probe {

using execution_probe::kRoute;
using execution_probe::kRouteCount;
namespace sec = execution_probe_integrated_secondary;

bool ValidateMappingTextFromStrings(const std::string& header, const std::string& source,
                                    const std::string& mapping, std::string& error) {
    if (mapping.find("const uint32_t X86EntryPoint = 0x2B3916u;") == std::string::npos ||
        mapping.find("const std::size_t X86FuncMappingCount = 11;") == std::string::npos ||
        mapping.find("const std::size_t X86ContinuationCount = 0;") == std::string::npos) {
        error = "generated integrated mapping entry, function or continuation count is unexpected";
        return false;
    }
    for (const auto& step : kRoute) {
        char name[64];
        std::snprintf(name, sizeof(name), "ProbeStep_0x%08X(", step.pc);
        if (Occurrences(header, name) != 1 || Occurrences(source, name) != 1) {
            error = std::string("root hook coverage mismatch at ") + name;
            return false;
        }
    }
    for (const auto& step : sec::kRoute) {
        char name[64];
        std::snprintf(name, sizeof(name), "SecondaryStep_0x%08X(", step.pc);
        if (Occurrences(header, name) != 1 || Occurrences(source, name) != 1) {
            error = std::string("secondary hook coverage mismatch at ") + name;
            return false;
        }
    }
    for (uint32_t pc : sec::kTrapPcs) {
        char name[64];
        std::snprintf(name, sizeof(name), "SecondaryStep_0x%08X(", pc);
        if (Occurrences(header, name) != 1 || Occurrences(source, name) != 1) {
            error = std::string("secondary trap hook coverage mismatch at ") + name;
            return false;
        }
    }
    if (Occurrences(source, "ProbeStep_0x") != kRouteCount ||
        Occurrences(header, "ProbeStep_0x") != kRouteCount) {
        error = "root hook counts are not exactly the reviewed set";
        return false;
    }
    const size_t secondary_hooks = sec::kRouteCount + sec::kTrapCount;
    if (Occurrences(source, "SecondaryStep_0x") != secondary_hooks ||
        Occurrences(header, "SecondaryStep_0x") != secondary_hooks) {
        error = "secondary hook counts are not exactly the reviewed set";
        return false;
    }
    if (Occurrences(source, "X86_FUNC(__imp__sub_") != 11) {
        error = "generated integrated source does not contain exactly eleven reviewed functions";
        return false;
    }
    for (const char* function : {"X86_FUNC(__imp__sub_002B3916)", "X86_FUNC(__imp__sub_002B5CA7)",
                                   "X86_FUNC(__imp__sub_002B2759)", "X86_FUNC(__imp__sub_002B3826)",
                                   "X86_FUNC(__imp__sub_002B5C0F)", "X86_FUNC(__imp__sub_00319804)",
                                   "X86_FUNC(__imp__sub_002B38A2)", "X86_FUNC(__imp__sub_002B58B8)",
                                   "X86_FUNC(__imp__sub_002B597D)"}) {
        if (source.find(function) == std::string::npos) {
            error = std::string("generated integrated source is missing ") + function;
            return false;
        }
    }
    if (Occurrences(source, "x86_checked_call") != 7 ||
        source.find("x86_checked_call(ctx, base, 0x2B5CA7u, 0x2B396Fu, 0x2B396Au)") == std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x2B55EEu, 0x2B397Fu, 0x2B397Au)") == std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x2B2759u, 0x2B3985u, 0x2B3980u)") == std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x2B3826u, 0x2B2772u, 0x2B276Du)") == std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x319804u, 0x2B5C1Bu, 0x2B5C16u)") == std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x2B58B8u, 0x2B38A7u, 0x2B38A2u)") == std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x2B597Du, 0x2B5C7Bu, 0x2B5C76u)") == std::string::npos) {
        error = "generated integrated source does not contain exactly the reviewed direct calls";
        return false;
    }
    if (Occurrences(source, "x86_checked_indirect_call") != 5 ||
        source.find("x86_checked_indirect_call(ctx, base, 0x3C9E68u, 0x2B5CE6u, 0x2B5CE0u)") == std::string::npos ||
        source.find("x86_checked_indirect_call(ctx, base, 0x3C9D4Cu, 0x2B2763u, 0x2B275Du)") == std::string::npos ||
        source.find("x86_checked_indirect_call(ctx, base, 0x3C9E44u, 0x2B598Cu, 0x2B5986u)") == std::string::npos ||
        source.find("x86_checked_indirect_call(ctx, base, 0x3C9E40u, 0x2B59AFu, 0x2B59A9u)") == std::string::npos ||
        source.find("x86_checked_indirect_call(ctx, base, (uint32_t)(ctx.eax + 0x8u), 0x2B59A4u, 0x2B59A1u)") ==
            std::string::npos ||
        Occurrences(source, "0x3C9E44u") != 1 || Occurrences(source, "0x3C9E40u") != 1) {
        error = "generated integrated source does not contain the reviewed imports and dynamic list operand";
        return false;
    }
    if (Occurrences(source, "x86_checked_ret") != 5 ||
        source.find("x86_checked_ret(ctx, 0x2B5D12u, 24)") == std::string::npos ||
        source.find("x86_checked_ret(ctx, 0x2B2774u, 4)") == std::string::npos ||
        source.find("x86_checked_ret(ctx, 0x2B3987u, 0)") == std::string::npos ||
        source.find("x86_checked_ret(ctx, 0x31983Eu, 0)") == std::string::npos ||
        source.find("x86_checked_ret(ctx, 0x2B59B2u, 4)") == std::string::npos) {
        error = "generated integrated source does not contain the reviewed RETs";
        return false;
    }
    if (Occurrences(source, "x86_rep_movs_checked") != 2 || Occurrences(source, "x86_rep_stos_checked") != 2) {
        error = "generated integrated source does not contain the reviewed REP forms";
        return false;
    }
    for (const char* forbidden : {"x86_icall", "x86_kernel", "ProbeStop_"}) {
        if (source.find(forbidden) != std::string::npos) {
            error = std::string("generated integrated source contains ") + forbidden;
            return false;
        }
    }
    if (Occurrences(source, "x86_resume") != 1 || Occurrences(source, "catch (const X86Resume& r)") != 1) {
        error = "generated integrated source contains an unexpected X86Resume path";
        return false;
    }
    for (const char* after_boundary : {"// 002B5D15", "// 002B2777", "// 002B3988", "// 002B5C7E",
                                       "// 0031983F", "// 002B38A7", "// 002B58BD", "// 002B59B5"}) {
        if (source.find(after_boundary) != std::string::npos) {
            error = "generated integrated source decoded past a reviewed boundary";
            return false;
        }
    }
    return true;
}

bool ValidateMapping(std::string& error) {
    std::string header, source, mapping;
    const std::string dir = EXECUTION_PROBE_REAL_GENERATED_DIR;
    std::ifstream header_file(dir + "/x86_recomp_shared.h", std::ios::binary);
    std::ifstream source_file(dir + "/x86_recomp.0.cpp", std::ios::binary);
    std::ifstream mapping_file(dir + "/x86_func_mapping.cpp", std::ios::binary);
    if (!header_file || !source_file || !mapping_file) {
        error = "generated integrated mapping files are missing";
        return false;
    }
    header.assign(std::istreambuf_iterator<char>(header_file), {});
    source.assign(std::istreambuf_iterator<char>(source_file), {});
    mapping.assign(std::istreambuf_iterator<char>(mapping_file), {});
    return ValidateMappingTextFromStrings(header, source, mapping, error);
}

}  // namespace integrated_probe

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) {
        const std::string scenario = argv[2];
        if (scenario == "execute-integrated" || scenario == "supervised-check") {
            if (!SupervisionTokenValid()) {
                std::printf("REFUSED: real integrated execution requires the supervised parent\n");
                return 2;
            }
            if (scenario == "supervised-check") {
                std::printf("RESULT ok supervision=1\n");
                return 0;
            }
            alarm(12);
            return RunIntegratedExecute();
        }
        return RunIntegratedPreflight(scenario);
    }

    bool execute_real = false;
    const char* mode = "self-test";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--execute-real") == 0) execute_real = true;
        else if (std::strcmp(argv[i], "--preflight") == 0) mode = "preflight";
        else if (std::strcmp(argv[i], "--self-test") == 0) mode = "self-test";
        else if (std::strcmp(argv[i], "--run-once") == 0) mode = "run-once";
        else {
            std::printf("usage: %s [--preflight|--self-test|--run-once --execute-real]\n", argv[0]);
            return 2;
        }
    }
    const std::string self = SelfPath();
    if (self.empty()) {
        std::printf("FAIL: cannot locate the integrated probe executable\n");
        return 1;
    }
    if (std::strcmp(mode, "preflight") == 0) {
        const ChildResult result = SpawnChild(self, "preflight", std::chrono::seconds(15));
        std::printf("%s", result.output.c_str());
        return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 ? 0 : 1;
    }
    if (std::strcmp(mode, "run-once") == 0) {
        if (!execute_real) {
            std::printf("REFUSED: pass --execute-real only after explicit approval\n");
            return 2;
        }
        const ChildResult result = SpawnSupervisedChild(self, "execute-integrated", std::chrono::seconds(15));
        std::printf("%s", result.output.c_str());
        if (result.timed_out) {
            std::printf("INTEGRATED REAL EXECUTION FAIL: timeout\n");
            return 1;
        }
        return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 ? 0 : 1;
    }

    struct Case {
        const char* scenario;
        int expect_exit;
        const char* expected;
    };
    const Case cases[] = {
        {"preflight", 0, "PREFLIGHT PASS"},
        {"identity-missing", 3, "real XBE is missing"},
        {"identity-mismatch", 3, "SHA-1 mismatch"},
        {"root-route-mismatch", 3, "route"},
        {"secondary-route-mismatch", 3, "secondary route"},
        {"root-hook-missing", 3, "root hook coverage"},
        {"secondary-hook-missing", 3, "secondary hook coverage"},
        {"root-failure-no-thread", 3, "root failure before thread preparation"},
        {"root-failure-after-prep", 3, "root failure after thread preparation"},
        {"handoff-no-thread", 3, "no retained ThreadRef"},
        {"handoff-active-thread", 3, "not kPrepared"},
        {"handoff-stale-handle", 3, "still resolves"},
        {"handoff-second-thread", 3, "second thread preparation was refused"},
        {"handoff-bad-fs", 3, "FS base"},
        {"handoff-bad-kpcr", 3, "fs:[0]"},
        {"handoff-bad-ethread", 3, "ETHREAD"},
        {"handoff-bad-tls", 3, "TLS pointer slot"},
        {"handoff-seh", 3, "SEH saved"},
        {"handoff-tls-contents", 3, "TLS zero-fill mismatch"},
        {"critical-services", 0, "RESULT ok critical-services"},
        {"critical-service-failure", 0, "RESULT ok critical-service-failure"},
        {"critical-abi-bad-cleanup", 0, "RESULT ok critical-abi-bad-cleanup"},
        {"critical-restore-failure", 5, "critical-section restoration failed"},
        {"critical-lock-held-cleanup", 0, "lock-held cleanup"},
        {"helper-import-slot", 0, "RESULT ok helper-import-slot"},
        {"helper-ret-mismatch", 0, "RESULT ok helper-ret-mismatch"},
        {"list-empty", 0, "empty critical-section list"},
        {"list-nonempty", 0, "RESULT ok list-nonempty"},
        {"list-cycle", 0, "RESULT ok list-cycle"},
        {"list-corrupt", 0, "RESULT ok list-corrupt"},
        {"list-uncommitted", 0, "RESULT ok list-uncommitted"},
        {"list-dynamic-call", 0, "dynamic list callback rejected"},
        {"cleanup-open-handle", 0, "CLEANUP thread="},
        {"cleanup-closed-handle", 0, "CLEANUP thread="},
        {"cleanup-twice", 0, "cleanup-twice"},
        {"cleanup-cancel-failure", 5, "integrated cleanup did not complete"},
        {"cleanup-restore-failure", 5, "integrated cleanup did not complete"},
        {"secondary-context-failure", 0, "secondary context rejected"},
        {"secondary-execution-failure", 0, "injected secondary execution failure"},
        {"owner-prepared-thread", 0, "RESULT ok owner-prepared-thread"},
        {"owner-zero-rejected", 0, "RESULT ok owner-zero-rejected"},
        {"owner-other-thread-rejected", 0, "RESULT ok owner-other-thread-rejected"},
        {"owner-bad-kpcr", 0, "RESULT ok owner-bad-kpcr"},
        {"owner-bridge-failure-cleanup", 0, "RESULT ok owner-bridge-failure-cleanup"},
        {"owner-context-failure-cleanup", 0, "RESULT ok owner-context-failure-cleanup"},
        {"branch-jz-not-taken", 0, "RESULT ok branch-jz-not-taken"},
        {"branch-jz-taken", 0, "RESULT ok branch-jz-taken"},
        {"branch-jnz-not-taken", 0, "RESULT ok branch-jnz-not-taken"},
        {"branch-jnz-taken", 0, "RESULT ok branch-jnz-taken"},
        {"branch-bad-successor", 0, "unexpected secondary control transfer"},
        {"branch-flag-mismatch", 0, "secondary branch outcome mismatch"},
        {"branch-jz-flag-mismatch", 0, "secondary branch outcome mismatch"},
        {"branch-failure-cleanup", 0, "RESULT ok branch-failure-cleanup"},
        {"abi-correct", 0, "RESULT ok abi-correct"},
        {"abi-enter-return-mismatch", 0, "RESULT ok abi-enter-return-mismatch"},
        {"abi-enter-arg-mismatch", 0, "RESULT ok abi-enter-arg-mismatch"},
        {"abi-leave-return-mismatch", 0, "RESULT ok abi-leave-return-mismatch"},
        {"abi-leave-arg-mismatch", 0, "RESULT ok abi-leave-arg-mismatch"},
        {"abi-leave-esp-mismatch", 0, "RESULT ok abi-leave-esp-mismatch"},
        {"generated-audit-unsupported", 0, "RESULT ok generated-audit-unsupported"},
        {"generated-audit-missing", 0, "RESULT ok generated-audit-missing"},
        {"forbidden-entry", 0, ""},
        {"cancel-twice", 0, ""},
        {"close-twice", 0, ""},
        {"restore-failure", 5, "IMPORT restored=0"},
    };
    int failed = 0;
    for (const Case& test : cases) {
        const ChildResult result = SpawnChild(self, test.scenario, std::chrono::seconds(15));
        const int exit_code = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : -1;
        const bool condition = !result.timed_out && exit_code == test.expect_exit &&
                               (test.expected[0] == 0 || result.output.find(test.expected) != std::string::npos) &&
                               result.output.find("INTEGRATED_REAL_EXEC ok") == std::string::npos;
        std::printf("%s: integrated child %s exits %d\n", condition ? "PASS" : "FAIL", test.scenario, exit_code);
        if (!condition) ++failed;
    }

    const ChildResult unauthorised = SpawnChild(self, "execute-integrated", std::chrono::seconds(5));
    const int unauthorised_exit = WIFEXITED(unauthorised.status) ? WEXITSTATUS(unauthorised.status) : -1;
    const bool unauthorised_ok = !unauthorised.timed_out && unauthorised_exit == 2 &&
                                 unauthorised.output.find("REFUSED") != std::string::npos;
    std::printf("%s: direct --child execute-integrated is refused without the supervised parent\n",
                unauthorised_ok ? "PASS" : "FAIL");
    if (!unauthorised_ok) ++failed;

    const ChildResult supervised = SpawnSupervisedChild(self, "supervised-check", std::chrono::seconds(5));
    const int supervised_exit = WIFEXITED(supervised.status) ? WEXITSTATUS(supervised.status) : -1;
    const bool supervised_ok = !supervised.timed_out && supervised_exit == 0 &&
                               supervised.output.find("RESULT ok supervision=1") != std::string::npos;
    std::printf("%s: supervised token/fd pathway is wired without execution\n", supervised_ok ? "PASS" : "FAIL");
    if (!supervised_ok) ++failed;

    if (execute_real) {
        std::printf("REFUSED: --execute-real requires --run-once\n");
        ++failed;
    }
    std::printf("integrated real probe: %s\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}
