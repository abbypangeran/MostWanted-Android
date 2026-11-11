// Phase 2D-19 synthetic rehearsal for the authentic-run harness. Every test
// below drives generated synthetic guest code through the same gate, claim,
// budget, SEH/TLS, kernel-call and cancellation paths the authentic experiment
// would use. No authentic instruction, address or image is executed.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/authentic_run.h"
#include "kernel/crypto.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xbe/image_loader.h"
#include "xbe/xbe.h"
#include "x86_recomp_shared.h"

#include "authentic_run_fixture_route.h"

namespace run = kernel::authentic;
namespace route = kernel::authentic::route;
namespace fixture = authentic_run_fixture;

namespace kernel {
#ifndef MOSTWANTED_FIXTURE_EXTERNAL_G_BASE
uint8_t* g_base = nullptr;
#endif
}

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

std::string Sha1Hex(const std::vector<uint8_t>& data) {
    alignas(8) std::array<uint8_t, 92> context{};
    uint8_t digest[20];
    crypto::ShaInit(context.data());
    crypto::ShaUpdate(context.data(), data.data(), data.size());
    crypto::ShaFinal(context.data(), digest);
    char text[41];
    for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}

run::RouteManifest ParentManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kParentRoute;
    manifest.count = fixture::kParentRouteCount;
    manifest.entry = fixture::kParentEntry;
    manifest.stop = fixture::kParentStop;
    manifest.executed_instructions = fixture::kParentExecutedInstructions;
    manifest.trap_entries = fixture::kParentTrapEntries;
    manifest.trap_count = 2;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

run::RouteManifest ChildManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kChildRoute;
    manifest.count = fixture::kChildRouteCount;
    manifest.entry = fixture::kChildEntry;
    manifest.stop = fixture::kChildStop;
    manifest.executed_instructions = fixture::kChildExecutedInstructions;
    manifest.trap_entries = fixture::kChildTrapEntries;
    manifest.trap_count = 2;
    manifest.seh_call = fixture::kSehCall;
    manifest.seh_entry = fixture::kSehEntry;
    manifest.seh_handler = fixture::kSehHandler;
    manifest.seh_scope_table = fixture::kSehScopeTable;
    manifest.seh_scope_bytes = fixture::kSehScopeBytes;
    manifest.seh_scope_flag = fixture::kSehScopeFlag;
    manifest.tls_block_size = fixture::kTlsBlockSize;
    manifest.critical_section_object = fixture::kCriticalSectionObject;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

run::RouteManifest UnsupportedManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kUnsupportedRoute;
    manifest.count = fixture::kUnsupportedRouteCount;
    manifest.entry = fixture::kUnsupportedRoute[0].pc;
    manifest.stop = fixture::kUnsupportedStop;
    manifest.executed_instructions = 1;
    manifest.trap_entries = fixture::kUnsupportedTrapEntries;
    manifest.trap_count = 1;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

run::RouteManifest RetImmManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kRetImmRoute;
    manifest.count = fixture::kRetImmRouteCount;
    manifest.entry = fixture::kRetImmEntry;
    manifest.stop = fixture::kRetImmStop;
    manifest.executed_instructions = fixture::kRetImmExecutedInstructions;
    manifest.trap_entries = fixture::kRetImmTrapEntries;
    manifest.trap_count = 1;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

run::RouteManifest MemoryManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kMemoryRoute;
    manifest.count = fixture::kMemoryRouteCount;
    manifest.entry = fixture::kMemoryRoute[0].pc;
    manifest.stop = fixture::kMemoryStop;
    manifest.executed_instructions = 1;
    manifest.trap_entries = fixture::kMemoryTrapEntries;
    manifest.trap_count = 1;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

struct Fixture {
    xbe::Image image;
    std::string sha1;
    run::RouteManifest parent;
    run::RouteManifest child;
    bool ready = false;
};

bool PrepareFixture(Fixture& fixture, std::string& error) {
    std::ifstream file(AUTHENTIC_RUN_FIXTURE_XBE, std::ios::binary);
    if (!file) {
        error = "cannot read the synthetic fixture XBE";
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (!fixture.image.Parse(bytes, error)) return false;
    fixture.sha1 = Sha1Hex(bytes);
    kernel::SetImageIdentity(fixture.sha1.c_str());
    kernel::g_base = mem::Base();
    if (!xbe::PrepareImage(fixture.image, fixture::kParentEntry, fixture.sha1, error)) return false;
    if (mem::Protect(fixture::kParentEntry & ~(mem::kPage - 1), mem::kPage, mem::kRead | mem::kExecute) !=
        mem::AccessStatus::kOk) {
        error = "synthetic code page is not executable";
        return false;
    }
    if (!dispatch::Init(error)) return false;
    if (!kernel::RestartKernelWorkers()) {
        error = "worker gate did not open";
        return false;
    }
    if (run::InstallHarnessKernelExports() != run::Status::kReady) {
        error = "harness kernel exports did not install";
        return false;
    }
    if (!mem::Commit(fixture::kDataPage, 2 * mem::kPage)) {
        error = "synthetic data pages did not commit";
        return false;
    }
    // The image is executable; the synthetic data page is not.
    if (mem::Protect(fixture::kDataPage, 2 * mem::kPage, mem::kRead | mem::kWrite) != mem::AccessStatus::kOk) {
        error = "synthetic data pages are not writable";
        return false;
    }
    fixture.parent = ParentManifest();
    fixture.child = ChildManifest();
    fixture.ready = true;
    return true;
}

void ResetData() {
    std::array<uint8_t, 2 * mem::kPage> zero{};
    mem::Write(fixture::kDataPage, zero.data(), zero.size());
    const uint32_t terminator = 0xFFFFFFFFu;
    mem::Write(fixture::kScopeTableAddress, &terminator, sizeof(terminator));
    // RtlInitializeCriticalSection layout used by the production export.
    const uint32_t header = 0x00040001u, zero32 = 0, self = fixture::kCriticalSectionAddress + 8;
    const uint32_t free_count = 0xFFFFFFFFu;
    mem::Write(fixture::kCriticalSectionAddress + 0x00, &header, sizeof(header));
    mem::Write(fixture::kCriticalSectionAddress + 0x04, &zero32, sizeof(zero32));
    mem::Write(fixture::kCriticalSectionAddress + 0x08, &self, sizeof(self));
    mem::Write(fixture::kCriticalSectionAddress + 0x0C, &self, sizeof(self));
    mem::Write(fixture::kCriticalSectionAddress + 0x10, &free_count, sizeof(free_count));
    mem::Write(fixture::kCriticalSectionAddress + 0x14, &zero32, sizeof(zero32));
    mem::Write(fixture::kCriticalSectionAddress + 0x18, &zero32, sizeof(zero32));
    const uint32_t thunk255 = dispatch::KernelThunkAddress(255);
    const uint32_t thunk187 = dispatch::KernelThunkAddress(187);
    const uint32_t thunk277 = dispatch::KernelThunkAddress(277);
    const uint32_t thunk294 = dispatch::KernelThunkAddress(294);
    mem::Write(fixture::kSlot255, &thunk255, sizeof(thunk255));
    mem::Write(fixture::kSlot187, &thunk187, sizeof(thunk187));
    mem::Write(fixture::kSlot277, &thunk277, sizeof(thunk277));
    mem::Write(fixture::kSlot294, &thunk294, sizeof(thunk294));
}

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

bool WaitForQuiescence(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    return WaitFor([] { return kernel::KernelWorkerTasksFinishedForTest(); }, timeout) &&
           WaitFor([] { return kernel::Threads().empty(); }, timeout);
}

bool WaitForRole(run::Role role, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    return WaitFor([role] {
        run::RunEvidence evidence;
        return run::EvidenceForRole(role, &evidence) &&
               (evidence.status == run::Status::kStopped || evidence.status == run::Status::kCancelled ||
                evidence.status == run::Status::kBudgetExceeded || evidence.status == run::Status::kForbiddenBranch ||
                evidence.status == run::Status::kMemoryViolation ||
                evidence.status == run::Status::kUnsupportedInstruction ||
                evidence.status == run::Status::kSehViolation ||
                evidence.status == run::Status::kKernelRejected ||
                evidence.status == run::Status::kTlsViolation) &&
               evidence.trace_count > 0;
    }, timeout) &&
           WaitForQuiescence(timeout);
}

struct Started {
    run::RunAuthorization authorization;
    kernel::ThreadRef parent;
    run::Status status = run::Status::kInternalError;
};

Started StartParent(const Fixture& fixture, uint32_t child_instruction_budget = fixture::kChildExecutedInstructions,
                    const run::RouteManifest* child = nullptr, uint32_t instruction_budget = fixture::kParentExecutedInstructions) {
    WaitForQuiescence();
    Started started;
    run::ParentStartRequest request;
    request.manifest = &fixture.parent;
    request.child_manifest = child ? child : &fixture.child;
    request.image_sha1 = fixture.sha1.c_str();
    request.mapping_identity = dispatch::MappingIdentity();
    request.dispatch_generation = dispatch::CallLease().Generation();
    request.memory_generation = mem::Generation();
    request.stack_size = 0x10000;
    request.tls_size = 0x94;
    request.instruction_budget = instruction_budget;
    request.transfer_budget = 64;
    request.kernel_budget = 4;
    request.maximum_call_depth = 4;
    request.child_instruction_budget = child_instruction_budget;
    request.child_transfer_budget = 64;
    request.child_kernel_budget = 4;
    request.child_maximum_call_depth = 4;
    request.prepared_thread = &started.parent;
    started.status = run::StartParentWorkerForTest(request, &started.authorization);
    return started;
}

void JoinThread(const kernel::ThreadRef& thread) {
    if (thread) WaitFor([&] { return thread->state.load() == kernel::ThreadState::kExited ||
                                     thread->state.load() == kernel::ThreadState::kCancelled; });
}

}  // namespace

// Injection hooks fire at the entry of the named reviewed instruction, before
// it executes. They are the deterministic failure injection for the tests.
namespace {
void ForceBranchNotTaken(X86Context& ctx) { ctx.ecx = 0; }
void FaultAtTlsWrite(X86Context& ctx) { ctx.edx = 0x00020000; }
uint32_t StopFrameE(const X86Context& ctx) { return ctx.ebp - 4; }
void CorruptScopeFlag(X86Context& ctx) { const uint32_t v = 0x1234; mem::Write(StopFrameE(ctx), &v, 4); }
void CorruptHandler(X86Context& ctx) { const uint32_t v = 0xDEADBEEF; mem::Write(StopFrameE(ctx) - 8, &v, 4); }
void CorruptPrevious(X86Context& ctx) { const uint32_t v = 0xDEADBEEF; mem::Write(StopFrameE(ctx) - 0xC, &v, 4); }
void CorruptScopeTable(X86Context& ctx) { const uint32_t v = 0; mem::Write(StopFrameE(ctx) - 4, &v, 4); }
void CorruptSavedEsp(X86Context& ctx) { const uint32_t v = 0; mem::Write(StopFrameE(ctx) - 0x14, &v, 4); }
void DetachExceptionChain(X86Context& ctx) { const uint32_t v = 0xFFFFFFFF; mem::Write(ctx.fs_base, &v, 4); }
void SetCallerEbp(X86Context& ctx) { ctx.ebp = 0x1000; }
void CorruptCallerSlot(X86Context& ctx) { const uint32_t v = 0x1234; mem::Write(StopFrameE(ctx) + 4, &v, 4); }
void CorruptReturnAddress(X86Context& ctx) { const uint32_t v = 0xDEADBEEF; mem::Write(ctx.esp, &v, 4); }
std::atomic<bool> g_stack_probe{false};
std::atomic<bool> g_stack_mutation_started{false};
std::atomic<bool> g_stack_mutation_done{false};
std::atomic<int> g_stack_mutation_status{-1};
void HoldForStackMutation(X86Context&) {
    g_stack_probe = true;
    for (unsigned i = 0; i < 5000 && !g_stack_mutation_started.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void CancelAfterChildStart(X86Context&) { kernel::authentic::CancelAllRuns(); }
void CancelAtChildEntry(X86Context&) { kernel::authentic::CancelAllRuns(); }

struct RouteRun {
    kernel::ThreadRef thread;
    run::RunAuthorization authorization;
    run::Status start = run::Status::kInternalError;
    bool have = false;
    run::RunEvidence evidence;
};

RouteRun RunSyntheticRoute(const Fixture& fixture, const run::RouteManifest& manifest, uint32_t instruction_budget,
                           uint32_t transfer_budget, uint32_t kernel_budget, uint32_t maximum_depth) {
    WaitForQuiescence();
    RouteRun result;
    if (kernel::PrepareSystemThreadEx(0, 0x10000, 0x94, 0, manifest.entry, 0, false, 0, result.thread) !=
        kernel::kSuccess)
        return result;
    result.start = run::AuthorizeSyntheticRouteForThread(
        result.thread.get(), manifest, fixture.sha1.c_str(), dispatch::MappingIdentity(),
        dispatch::CallLease().Generation(), mem::Generation(), instruction_budget, transfer_budget, kernel_budget,
        maximum_depth, &result.authorization);
    if (result.start != run::Status::kReady) return result;
    if (!kernel::StartThread(result.thread)) {
        result.start = run::Status::kInvalidThread;
        return result;
    }
    JoinThread(result.thread);
    WaitForRole(run::Role::kParent);
    result.have = run::EvidenceForRole(run::Role::kParent, &result.evidence);
    return result;
}

struct ChildRun {
    Started started;
    run::RunEvidence parent;
    run::RunEvidence child;
    bool have_parent = false;
    bool have_child = false;
};

template <typename Setup>
ChildRun RunWithChildHooks(const Fixture& fixture, Setup setup) {
    ResetData();
    setup();
    ChildRun result;
    result.started = StartParent(fixture);
    JoinThread(result.started.parent);
    WaitForRole(run::Role::kChild);
    WaitForRole(run::Role::kParent);
    result.have_parent = run::EvidenceForRole(run::Role::kParent, &result.parent);
    result.have_child = run::EvidenceForRole(run::Role::kChild, &result.child);
    run::ClearInjectionHooksForTest();
    return result;
}

bool HasSehCheck(const run::RunEvidence& evidence, run::SehCheckKind kind, run::SehCheckStatus status) {
    for (size_t i = 0; i < evidence.seh_check_count; ++i)
        if (evidence.seh_checks[i].kind == uint8_t(kind) && evidence.seh_checks[i].status == uint8_t(status))
            return true;
    return false;
}

const run::TraceRecord* FindTrace(const run::RunEvidence& evidence, run::Role role, run::EventKind kind) {
    for (size_t i = 0; i < evidence.trace_count; ++i) {
        const run::TraceRecord* record = run::TraceForRole(role, i);
        if (record && record->event == uint8_t(kind)) return record;
    }
    return nullptr;
}

}  // namespace

int main() {
    if (!mem::Init()) {
        std::printf("FAIL: guest memory initializes\n");
        return 1;
    }
    Fixture fixture;
    std::string error;
    if (!PrepareFixture(fixture, error)) {
        std::printf("FAIL: fixture preparation: %s\n", error.c_str());
        return 1;
    }
    const mem::Stats allocation_baseline = mem::Query();
    {
        ResetData();
        const Started started = StartParent(fixture);
        Check(started.status == run::Status::kReady, "synthetic parent run is authorised");
        JoinThread(started.parent);
        const bool child_done = WaitForRole(run::Role::kChild);
        const bool parent_done = WaitForRole(run::Role::kParent);
        Check(child_done && parent_done, "parent and child reach their reviewed stops");
        run::RunEvidence parent, child;
        const bool parent_evidence = run::EvidenceForRole(run::Role::kParent, &parent);
        const bool child_evidence = run::EvidenceForRole(run::Role::kChild, &child);
        Check(parent_evidence && child_evidence, "both workers leave evidence");
        Check(parent.status == run::Status::kStopped && child.status == run::Status::kStopped,
              "both stops are recorded as kStopped");
        Check(parent.instructions == fixture::kParentExecutedInstructions, "parent retires exactly 19 instructions");
        Check(child.instructions == fixture::kChildExecutedInstructions, "child retires exactly the reviewed instruction count");
        Check(parent.stop_pc == fixture::kParentStop, "parent stop PC is the pre-instruction ret");
        Check(child.stop_pc == fixture::kChildStop && child.at_stop, "child stop PC is the pre-instruction start-routine entry");
        Check(child.seh_verified && child.seh_registration != 0, "child SEH record is live and verified at the stop");
        Check(child.tls_verified && child.tls != 0, "child TLS pointer is initialised and verified");
        bool seh_checks_ok = child.seh_check_count >= 14;
        for (size_t i = 0; i < child.seh_check_count; ++i) {
            const run::SehCheck& check = child.seh_checks[i];
            if (check.status == uint8_t(run::SehCheckStatus::kFail) ||
                check.status == uint8_t(run::SehCheckStatus::kUnknown))
                seh_checks_ok = false;
        }
        Check(seh_checks_ok, "every SEH predicate is recorded and passes on the normal synthetic frame");
        Check(parent.thread_id != child.thread_id && parent.claim_id && child.claim_id && parent.claim_id != child.claim_id,
              "parent and child have distinct worker and claim identities");
        Check(parent.origin == uint32_t(run::Origin::kSyntheticFixture) &&
                  child.origin == uint32_t(run::Origin::kSyntheticFixture),
              "the trace labels synthetic fixture execution, not authentic execution");
        Check(parent.kernel_calls == 2 && child.kernel_calls == 2, "both workers record two reviewed kernel calls");
        bool parent_kernel_ok = false, child_kernel_ok = false;
        for (size_t i = 0; i < parent.kernel_count; ++i) {
            const run::KernelObservation* o = run::KernelObservationForRole(run::Role::kParent, i);
            if (o && o->ordinal == 255 && o->result == 0) parent_kernel_ok = true;
        }
        bool child_leave_ok = false;
        for (size_t i = 0; i < child.kernel_count; ++i) {
            const run::KernelObservation* o = run::KernelObservationForRole(run::Role::kChild, i);
            if (o && o->ordinal == 277 && o->result == 0) child_kernel_ok = true;
            if (o && o->ordinal == 294 && o->result == 0) child_leave_ok = true;
        }
        Check(parent_kernel_ok, "ordinal-255 child creation returned success through the checked ABI");
        Check(child_kernel_ok, "ordinal-277 enter-critical-section returned success through the checked ABI");
        Check(child_leave_ok, "ordinal-294 leave-critical-section returned success through the checked ABI");
        uint32_t handle = 0, child_id = 0, create_status = 0xFFFFFFFF, close_status = 0xFFFFFFFF;
        mem::Read(fixture::kHandleOut, &handle, sizeof(handle));
        mem::Read(fixture::kIdOut, &child_id, sizeof(child_id));
        mem::Read(fixture::kStatusOut, &create_status, sizeof(create_status));
        mem::Read(fixture::kCloseStatusOut, &close_status, sizeof(close_status));
        Check(handle != 0 && child_id != 0 && create_status == 0 && close_status == 0,
              "the parent published the child handle/id, received success and closed the handle");
        uint32_t lock_count = 0, recursion = 0, owner = 0;
        mem::Read(fixture::kCriticalSectionAddress + 0x10, &lock_count, sizeof(lock_count));
        mem::Read(fixture::kCriticalSectionAddress + 0x14, &recursion, sizeof(recursion));
        mem::Read(fixture::kCriticalSectionAddress + 0x18, &owner, sizeof(owner));
        Check(lock_count == 0xFFFFFFFFu && recursion == 0 && owner == 0,
              "the checked critical-section enter/leave pair restores the free layout");
        uint32_t marker = 0;
        mem::Read(fixture::kHandlerMarker, &marker, sizeof(marker));
        Check(marker == 0, "the synthetic SEH handler never ran");
        // Trace shape: a worker start, instruction entries, a stop and a
        // finalize, all with guest-visible identities only.
        const run::TraceRecord* first = run::TraceForRole(run::Role::kChild, 0);
        const run::TraceRecord* last = run::TraceForRole(run::Role::kChild, child.trace_count - 1);
        Check(first && first->event == uint8_t(run::EventKind::kWorkerStart), "child trace starts at the worker start");
        Check(last && last->event == uint8_t(run::EventKind::kFinalize), "child trace ends at finalisation");
        bool stop_seen = false;
        for (size_t i = 0; i < child.trace_count; ++i) {
            const run::TraceRecord* r = run::TraceForRole(run::Role::kChild, i);
            if (r && r->event == uint8_t(run::EventKind::kStop) && r->pc == fixture::kChildStop) stop_seen = true;
        }
        Check(stop_seen, "child trace records the exact stop PC");
        Check(kernel::Threads().empty(), "both guest workers unregister after finalisation");
    }

    {
        ResetData();
        const Started started = StartParent(fixture, 10);
        Check(started.status == run::Status::kReady, "budget scenario starts");
        JoinThread(started.parent);
        WaitForRole(run::Role::kChild);
        run::RunEvidence child;
        Check(run::EvidenceForRole(run::Role::kChild, &child) && child.status == run::Status::kBudgetExceeded &&
                  child.instructions == 10,
              "a short child instruction budget stops before the over-budget instruction");
    }

    {
        ResetData();
        run::SetInjectionHookForTest(run::Role::kParent, 0x0011006, ForceBranchNotTaken);
        const Started started = StartParent(fixture);
        JoinThread(started.parent);
        WaitForRole(run::Role::kParent);
        run::RunEvidence parent;
        Check(started.status == run::Status::kReady && run::EvidenceForRole(run::Role::kParent, &parent) &&
                  parent.status == run::Status::kForbiddenBranch,
              "an unexpected conditional outcome is refused at the reviewed edge");
        run::ClearInjectionHooksForTest();
    }

    {
        ResetData();
        const uint32_t wrong = dispatch::KernelThunkAddress(258);
        mem::Write(fixture::kSlot255, &wrong, sizeof(wrong));
        const Started started = StartParent(fixture);
        JoinThread(started.parent);
        WaitForRole(run::Role::kParent);
        run::RunEvidence parent;
        Check(started.status == run::Status::kReady && run::EvidenceForRole(run::Role::kParent, &parent) &&
                  parent.status == run::Status::kKernelRejected,
              "an unreviewed kernel ordinal is rejected before any export runs");
        run::RunEvidence child;
        Check(!run::EvidenceForRole(run::Role::kChild, &child), "a rejected ordinal-255 edge creates no child worker");
    }

    {
        ResetData();
        run::SetInjectionHookForTest(run::Role::kChild, 0x001111F, FaultAtTlsWrite);
        const Started started = StartParent(fixture);
        JoinThread(started.parent);
        WaitForRole(run::Role::kChild);
        run::RunEvidence child;
        uint32_t marker = 0;
        mem::Read(fixture::kHandlerMarker, &marker, sizeof(marker));
        Check(run::EvidenceForRole(run::Role::kChild, &child) &&
                  child.status == run::Status::kMemoryViolation,
              "a memory fault while the SEH frame is live stops the child");
        Check(marker == 0, "an unexpected exception is not delivered to the retail SEH handler");
        run::ClearInjectionHooksForTest();
    }

    {
        ResetData();
        run::RouteManifest bad_child = ChildManifest();
        bad_child.tls_block_size = 0x90;
        const Started started = StartParent(fixture, fixture::kChildExecutedInstructions, &bad_child);
        JoinThread(started.parent);
        WaitForRole(run::Role::kParent);
        run::RunEvidence parent, child;
        uint32_t status = 0;
        mem::Read(fixture::kStatusOut, &status, sizeof(status));
        Check(run::EvidenceForRole(run::Role::kParent, &parent) && parent.status == run::Status::kStopped &&
                  status != 0,
              "a refused ordinal-255 admission lets the parent finish with the failure status");
        Check(!run::EvidenceForRole(run::Role::kChild, &child),
              "a refused ordinal-255 admission creates no child worker");
    }

    {
        ResetData();
        WaitForQuiescence();
        run::RouteManifest unsupported = UnsupportedManifest();
        // The unsupported route is a parent-only rehearsal: prepare a thread
        // directly and authorise the negative manifest.
        kernel::ThreadRef thread;
        const uint32_t status = kernel::PrepareSystemThreadEx(0, 0x10000, 0x94, 0, unsupported.entry, 0, false, 0, thread);
        Check(status == kernel::kSuccess && thread, "unsupported-route thread prepares");
        run::RunAuthorization authorization;
        const run::Status authorized = run::AuthorizeSyntheticRouteForThread(
            thread.get(), unsupported, fixture.sha1.c_str(), dispatch::MappingIdentity(),
            dispatch::CallLease().Generation(), mem::Generation(), 1, 8, 1, 2, &authorization);
        Check(authorized == run::Status::kReady, "unsupported-route authorisation is synthetic and bounded");
        if (authorized == run::Status::kReady) {
            const bool started_thread = kernel::StartThread(thread);
            Check(started_thread, "unsupported-route worker starts");
            JoinThread(thread);
            run::RunEvidence evidence;
            Check(run::EvidenceForRole(run::Role::kParent, &evidence) &&
                      evidence.status == run::Status::kUnsupportedInstruction,
                  "an unsupported checked instruction stops execution");
        }
    }


    {
        ResetData();
        run::AuthorizationPolicy policy;
        policy.origin = run::Origin::kAuthenticImage;
        policy.role = run::Role::kParent;
        policy.manifest = &run::AuthenticParentManifest();
        policy.child_manifest = &run::AuthenticChildManifest();
        policy.image_sha1 = kernel::kAuthenticImageSha1;
        policy.policy_identity = kernel::kAuthenticCallbackPolicyIdentity;
        policy.mapping_identity = dispatch::MappingIdentity();
        policy.dispatch_generation = dispatch::CallLease().Generation();
        policy.memory_generation = mem::Generation();
        policy.instruction_budget = 1;
        policy.transfer_budget = 1;
        policy.kernel_budget = 1;
        policy.maximum_call_depth = 1;
        run::RunAuthorization authorization;
        const run::Status authentic = run::AuthorizeAuthenticRun(run::Role::kParent, policy, &authorization);
        Check(authentic == run::Status::kAuthenticExecutionNotAuthorised ||
                  authentic == run::Status::kInvalidImage || authentic == run::Status::kInvalidMapping,
              "authentic execution authorisation is not granted");
        Check(!authorization && run::LiveAuthorizationCount() == 0,
              "the authentic refusal creates no claim");
    }

    {
        ResetData();
        run::ParentStartRequest request;
        request.manifest = &fixture.parent;
        request.child_manifest = &fixture.child;
        request.image_sha1 = "0000000000000000000000000000000000000000";
        request.mapping_identity = dispatch::MappingIdentity();
        request.dispatch_generation = dispatch::CallLease().Generation();
        request.memory_generation = mem::Generation();
        request.instruction_budget = fixture::kParentExecutedInstructions;
        request.transfer_budget = 8;
        request.kernel_budget = 1;
        request.maximum_call_depth = 2;
        request.child_instruction_budget = fixture::kChildExecutedInstructions;
        request.child_transfer_budget = 8;
        request.child_kernel_budget = 1;
        request.child_maximum_call_depth = 2;
        run::RunAuthorization authorization;
        Check(run::StartParentWorkerForTest(request, &authorization) == run::Status::kInvalidImage,
              "a mismatched image identity is refused before a worker starts");
        request.image_sha1 = fixture.sha1.c_str();
        request.mapping_identity = dispatch::MappingIdentity() ^ 1;
        Check(run::StartParentWorkerForTest(request, &authorization) == run::Status::kInvalidMapping,
              "a mismatched generated mapping identity is refused");
        request.mapping_identity = dispatch::MappingIdentity();
        request.dispatch_generation = dispatch::CallLease().Generation() ^ 1;
        Check(run::StartParentWorkerForTest(request, &authorization) == run::Status::kStaleDispatch,
              "a stale dispatch generation is refused");
    }

    {
        ResetData();
        run::RouteManifest ret = RetImmManifest();
        const RouteRun run = RunSyntheticRoute(fixture, ret, 4, 8, 1, 2);
        Check(run.start == run::Status::kReady && run.have && run.evidence.status == run::Status::kStopped &&
                  run.evidence.instructions == fixture::kRetImmExecutedInstructions &&
                  run.evidence.stop_pc == fixture::kRetImmStop &&
                  run.evidence.at_stop,
              "RET imm16 callee returns through the checked frame and the caller stops at the reviewed boundary");
        const run::TraceRecord* start = FindTrace(run.evidence, run::Role::kParent, run::EventKind::kWorkerStart);
        const run::TraceRecord* stop = FindTrace(run.evidence, run::Role::kParent, run::EventKind::kStop);
        Check(start && stop && start->esp == stop->esp,
              "RET imm16 restores ESP exactly after the caller push, call return address and clean-up");
    }
    {
        ResetData();
        run::SetInjectionHookForTest(run::Role::kParent, 0x0011412, CorruptReturnAddress);
        const run::RouteManifest ret = RetImmManifest();
        const RouteRun run = RunSyntheticRoute(fixture, ret, 4, 8, 1, 2);
        run::ClearInjectionHooksForTest();
        Check(run.have && run.evidence.status == run::Status::kUnreviewedTransfer,
              "a modified return address is refused before the RET transfer can be taken");
    }
    {
        ResetData();
        std::array<route::Step, 4> misclassified{};
        std::copy(std::begin(fixture::kRetImmRoute), std::end(fixture::kRetImmRoute), misclassified.begin());
        misclassified[3].kind = route::StepKind::kSequential;
        run::RouteManifest wrong;
        wrong.steps = misclassified.data();
        wrong.count = misclassified.size();
        wrong.entry = fixture::kRetImmEntry;
        wrong.stop = fixture::kRetImmStop;
        wrong.executed_instructions = fixture::kRetImmExecutedInstructions;
        wrong.identity = run::ManifestIdentity(wrong);
        const RouteRun run = RunSyntheticRoute(fixture, wrong, 4, 8, 1, 2);
        Check(run.have && run.evidence.status == run::Status::kUnreviewedTransfer,
              "a RET classified as sequential is refused at the return boundary");
    }
    {
        struct Case {
            const char* name;
            void (*hook)(X86Context&);
            run::SehCheckKind kind;
        };
        const Case cases[] = {
            {"scope flag", CorruptScopeFlag, run::SehCheckKind::kScopeFlag},
            {"handler", CorruptHandler, run::SehCheckKind::kHandler},
            {"previous record", CorruptPrevious, run::SehCheckKind::kPreviousRecord},
            {"scope table", CorruptScopeTable, run::SehCheckKind::kScopeTable},
            {"saved ESP", CorruptSavedEsp, run::SehCheckKind::kSavedEsp},
            {"exception chain", DetachExceptionChain, run::SehCheckKind::kChain},
        };
        for (const Case& current : cases) {
            const ChildRun run = RunWithChildHooks(fixture, [&] {
                run::SetInjectionHookForTest(run::Role::kChild, 0x0011113, current.hook);
            });
            Check(run.have_child && run.child.status == run::Status::kSehViolation &&
                      HasSehCheck(run.child, current.kind, run::SehCheckStatus::kFail),
                  current.name);
        }
        const ChildRun caller = RunWithChildHooks(fixture, [] {
            run::SetInjectionHookForTest(run::Role::kChild, 0x001110A, SetCallerEbp);
            run::SetInjectionHookForTest(run::Role::kChild, 0x0011113, CorruptCallerSlot);
        });
        Check(caller.have_child && caller.child.status == run::Status::kSehViolation &&
                  HasSehCheck(caller.child, run::SehCheckKind::kCallerEbpHeld, run::SehCheckStatus::kFail),
              "caller EBP");
    }
    {
        ResetData();
        const uint32_t initial_handle = 0x11111111u, initial_id = 0x22222222u;
        mem::Write(fixture::kHandleOut, &initial_handle, sizeof(initial_handle));
        mem::Write(fixture::kIdOut, &initial_id, sizeof(initial_id));
        kernel::ThreadRef live;
        const uint32_t prepared = kernel::PrepareSystemThreadEx(
            fixture::kHandleOut, 0x10000, 0x94, fixture::kIdOut, fixture::kChildStop, 0, false,
            fixture::kChildEntry, live);
        uint32_t handle = 0, id = 0;
        mem::Read(fixture::kHandleOut, &handle, sizeof(handle));
        mem::Read(fixture::kIdOut, &id, sizeof(id));
        Check(prepared == kernel::kSuccess && handle != 0 && id != 0 &&
                  kernel::FromHandle(handle).get() == live.get(),
              "a live synthetic child publishes its handle and id");
        const mem::Stats before = mem::Query();
        kernel::AuthenticSystemThreadRequest request;
        request.handle_out = fixture::kHandleOut;
        request.stack_size = 0x10000;
        request.tls_size = 0x94;
        request.thread_id_out = fixture::kIdOut;
        request.start_routine = fixture::kChildStop;
        request.start_context = 0;
        request.system_routine = fixture::kChildEntry;
        request.call_pc = 0x001103A;
        request.argument_count = 10;
        request.policy_identity = "synthetic-assessment";
        request.dispatch_generation = dispatch::CallLease().Generation();
        request.memory_generation = mem::Generation();
        const kernel::SystemThreadAssessmentContract contract{
            fixture.sha1.c_str(), fixture::kChildEntry, fixture::kChildStop, 0x001103A, 10, 0x94,
            "synthetic-assessment",
        };
        for (int repeat = 0; repeat < 5; ++repeat) {
            const kernel::AuthenticAdmissionResult assessed = kernel::AssessSystemThreadRequest(request, contract);
            uint32_t observed_handle = 0, observed_id = 0;
            mem::Read(fixture::kHandleOut, &observed_handle, sizeof(observed_handle));
            mem::Read(fixture::kIdOut, &observed_id, sizeof(observed_id));
            Check(assessed.status == kernel::AuthenticAdmissionStatus::kValidatedButExecutionProhibited &&
                      observed_handle == handle && observed_id == id,
                  "assessment is repeatable and leaves the live handle and id unchanged");
        }
        Check(mem::Query().system_used == before.system_used &&
                  mem::Query().contiguous_used == before.contiguous_used &&
                  kernel::Threads().empty(),
              "assessment candidates release every allocation and registration");
        kernel::AuthenticSystemThreadRequest wrong = request;
        wrong.call_pc = 0x001103B;
        Check(kernel::AssessSystemThreadRequest(wrong, contract).status ==
                  kernel::AuthenticAdmissionStatus::kInvalidCallSite,
              "assessment rejects a wrong call site");
        wrong = request;
        wrong.tls_size = 0x90;
        Check(kernel::AssessSystemThreadRequest(wrong, contract).status ==
                  kernel::AuthenticAdmissionStatus::kRejected,
              "assessment rejects an invalid TLS block");
        wrong = request;
        wrong.policy_identity = "wrong-policy";
        Check(kernel::AssessSystemThreadRequest(wrong, contract).status ==
                  kernel::AuthenticAdmissionStatus::kInvalidPolicyIdentity,
              "assessment rejects a wrong policy identity");
        kernel::SystemThreadAssessmentContract wrong_image = contract;
        wrong_image.image_sha1 = "0000000000000000000000000000000000000000";
        Check(kernel::AssessSystemThreadRequest(request, wrong_image).status ==
                  kernel::AuthenticAdmissionStatus::kInvalidImage,
              "assessment rejects a wrong image identity");
        mem::Read(fixture::kHandleOut, &handle, sizeof(handle));
        mem::Read(fixture::kIdOut, &id, sizeof(id));
        Check(handle != 0 && id != 0 && kernel::FromHandle(handle).get() == live.get(),
              "rejected assessments do not alter the live child");
        Check(kernel::CloseHandle(handle) && !kernel::FromHandle(handle),
              "the live child's handle closes through the ordinary checked path");
        Check(kernel::CancelPreparedThread(live), "the live prepared child cancels after handle close");
        uint32_t cleared_handle = 1, cleared_id = 1;
        mem::Read(fixture::kHandleOut, &cleared_handle, sizeof(cleared_handle));
        mem::Read(fixture::kIdOut, &cleared_id, sizeof(cleared_id));
        Check(cleared_handle == 0 && cleared_id == 0,
              "cancellation clears the live child's published outputs after the close");

        const uint32_t zero = 0;
        mem::Write(fixture::kHandleOut, &zero, sizeof(zero));
        mem::Write(fixture::kIdOut, &zero, sizeof(zero));
        const kernel::AuthenticAdmissionResult assessed = kernel::AssessSystemThreadRequest(request, contract);
        uint32_t after_assessment_handle = 1, after_assessment_id = 1;
        mem::Read(fixture::kHandleOut, &after_assessment_handle, sizeof(after_assessment_handle));
        mem::Read(fixture::kIdOut, &after_assessment_id, sizeof(after_assessment_id));
        Check(assessed.status == kernel::AuthenticAdmissionStatus::kValidatedButExecutionProhibited &&
                  after_assessment_handle == 0 && after_assessment_id == 0,
              "assessment leaves an unwritten handle and id output for the real creation");
        kernel::ThreadRef actual;
        const uint32_t created = kernel::PrepareSystemThreadEx(
            fixture::kHandleOut, 0x10000, 0x94, fixture::kIdOut, fixture::kChildStop, 0, false,
            fixture::kChildEntry, actual);
        uint32_t actual_handle = 0, actual_id = 0;
        mem::Read(fixture::kHandleOut, &actual_handle, sizeof(actual_handle));
        mem::Read(fixture::kIdOut, &actual_id, sizeof(actual_id));
        Check(created == kernel::kSuccess && actual_handle != 0 && actual_id != 0,
              "creation after assessment publishes the real handle and id");
        Check(kernel::CloseHandle(actual_handle) && !kernel::FromHandle(actual_handle),
              "the created child's handle closes normally after assessment");
        Check(kernel::CancelPreparedThread(actual), "the created child cancels after its handle close");
        mem::Read(fixture::kHandleOut, &actual_handle, sizeof(actual_handle));
        mem::Read(fixture::kIdOut, &actual_id, sizeof(actual_id));
        Check(actual_handle == 0 && actual_id == 0,
              "the created child's outputs clear on cancellation");
    }
    {
        ResetData();
        g_stack_probe = false;
        g_stack_mutation_started = false;
        g_stack_mutation_done = false;
        g_stack_mutation_status = -1;
        run::SetInjectionHookForTest(run::Role::kParent, 0x0011040, HoldForStackMutation);
        const Started started = StartParent(fixture);
        const uint32_t stack_page = (started.parent->stack_top - 1) & ~(mem::kPage - 1);
        std::thread mutator([&] {
            while (!g_stack_probe.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            g_stack_mutation_started = true;
            g_stack_mutation_status = mem::Decommit(stack_page, mem::kPage) ? 1 : 0;
            g_stack_mutation_done = true;
        });
        JoinThread(started.parent);
        WaitForRole(run::Role::kChild);
        WaitForRole(run::Role::kParent);
        mutator.join();
        run::ClearInjectionHooksForTest();
        run::RunEvidence parent, child;
        const bool have_parent = run::EvidenceForRole(run::Role::kParent, &parent);
        const bool have_child = run::EvidenceForRole(run::Role::kChild, &child);
        Check(started.parent->resources_released && g_stack_mutation_done.load(),
              "a concurrent stack decommit completes only after the worker finalises");
        Check(have_parent && parent.status == run::Status::kStopped && have_child &&
                  child.status == run::Status::kStopped,
              "the stack mutation does not disturb either reviewed stop");
        std::printf("stack mutation decommit=%d\n", g_stack_mutation_status.load());
    }
    {
        const mem::Stats final = mem::Query();
        Check(final.system_used == allocation_baseline.system_used &&
                  final.contiguous_used == allocation_baseline.contiguous_used &&
                  final.virtual_committed == allocation_baseline.virtual_committed,
              "all synthetic runs, assessments and cancellations restore the allocation baseline");
    }

    Check(kernel::StopKernelWorkers(), "the harness worker gate closes");
    {
        ResetData();
        run::ParentStartRequest request;
        request.manifest = &fixture.parent;
        request.child_manifest = &fixture.child;
        request.image_sha1 = fixture.sha1.c_str();
        request.mapping_identity = dispatch::MappingIdentity();
        request.dispatch_generation = dispatch::CallLease().Generation();
        request.memory_generation = mem::Generation();
        request.instruction_budget = fixture::kParentExecutedInstructions;
        request.transfer_budget = 8;
        request.kernel_budget = 1;
        request.maximum_call_depth = 2;
        request.child_instruction_budget = fixture::kChildExecutedInstructions;
        request.child_transfer_budget = 8;
        request.child_kernel_budget = 1;
        request.child_maximum_call_depth = 2;
        run::RunAuthorization authorization;
        Check(run::StartParentWorkerForTest(request, &authorization) == run::Status::kShutdown,
              "no run can be authorised after worker shutdown");
    }
    mem::Shutdown();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
