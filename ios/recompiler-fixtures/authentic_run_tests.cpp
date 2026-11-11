// Phase 2D-19 synthetic rehearsal for the authentic-run harness. Every test
// below drives generated synthetic guest code through the same gate, claim,
// budget, SEH/TLS, kernel-call and cancellation paths the authentic experiment
// would use. No authentic instruction, address or image is executed.

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
void CancelAfterChildStart(X86Context&) { kernel::authentic::CancelAllRuns(); }
void CancelAtChildEntry(X86Context&) { kernel::authentic::CancelAllRuns(); }
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
        Check(child.instructions == fixture::kChildExecutedInstructions, "child retires exactly 33 instructions");
        Check(parent.stop_pc == fixture::kParentStop, "parent stop PC is the pre-instruction ret");
        Check(child.stop_pc == fixture::kChildStop && child.at_stop, "child stop PC is the pre-instruction start-routine entry");
        Check(child.seh_verified && child.seh_registration != 0, "child SEH record is live and verified at the stop");
        Check(child.tls_verified && child.tls != 0, "child TLS pointer is initialised and verified");
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
        run::SetInjectionHookForTest(run::Role::kChild, 0x001111B, FaultAtTlsWrite);
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
