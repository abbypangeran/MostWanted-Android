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
#include <functional>
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
#include "kernel/kernel_objects.h"
#include "kernel/memory.h"
#include "xbe/image_loader.h"
#include "xbe/xbe.h"
#include "x86_recomp_shared.h"

#include "authentic_run_fixture_route.h"
#include "authentic_run_full_route.h"

namespace run = kernel::authentic;
namespace route = kernel::authentic::route;
namespace fixture = authentic_run_fixture;
namespace full = authentic_run_full_fixture;

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
    manifest.start_routine = fixture::kChildStart;
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

run::RouteManifest FullParentManifest() {
    run::RouteManifest manifest;
    manifest.steps = full::kParentRoute;
    manifest.count = full::kParentRouteCount;
    manifest.entry = full::kParentEntry;
    manifest.stop = full::kParentStop;
    manifest.executed_instructions = full::kParentExecutedInstructions;
    manifest.required_transfers = full::kParentRequiredTransfers;
    manifest.trap_entries = full::kParentTrapEntries;
    manifest.trap_count = 1;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

run::RouteManifest FullChildManifest() {
    run::RouteManifest manifest;
    manifest.steps = full::kChildRoute;
    manifest.count = full::kChildRouteCount;
    manifest.entry = full::kChildEntry;
    manifest.stop = full::kChildStop;
    manifest.executed_instructions = full::kChildExecutedInstructions;
    manifest.required_transfers = full::kChildRequiredTransfers;
    manifest.trap_entries = full::kChildTrapEntries;
    manifest.trap_count = 2;
    manifest.seh_call = full::kSehCall;
    manifest.seh_entry = full::kSehEntry;
    manifest.seh_handler = full::kSehHandler;
    manifest.seh_scope_table = full::kSehScopeTable;
    manifest.seh_scope_bytes = full::kSehScopeBytes;
    manifest.seh_scope_flag = full::kSehScopeFlag;
    manifest.tls_block_size = full::kTlsBlockSize;
    manifest.critical_section_object = full::kCriticalSectionObject;
    manifest.start_routine = full::kChildStartRoutine;
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

run::RouteManifest ChildContinuationManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kChildContinuationRoute;
    manifest.count = fixture::kChildContinuationCount;
    manifest.entry = fixture::kChildEntry;
    manifest.stop = fixture::kChildContinuationStop;
    manifest.executed_instructions = fixture::kChildContinuationExecuted;
    manifest.start_routine = fixture::kChildStart;
    manifest.trap_entries = fixture::kChildContinuationTrapEntries;
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

run::RouteManifest VersionCallerManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kVersionCallerRoute;
    manifest.count = fixture::kVersionCallerCount;
    manifest.entry = fixture::kVersionCallerEntry;
    manifest.stop = fixture::kVersionCallerStop;
    manifest.executed_instructions = fixture::kVersionCallerExecuted;
    manifest.trap_entries = fixture::kVersionCallerTrapEntries;
    manifest.trap_count = 2;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

run::RouteManifest CliManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kCliRoute;
    manifest.count = fixture::kCliCount;
    manifest.entry = fixture::kCliEntry;
    manifest.stop = fixture::kCliStop;
    manifest.executed_instructions = fixture::kCliExecuted;
    manifest.trap_entries = fixture::kCliTrapEntries;
    manifest.trap_count = 1;
    manifest.identity = run::ManifestIdentity(manifest);
    return manifest;
}

run::RouteManifest StiManifest() {
    run::RouteManifest manifest;
    manifest.steps = fixture::kStiRoute;
    manifest.count = fixture::kStiCount;
    manifest.entry = fixture::kStiEntry;
    manifest.stop = fixture::kStiStop;
    manifest.executed_instructions = fixture::kStiExecuted;
    manifest.trap_entries = fixture::kStiTrapEntries;
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
    run::RouteManifest full_parent;
    run::RouteManifest full_child;
    uint64_t mapping_identity = 0;
    uint32_t mapping_count = 0;
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
    if (mem::Protect(full::kParentEntry & ~(mem::kPage - 1), 0x4000, mem::kRead | mem::kExecute) !=
        mem::AccessStatus::kOk) {
        error = "full synthetic code pages are not executable";
        return false;
    }
    if (!dispatch::Init(error)) return false;
    fixture.mapping_identity = dispatch::MappingIdentity();
    fixture.mapping_count = dispatch::MappingCount();
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
    if (!mem::Commit(full::kDataPage, full::kDataPages * mem::kPage)) {
        error = "full synthetic data pages did not commit";
        return false;
    }
    if (mem::Protect(full::kDataPage, full::kDataPages * mem::kPage, mem::kRead | mem::kWrite) !=
        mem::AccessStatus::kOk) {
        error = "full synthetic data pages are not writable";
        return false;
    }
    // The image is executable; the synthetic data page is not.
    if (mem::Protect(fixture::kDataPage, 2 * mem::kPage, mem::kRead | mem::kWrite) != mem::AccessStatus::kOk) {
        error = "synthetic data pages are not writable";
        return false;
    }
    fixture.parent = ParentManifest();
    fixture.child = ChildManifest();
    fixture.full_parent = FullParentManifest();
    fixture.full_child = FullChildManifest();
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

void ResetFullData() {
    kernel::ClearShutdownRegistrationsForTest();
    std::array<uint8_t, full::kDataPages * mem::kPage> zero{};
    mem::Write(full::kDataPage, zero.data(), zero.size());
    const uint32_t terminator = 0xFFFFFFFFu;
    mem::Write(full::kScopeTable, &terminator, sizeof(terminator));
    const uint32_t header = 0x00040001u, zero32 = 0, self = full::kCriticalSectionAddress + 8;
    const uint32_t free_count = 0xFFFFFFFFu;
    mem::Write(full::kCriticalSectionAddress + 0x00, &header, sizeof(header));
    mem::Write(full::kCriticalSectionAddress + 0x04, &zero32, sizeof(zero32));
    mem::Write(full::kCriticalSectionAddress + 0x08, &self, sizeof(self));
    mem::Write(full::kCriticalSectionAddress + 0x0C, &self, sizeof(self));
    mem::Write(full::kCriticalSectionAddress + 0x10, &free_count, sizeof(free_count));
    mem::Write(full::kCriticalSectionAddress + 0x14, &zero32, sizeof(zero32));
    mem::Write(full::kCriticalSectionAddress + 0x18, &zero32, sizeof(zero32));
    // The shutdown registration object the synthetic route registers: the
    // callback is the synthetic handler (never invoked).
    const uint32_t registration[4] = {full::kSehHandler, 0x80000000u, 0, 0};
    mem::Write(full::kShutdownObject, registration, sizeof(registration));
    // The synthetic hardware/version data the probe reads through the data
    // import slots.
    const uint32_t hardware = full::kDataPage + 0x500;
    const uint32_t version = full::kDataPage + 0x510;
    const uint32_t flags = 0x20u, build = 5838u;
    mem::Write(full::kHardwareSlot, &hardware, sizeof(hardware));
    mem::Write(full::kVersionSlot, &version, sizeof(version));
    mem::Write(hardware, &flags, sizeof(flags));
    mem::Write(version + 4, &build, sizeof(build));
    const uint32_t dirty = 0xFFFFFFFFu;
    mem::Write(full::kDebe8, &dirty, sizeof(dirty));
    mem::Write(full::kDebec, &dirty, sizeof(dirty));
    for (uint32_t ordinal : {255u, 187u, 277u, 294u, 47u, 107u, 113u}) {
        const uint32_t thunk = dispatch::KernelThunkAddress(ordinal);
        mem::Write(full::SlotFor(ordinal), &thunk, sizeof(thunk));
    }
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
                evidence.status == run::Status::kUnreviewedTransfer ||
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

Started StartFullParent(const Fixture& fixture, uint32_t parent_transfer = full::kParentRequiredTransfers,
                        uint32_t child_transfer = full::kChildRequiredTransfers,
                        uint32_t parent_instructions = full::kParentExecutedInstructions,
                        uint32_t child_instructions = full::kChildExecutedInstructions,
                        const run::RouteManifest* child = nullptr,
                        const run::RouteManifest* parent = nullptr) {
    WaitForQuiescence();
    Started started;
    run::ParentStartRequest request;
    request.manifest = parent ? parent : &fixture.full_parent;
    request.child_manifest = child ? child : &fixture.full_child;
    request.image_sha1 = fixture.sha1.c_str();
    request.mapping_identity = dispatch::MappingIdentity();
    request.dispatch_generation = dispatch::CallLease().Generation();
    request.memory_generation = mem::Generation();
    request.stack_size = 0x10000;
    request.tls_size = 0x94;
    request.instruction_budget = parent_instructions;
    request.transfer_budget = parent_transfer;
    request.kernel_budget = 8;
    request.maximum_call_depth = 8;
    request.child_instruction_budget = child_instructions;
    request.child_transfer_budget = child_transfer;
    request.child_kernel_budget = 8;
    request.child_maximum_call_depth = 8;
    request.prepared_thread = &started.parent;
    started.status = run::StartParentWorkerForTest(request, &started.authorization);
    return started;
}

struct FullRun {
    Started started;
    run::RunEvidence parent;
    run::RunEvidence child;
    bool have_parent = false;
    bool have_child = false;
};

FullRun RunFullRoute(const Fixture& fixture, uint32_t parent_transfer = full::kParentRequiredTransfers,
                     uint32_t child_transfer = full::kChildRequiredTransfers,
                     uint32_t parent_instructions = full::kParentExecutedInstructions,
                     uint32_t child_instructions = full::kChildExecutedInstructions,
                     const run::RouteManifest* child = nullptr,
                     const run::RouteManifest* parent = nullptr) {
    ResetFullData();
    FullRun result;
    result.started = StartFullParent(fixture, parent_transfer, child_transfer, parent_instructions,
                                     child_instructions, child, parent);
    if (result.started.status != run::Status::kReady) return result;
    JoinThread(result.started.parent);
    WaitForRole(run::Role::kChild);
    WaitForRole(run::Role::kParent);
    result.have_parent = run::EvidenceForRole(run::Role::kParent, &result.parent);
    result.have_child = run::EvidenceForRole(run::Role::kChild, &result.child);
    return result;
}

const run::KernelObservation* FindKernel(const run::RunEvidence& evidence, run::Role role, uint32_t ordinal) {
    for (size_t i = 0; i < evidence.kernel_count; ++i) {
        const run::KernelObservation* observation = run::KernelObservationForRole(role, i);
        if (observation && observation->ordinal == ordinal) return observation;
    }
    return nullptr;
}

bool TraceHasInstructionAt(const run::RunEvidence& evidence, run::Role role, uint32_t pc) {
    for (size_t i = 0; i < evidence.trace_count; ++i) {
        const run::TraceRecord* record = run::TraceForRole(role, i);
        if (record && record->event == uint8_t(run::EventKind::kInstructionEntry) && record->pc == pc) return true;
    }
    return false;
}

bool AcceptFullRoute(const Fixture& fixture, const FullRun& run, const run::RouteManifest& parent,
                     const run::RouteManifest& child, std::string& error) {
    auto fail = [&error](const char* text) {
        error = text;
        return false;
    };
    if (run.started.status != run::Status::kReady || !run.have_parent || !run.have_child)
        return fail("workers did not complete");
    if (dispatch::MappingIdentity() != fixture.mapping_identity ||
        dispatch::MappingCount() != fixture.mapping_count)
        return fail("runtime mapping identity changed");
    if (parent.identity != fixture.full_parent.identity || child.identity != fixture.full_child.identity)
        return fail("manifest identity mismatch");
    if (!run::ManifestTransfersConsistent(parent) || !run::ManifestTransfersConsistent(child))
        return fail("manifest transfer requirement is inconsistent");
    if (parent.required_transfers != route::kParentRequiredTransfers ||
        child.required_transfers != route::kChildRequiredTransfers)
        return fail("manifest transfer requirement differs from the reviewed route");
    if (run.parent.status != run::Status::kStopped ||
        run.parent.instructions != parent.executed_instructions ||
        run.parent.transfers != parent.required_transfers || run.parent.required_transfers != parent.required_transfers ||
        run.parent.stop_pc != parent.stop || !run.parent.at_stop)
        return fail("parent route accounting");
    if (run.child.status != run::Status::kStopped ||
        run.child.instructions != child.executed_instructions ||
        run.child.transfers != child.required_transfers || run.child.required_transfers != child.required_transfers ||
        run.child.stop_pc != child.stop || !run.child.at_stop)
        return fail("child route accounting");
    auto accept_kernels = [&run](const run::RouteManifest& manifest, run::Role role) {
        size_t index = 0;
        for (size_t i = 0; i < manifest.count; ++i) {
            if (manifest.steps[i].kind != route::StepKind::kKernelCall) continue;
            const run::KernelObservation* observation = run::KernelObservationForRole(role, index++);
            if (!observation || observation->ordinal != manifest.steps[i].ordinal ||
                observation->result != 0 || observation->status != run::Status::kReady)
                return false;
        }
        const run::RunEvidence& evidence = role == run::Role::kParent ? run.parent : run.child;
        return index == evidence.kernel_count;
    };
    if (!accept_kernels(parent, run::Role::kParent) || !accept_kernels(child, run::Role::kChild))
        return fail("kernel ordinal admission");
    if (!run.child.seh_verified || run.child.seh_check_count != 14)
        return fail("SEH evidence");
    for (size_t i = 0; i < run.child.seh_check_count; ++i) {
        const run::SehCheckStatus status = run::SehCheckStatus(run.child.seh_checks[i].status);
        if (status == run::SehCheckStatus::kFail || status == run::SehCheckStatus::kUnknown)
            return fail("SEH predicate");
    }
    const run::TlsEvidence& tls = run.child.tls_evidence;
    if (tls.status != run::TlsEvidenceStatus::kValid || !tls.pointer_read_ok || !tls.block_read_ok ||
        !tls.hash_recorded || tls.address != run.child.tls ||
        tls.expected_pointer != run.child.tls + 4 || tls.self_pointer != run.child.tls + 4 ||
        tls.block_size != child.tls_block_size || tls.content_hash == 0)
        return fail("TLS evidence");
    if (run.parent.tls_evidence.status != run::TlsEvidenceStatus::kNotEvaluated)
        return fail("parent TLS evidence");
    if (!kernel::Threads().empty() || run::LiveAuthorizationCount() != 0)
        return fail("worker cleanup");
    if (TraceHasInstructionAt(run.parent, run::Role::kParent, parent.stop) ||
        TraceHasInstructionAt(run.child, run::Role::kChild, child.stop))
        return fail("terminal stop instruction entered");
    return true;
}

}  // namespace

// Injection hooks fire at the entry of the named reviewed instruction, before
// it executes. They are the deterministic failure injection for the tests.
namespace {
void ForceBranchNotTaken(X86Context& ctx) { ctx.ecx = 0; }
void FaultAtTlsWrite(X86Context& ctx) { ctx.edx = 0x00080000; }
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

std::atomic<uint32_t> g_tls_reference_address{0};
std::atomic<uint32_t> g_tls_reference_self{0};
std::atomic<uint32_t> g_tls_reference_size{0};
std::atomic<uint32_t> g_tls_reference_hash{0};
std::atomic<bool> g_tls_reference_read_ok{false};

uint32_t TlsHash(const uint8_t* bytes, uint32_t size) {
    uint32_t hash = 0x811C9DC5u;
    for (uint32_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x01000193u;
    }
    return hash;
}

uint32_t ZeroTlsHash(uint32_t size) {
    uint32_t hash = 0x811C9DC5u;
    for (uint32_t i = 0; i < size; ++i) {
        hash ^= 0;
        hash *= 0x01000193u;
    }
    return hash;
}

// Independent read-only reference captured before the final child instruction
// executes. The kernel call that follows does not change the TLS block.
void CaptureTlsReference(X86Context&) {
    kernel::Thread* thread = kernel::Current();
    const uint32_t address = thread ? thread->tls : 0;
    const uint32_t size = thread && thread->tls ? thread->stack_top - thread->tls : 0;
    std::array<uint8_t, 0x1000> block{};
    uint32_t pointer = 0;
    bool ok = address && size && size <= block.size() &&
              mem::Read(address, &pointer, sizeof(pointer)) == mem::AccessStatus::kOk &&
              mem::Read(address, block.data(), size) == mem::AccessStatus::kOk;
    g_tls_reference_address = address;
    g_tls_reference_size = size;
    g_tls_reference_self = ok ? pointer : 0;
    g_tls_reference_hash = ok ? TlsHash(block.data(), size) : 0;
    g_tls_reference_read_ok = ok;
}

// Test-only negative injections. They mutate the thread's TLS address or the
// TLS self-pointer immediately before the stop verification.
void CorruptTlsAddress(X86Context&) {
    if (kernel::Thread* thread = kernel::Current()) thread->tls = 0x00080000;
}
void ClearTlsAddress(X86Context&) {
    if (kernel::Thread* thread = kernel::Current()) thread->tls = 0;
}
void CorruptTlsPointer(X86Context&) {
    kernel::Thread* thread = kernel::Current();
    if (!thread || !thread->tls) return;
    const uint32_t value = 0xDEADBEEFu;
    mem::Write(thread->tls, &value, sizeof(value));
}

struct RouteRun {
    kernel::ThreadRef thread;
    run::RunAuthorization authorization;
    run::Status start = run::Status::kInternalError;
    bool have = false;
    run::RunEvidence evidence;
};

RouteRun RunSyntheticRoute(const Fixture& fixture, const run::RouteManifest& manifest, uint32_t instruction_budget,
                           uint32_t transfer_budget, uint32_t kernel_budget, uint32_t maximum_depth,
                           const std::function<void(kernel::Thread&)>& setup = {}) {
    WaitForQuiescence();
    RouteRun result;
    if (kernel::PrepareSystemThreadEx(0, 0x10000, 0x94, 0, manifest.entry, 0, false, 0, result.thread) !=
        kernel::kSuccess)
        return result;
    if (setup) setup(*result.thread);
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
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
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
    auto read_guest32 = [](uint32_t address) {
        uint32_t value = 0;
        mem::Read(address, &value, sizeof(value));
        return value;
    };
    auto set_version_data = [](uint32_t build, uint32_t physical, uint32_t init) {
        const uint32_t pointer = fixture::kVersionStruct;
        mem::Write(fixture::kVersionPointer, &pointer, sizeof(pointer));
        mem::Write(fixture::kVersionBuild, &build, sizeof(build));
        const uint32_t zero = 0;
        mem::Write(fixture::kVersionMarker, &zero, sizeof(zero));
        mem::Write(fixture::kPhysicalMarker, &zero, sizeof(zero));
        mem::Write(fixture::kInitMarker, &zero, sizeof(zero));
        mem::Write(0x8001003C, &physical, sizeof(physical));
        mem::Write(0x8000FFF0, &init, sizeof(init));
    };
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

    // Phase 2D-27B: manifest-derived transfer budgets and the complete
    // synthetic 224-instruction rehearsal. No authentic instruction runs.
    {
        bool same_shape = full::kParentRouteCount == route::kParentRouteCount &&
                          full::kChildRouteCount == route::kChildRouteCount &&
                          full::kTotalExecutedInstructions == route::kTotalExecutedInstructions &&
                          full::kTotalBoundarySteps == route::kTotalBoundarySteps;
        for (size_t i = 0; same_shape && i < route::kParentRouteCount; ++i) {
            const route::Step& reviewed = route::kParentRoute[i];
            const route::Step& mirrored = full::kParentRoute[i];
            same_shape = reviewed.kind == mirrored.kind && reviewed.ordinal == mirrored.ordinal &&
                         reviewed.arguments == mirrored.arguments && reviewed.taken == mirrored.taken &&
                         reviewed.segment == mirrored.segment && reviewed.ret_pop_bytes == mirrored.ret_pop_bytes;
        }
        for (size_t i = 0; same_shape && i < route::kChildRouteCount; ++i) {
            const route::Step& reviewed = route::kChildRoute[i];
            const route::Step& mirrored = full::kChildRoute[i];
            same_shape = reviewed.kind == mirrored.kind && reviewed.ordinal == mirrored.ordinal &&
                         reviewed.arguments == mirrored.arguments && reviewed.taken == mirrored.taken &&
                         reviewed.segment == mirrored.segment && reviewed.ret_pop_bytes == mirrored.ret_pop_bytes;
        }
        Check(same_shape, "the synthetic mirror preserves the reviewed step classes and kernel services");
        Check(full::kParentRequiredTransfers == route::kParentRequiredTransfers &&
                  full::kChildRequiredTransfers == route::kChildRequiredTransfers &&
                  full::kParentRequiredTransfers == 73 && full::kChildRequiredTransfers == 156,
              "the mirror derives the reviewed 73/156 transfer requirement");
        Check(full::kTotalExecutedInstructions == 224 && full::kTotalBoundarySteps == 226,
              "the mirror keeps 224 retired instructions and 226 boundary steps");
    }

    {
        const run::RouteManifest parent = FullParentManifest();
        const run::RouteManifest child = FullChildManifest();
        Check(run::TransferBudgetSatisfies(parent, route::kParentRequiredTransfers) &&
                  !run::TransferBudgetSatisfies(parent, route::kParentRequiredTransfers - 1) &&
                  run::TransferBudgetSatisfies(child, route::kChildRequiredTransfers) &&
                  !run::TransferBudgetSatisfies(child, route::kChildRequiredTransfers - 1) &&
                  !run::TransferBudgetSatisfies(child, 128),
              "the budget predicate accepts exactly the manifest-derived totals");
        Check(run::ManifestTransfersConsistent(parent) && run::ManifestTransfersConsistent(child),
              "the full manifests declare their exact derived transfer totals");
        run::RouteManifest understated = child;
        understated.required_transfers = route::kChildRequiredTransfers - 1;
        Check(!run::ManifestTransfersConsistent(understated),
              "an understated required_transfers is detected independently of the budget");
    }

    {
        const Started short_child = StartFullParent(fixture, 73, 128);
        Check(short_child.status == run::Status::kInvalidPolicy && !short_child.parent,
              "a child transfer budget of 128 is rejected before execution");
        const Started terminal_child = StartFullParent(fixture, 73, route::kChildRequiredTransfers - 1);
        Check(terminal_child.status == run::Status::kInvalidPolicy && !terminal_child.parent,
              "a child budget below the terminal-stop requirement is rejected");
        const Started short_parent = StartFullParent(fixture, 72, full::kChildRequiredTransfers);
        Check(short_parent.status == run::Status::kInvalidPolicy && !short_parent.parent,
              "a parent budget of 72 is rejected before execution");
        const Started shared = StartFullParent(fixture, 73, 73);
        Check(shared.status == run::Status::kInvalidPolicy && !shared.parent,
              "a shared parent-sized child budget is rejected");
        Check(!short_child.authorization && !terminal_child.authorization && !short_parent.authorization &&
                  !shared.authorization && run::LiveAuthorizationCount() == 0,
              "rejected budgets create no authorisation or worker");
    }

    {
        g_tls_reference_address = 0;
        g_tls_reference_self = 0;
        g_tls_reference_size = 0;
        g_tls_reference_hash = 0;
        g_tls_reference_read_ok = false;
        run::SetInjectionHookForTest(run::Role::kChild,
                                     full::kChildRoute[full::kChildRouteCount - 1].pc, CaptureTlsReference);
        const FullRun run = RunFullRoute(fixture);
        run::ClearInjectionHooksForTest();
        Check(run.started.status == run::Status::kReady, "the full 224-instruction synthetic run is authorised");
        Check(run.have_parent && run.have_child, "both full-route workers leave evidence");
        Check(run.parent.status == run::Status::kStopped && run.parent.instructions == 71 &&
                  run.parent.transfers == 73 && run.parent.required_transfers == 73 &&
                  run.parent.stop_pc == full::kParentStop && run.parent.at_stop,
              "the parent retires 71 instructions in 73 transfers and stops at its reviewed boundary");
        Check(run.child.status == run::Status::kStopped &&
                  run.child.instructions == full::kChildExecutedInstructions &&
                  run.child.transfers == full::kChildRequiredTransfers &&
                  run.child.required_transfers == full::kChildRequiredTransfers &&
                  run.child.stop_pc == full::kChildStop && run.child.at_stop,
              "the child retires 153 instructions in 156 transfers and stops at its reviewed boundary");
        Check(run.parent.instructions + run.child.instructions == full::kTotalExecutedInstructions,
              "the synthetic execution retires exactly 224 instructions");
        Check(!TraceHasInstructionAt(run.parent, run::Role::kParent, full::kParentStop) &&
                  !TraceHasInstructionAt(run.child, run::Role::kChild, full::kChildStop),
              "neither mandatory stop instruction is entered");
        Check(run.parent.kernel_calls == 2 && run.child.kernel_calls == 5,
              "the parent reaches 2 kernel services and the child 5");
        const run::KernelObservation* create = FindKernel(run.parent, run::Role::kParent, 255);
        const run::KernelObservation* close = FindKernel(run.parent, run::Role::kParent, 187);
        Check(create && create->result == 0 && close && close->result == 0,
              "ordinal 255 creates the child and ordinal 187 closes its handle through the checked ABI");
        Check(FindKernel(run.child, run::Role::kChild, 277) && FindKernel(run.child, run::Role::kChild, 277)->result == 0 &&
                  FindKernel(run.child, run::Role::kChild, 294) && FindKernel(run.child, run::Role::kChild, 294)->result == 0,
              "ordinals 277/294 balance the critical section through the checked ABI");
        const run::KernelObservation* shutdown = FindKernel(run.child, run::Role::kChild, 47);
        const run::KernelObservation* dpc = FindKernel(run.child, run::Role::kChild, 107);
        const run::KernelObservation* timer = FindKernel(run.child, run::Role::kChild, 113);
        Check(shutdown && shutdown->result == 0 && dpc && dpc->result == 0 && timer && timer->result == 0,
              "ordinals 47/107/113 reach the production checked implementations");
        kernel::ShutdownRegistrationRecord registration;
        Check(kernel::ShutdownRegistrationCountForTest() == 1 &&
                  kernel::ShutdownRegistrationForTest(0, &registration) &&
                  registration.object == full::kShutdownObject &&
                  registration.notification_routine == full::kSehHandler &&
                  registration.priority == 0x80000000u && registration.owner_thread == run.child.thread_id &&
                  registration.memory_generation == mem::Generation() &&
                  std::strcmp(registration.image_sha1, fixture.sha1.c_str()) == 0,
              "ordinal 47 records the reviewed object, routine, priority, owner and image identity");
        auto read_u8 = [](uint32_t address) {
            uint8_t value = 0;
            mem::Read(address, &value, sizeof(value));
            return value;
        };
        auto read_u32 = [](uint32_t address) {
            uint32_t value = 0;
            mem::Read(address, &value, sizeof(value));
            return value;
        };
        Check(read_u8(full::kDpcObject + 0) == 19 && read_u8(full::kDpcObject + 1) == 0 &&
                  read_u8(full::kDpcObject + 2) == 0 && read_u8(full::kDpcObject + 3) == 0 &&
                  read_u32(full::kDpcObject + 4) == 0 && read_u32(full::kDpcObject + 8) == 0 &&
                  read_u32(full::kDpcObject + 0x0C) == full::kSehHandler &&
                  read_u32(full::kDpcObject + 0x10) == full::kDpcContext &&
                  read_u32(full::kDpcObject + 0x14) == 0 && read_u32(full::kDpcObject + 0x18) == 0,
              "ordinal 107 publishes the exact 0x1C KDPC fields");
        Check(read_u8(full::kTimerObject + 0) == 8 && read_u8(full::kTimerObject + 2) == 10 &&
                  read_u32(full::kTimerObject + 4) == 0 && read_u32(full::kTimerObject + 8) == full::kTimerObject + 8 &&
                  read_u32(full::kTimerObject + 12) == full::kTimerObject + 8 &&
                  read_u32(full::kTimerObject + 0x10) == 0 && read_u32(full::kTimerObject + 0x14) == 0 &&
                  read_u32(full::kTimerObject + 0x18) == 0 && read_u32(full::kTimerObject + 0x1C) == 0 &&
                  read_u32(full::kTimerObject + 0x20) == 0 && read_u32(full::kTimerObject + 0x24) == 0,
              "ordinal 113 publishes the exact 0x28 KTIMER fields");
        Check(read_u32(full::kCriticalSectionAddress + 0x10) == 0xFFFFFFFFu &&
                  read_u32(full::kCriticalSectionAddress + 0x14) == 0 &&
                  read_u32(full::kCriticalSectionAddress + 0x18) == 0,
              "the checked critical-section enter/leave pair restores the free layout");
        Check(run.child.seh_verified && run.child.seh_registration != 0 && run.child.tls_verified &&
                  run.child.tls != 0,
              "SEH and TLS remain live and verified at the child stop");
        const run::TlsEvidence& tls = run.child.tls_evidence;
        Check(tls.status == run::TlsEvidenceStatus::kValid && tls.pointer_read_ok && tls.block_read_ok &&
                  tls.hash_recorded && tls.address == run.child.tls &&
                  tls.expected_pointer == run.child.tls + 4 && tls.self_pointer == run.child.tls + 4 &&
                  tls.block_size == full::kTlsBlockSize && tls.content_hash != 0,
              "the stop-time TLS snapshot is valid and complete");
        Check(g_tls_reference_read_ok.load() && g_tls_reference_address.load() == tls.address &&
                  g_tls_reference_self.load() == tls.self_pointer &&
                  g_tls_reference_size.load() == tls.block_size &&
                  g_tls_reference_hash.load() == tls.content_hash,
              "the TLS snapshot matches the independent pre-stop reference");
        Check(run.parent.tls_evidence.status == run::TlsEvidenceStatus::kNotEvaluated,
              "a route without TLS leaves the snapshot not evaluated");
        bool seh_checks_ok = run.child.seh_check_count >= 14;
        for (size_t i = 0; i < run.child.seh_check_count; ++i) {
            const run::SehCheck& check = run.child.seh_checks[i];
            if (check.status == uint8_t(run::SehCheckStatus::kFail) ||
                check.status == uint8_t(run::SehCheckStatus::kUnknown))
                seh_checks_ok = false;
        }
        Check(seh_checks_ok, "every SEH predicate passes on the full-route frame");
        const uint32_t handle = read_u32(full::kHandleOut);
        Check(handle != 0 && !kernel::FromHandle(handle),
              "the parent closed the created child handle after the ordinal-187 call");
        Check(read_u32(full::kHandlerMarker) == 0, "the synthetic SEH handler never ran");
        Check(run.parent.thread_id != run.child.thread_id && run.parent.claim_id && run.child.claim_id &&
                  run.parent.claim_id != run.child.claim_id,
              "the full-route workers have distinct thread and claim identities");
        Check(kernel::Threads().empty(), "both full-route workers unregister after finalisation");
        Check(read_u32(full::kDebe8) == 0 && read_u32(full::kDebec) == 0,
              "the extended helper clears the reviewed timer flags");
        {
            std::string acceptance_error;
            const bool accepted = AcceptFullRoute(fixture, run, fixture.full_parent, fixture.full_child,
                                                  acceptance_error);
            const std::string message = accepted ? "the full-route acceptance contract passes"
                                                 : "acceptance: " + acceptance_error;
            Check(accepted, message.c_str());
        }
        run::RunEvidence after_finalize;
        Check(run::EvidenceForRole(run::Role::kChild, &after_finalize) &&
                  after_finalize.tls_evidence.status == run::TlsEvidenceStatus::kValid &&
                  after_finalize.tls_evidence.content_hash == g_tls_reference_hash.load() &&
                  after_finalize.tls_evidence.self_pointer == g_tls_reference_self.load() &&
                  after_finalize.tls_evidence.address == g_tls_reference_address.load(),
              "the host-owned TLS snapshot survives child-worker finalisation");
        std::printf("FULL_ROUTE parent_instructions=%u parent_transfers=%u child_instructions=%u "
                    "child_transfers=%u total_instructions=%u boundary_steps=%u\n",
                    run.parent.instructions, run.parent.transfers, run.child.instructions, run.child.transfers,
                    run.parent.instructions + run.child.instructions,
                    run.parent.instructions + run.child.instructions + 2);
    }

    {
        run::SetInjectionHookForTest(run::Role::kChild,
                                     full::kChildRoute[full::kChildRouteCount - 1].pc, CorruptTlsAddress);
        const FullRun run = RunFullRoute(fixture);
        run::ClearInjectionHooksForTest();
        Check(run.have_child && run.child.status == run::Status::kTlsViolation &&
                  run.child.instructions == full::kChildExecutedInstructions &&
                  run.child.transfers == full::kChildRequiredTransfers &&
                  run.child.tls_evidence.status == run::TlsEvidenceStatus::kReadFailed &&
                  !run.child.tls_evidence.hash_recorded && run.child.tls_evidence.content_hash == 0 &&
                  run.child.tls_evidence.content_hash != ZeroTlsHash(full::kTlsBlockSize),
              "a failed TLS read cannot produce valid evidence or a zero-buffer hash");
        Check(run.have_parent && run.parent.status == run::Status::kStopped &&
                  run.parent.instructions == 71 && run.parent.transfers == 73,
              "the parent stop is independent of the child TLS read failure");
    }

    {
        run::SetInjectionHookForTest(run::Role::kChild,
                                     full::kChildRoute[full::kChildRouteCount - 1].pc, CorruptTlsPointer);
        const FullRun run = RunFullRoute(fixture);
        run::ClearInjectionHooksForTest();
        Check(run.have_child && run.child.status == run::Status::kTlsViolation &&
                  run.child.instructions == full::kChildExecutedInstructions &&
                  run.child.transfers == full::kChildRequiredTransfers &&
                  run.child.tls_evidence.status == run::TlsEvidenceStatus::kMismatch &&
                  run.child.tls_evidence.pointer_read_ok && run.child.tls_evidence.block_read_ok &&
                  run.child.tls_evidence.hash_recorded && !run.child.tls_verified,
              "a TLS self-pointer mismatch is distinguished from a failed read");
    }

    {
        run::SetInjectionHookForTest(run::Role::kChild,
                                     full::kChildRoute[full::kChildRouteCount - 1].pc, ClearTlsAddress);
        const FullRun run = RunFullRoute(fixture);
        run::ClearInjectionHooksForTest();
        Check(run.have_child && run.child.status == run::Status::kTlsViolation &&
                  run.child.instructions == full::kChildExecutedInstructions &&
                  run.child.transfers == full::kChildRequiredTransfers &&
                  run.child.tls_evidence.status == run::TlsEvidenceStatus::kUnavailable &&
                  !run.child.tls_evidence.hash_recorded && run.child.tls_evidence.content_hash == 0,
              "an unavailable TLS pointer is reported without evidence");
    }

    {
        const FullRun run = RunFullRoute(fixture, 143, 200);
        Check(run.started.status == run::Status::kReady && run.have_parent && run.have_child,
              "larger budgets are admitted");
        Check(run.parent.status == run::Status::kStopped && run.parent.instructions == 71 &&
                  run.parent.transfers == 73 && run.child.status == run::Status::kStopped &&
                  run.child.instructions == full::kChildExecutedInstructions &&
                  run.child.transfers == full::kChildRequiredTransfers,
              "larger budgets do not execute past the reviewed route or terminal stops");
        {
            std::string acceptance_error;
            const bool accepted = AcceptFullRoute(fixture, run, fixture.full_parent, fixture.full_child,
                                                  acceptance_error);
            const std::string message = accepted ? "the larger-budget acceptance contract passes"
                                                 : "acceptance: " + acceptance_error;
            Check(accepted, message.c_str());
        }
    }

    {
        run::RouteManifest child = FullChildManifest();
        child.required_transfers = 0;  // test-only: exercise the gate's terminal accounting directly
        const FullRun run = RunFullRoute(fixture, 73, route::kChildRequiredTransfers - 1, 71,
                                         full::kChildExecutedInstructions, &child);
        Check(run.started.status == run::Status::kReady, "the terminal-stop probe is admitted");
        Check(run.have_child && run.child.status == run::Status::kBudgetExceeded &&
                  run.child.instructions == full::kChildExecutedInstructions &&
                  run.child.transfers == full::kChildRequiredTransfers - 1 &&
                  run.child.stop_pc == full::kChildStop &&
                  run.child.stop_target == full::kChildStop && !run.child.at_stop,
              "a budget one short of the terminal stop refuses it instead of reporting completion");
    }

    {
        struct Stage {
            uint32_t ordinal;
            uint32_t budget;
        } stages[] = {{47, 128}, {107, 136}, {113, 139}};
        for (const Stage& stage : stages) {
            run::RouteManifest child = FullChildManifest();
            child.required_transfers = 0;  // test-only: the gate budget is the subject
            const FullRun run = RunFullRoute(fixture, 73, full::kChildRequiredTransfers, 71, stage.budget, &child);
            Check(run.started.status == run::Status::kReady && run.have_child &&
                      run.child.status == run::Status::kBudgetExceeded &&
                      run.child.instructions == stage.budget,
                  "an instruction budget stops before the next kernel service");
            Check(!FindKernel(run.child, run::Role::kChild, stage.ordinal),
                  "no kernel service executes past the budget");
            Check(run.have_parent && run.parent.status == run::Status::kStopped,
                  "the parent stop is independent of the child's budget stop");
        }
    }

    {
        run::RouteManifest loop;
        loop.steps = fixture::kLoopRoute;
        loop.count = fixture::kLoopRouteCount;
        loop.entry = fixture::kLoopEntry;
        loop.stop = fixture::kLoopStop;
        loop.executed_instructions = fixture::kLoopExecutedInstructions;
        loop.trap_entries = fixture::kLoopTrapEntries;
        loop.trap_count = 1;
        loop.identity = run::ManifestIdentity(loop);
        const RouteRun run = RunSyntheticRoute(fixture, loop, 4, 8, 1, 2);
        Check(run.have && run.evidence.status == run::Status::kUnreviewedTransfer &&
                  run.evidence.instructions == 1 && !run.evidence.at_stop,
              "an unexpected self-loop is refused at the reviewed edge");
    }

    {
        run::RouteManifest unknown;
        unknown.steps = fixture::kUnknownRoute;
        unknown.count = fixture::kUnknownRouteCount;
        unknown.entry = fixture::kUnknownEntry;
        unknown.stop = fixture::kUnknownStop;
        unknown.executed_instructions = fixture::kUnknownExecutedInstructions;
        unknown.trap_entries = fixture::kUnknownTrapEntries;
        unknown.trap_count = 1;
        unknown.identity = run::ManifestIdentity(unknown);
        const RouteRun run = RunSyntheticRoute(fixture, unknown, 4, 8, 1, 2);
        Check(run.have && run.evidence.status == run::Status::kUnreviewedTransfer &&
                  run.evidence.instructions == 1 && !run.evidence.at_stop,
              "an unknown transfer target is refused before execution");
    }

    {
        // The parent's last step is a plain tail transfer: rewriting its next
        // cannot bypass the mandatory stop because the stop target is checked
        // first.
        std::array<route::Step, full::kParentRouteCount> parent_steps{};
        std::copy(std::begin(full::kParentRoute), std::end(full::kParentRoute), parent_steps.begin());
        parent_steps.back().next = full::kParentEntry;
        run::RouteManifest parent = FullParentManifest();
        parent.steps = parent_steps.data();
        parent.identity = run::ManifestIdentity(parent);
        const FullRun run = RunFullRoute(fixture, 73, full::kChildRequiredTransfers, 71,
                                         full::kChildExecutedInstructions, nullptr, &parent);
        Check(run.started.status == run::Status::kReady && run.have_parent &&
                  run.parent.status == run::Status::kStopped && run.parent.instructions == 71 &&
                  run.parent.transfers == 73 && run.parent.stop_pc == full::kParentStop && run.parent.at_stop,
              "a rewritten parent next cannot continue past the mandatory terminal stop");
    }

    {
        // The child's last step is the ordinal-113 kernel call. Rewriting its
        // next changes the reviewed return address, so the kernel call itself
        // fails closed before any continuation can be attempted.
        std::array<route::Step, full::kChildRouteCount> steps{};
        std::copy(std::begin(full::kChildRoute), std::end(full::kChildRoute), steps.begin());
        steps.back().next = full::kChildEntry;
        run::RouteManifest child = FullChildManifest();
        child.steps = steps.data();
        child.identity = run::ManifestIdentity(child);
        const FullRun run = RunFullRoute(fixture, 73, full::kChildRequiredTransfers, 71,
                                         full::kChildExecutedInstructions, &child);
        Check(run.started.status == run::Status::kReady && run.have_child &&
                  run.child.status == run::Status::kStopped &&
                  run.child.instructions == full::kChildExecutedInstructions &&
                  run.child.transfers == full::kChildRequiredTransfers &&
                  run.child.stop_pc == full::kChildStop && run.child.at_stop,
              "a rewritten child next cannot continue past the mandatory terminal stop");
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
        policy.instruction_budget = route::kParentExecutedInstructions;
        policy.transfer_budget = 1;
        policy.kernel_budget = 1;
        policy.maximum_call_depth = 1;
        policy.child_instruction_budget = route::kChildExecutedInstructions;
        policy.child_transfer_budget = 128;
        policy.child_kernel_budget = 1;
        policy.child_maximum_call_depth = 1;
        run::RunAuthorization authorization;
        const run::Status undersized = run::AuthorizeAuthenticRun(run::Role::kParent, policy, &authorization);
        Check(undersized == run::Status::kInvalidPolicy && !authorization,
              "the previous child budget of 128 is refused by the authentic preflight");
        policy.transfer_budget = route::kParentRequiredTransfers;
        policy.child_transfer_budget = route::kChildRequiredTransfers;
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
        ResetData();
        const run::RouteManifest continuation = ChildContinuationManifest();
        const Started started = StartParent(fixture, fixture::kChildContinuationExecuted, &continuation);
        Check(started.status == run::Status::kReady, "extended child continuation is authorised");
        JoinThread(started.parent);
        const bool child_done = WaitForRole(run::Role::kChild);
        const bool parent_done = WaitForRole(run::Role::kParent);
        run::RunEvidence parent, child;
        const bool have_parent = run::EvidenceForRole(run::Role::kParent, &parent);
        const bool have_child = run::EvidenceForRole(run::Role::kChild, &child);
        Check(child_done && parent_done && have_parent && have_child, "continuation workers leave evidence");
        std::printf("continuation child status=%s ins=%u stop=%08X at=%d seh=%d tls=%d parent=%s\n",
                    run::StatusName(child.status), child.instructions, child.stop_pc, child.at_stop,
                    child.seh_verified, child.tls_verified, run::StatusName(parent.status));
        Check(child.status == run::Status::kStopped && child.instructions == fixture::kChildContinuationExecuted &&
                  child.stop_pc == fixture::kChildContinuationStop && child.at_stop,
              "the first start-routine call returns and stops before the next instruction");
        Check(child.seh_verified && child.seh_registration != 0 && child.tls_verified && child.tls != 0,
              "SEH and TLS remain valid at the extended stop");
        const run::TraceRecord* stop = FindTrace(child, run::Role::kChild, run::EventKind::kStop);
        Check(stop && stop->eax == 0xDEADBEEF, "the called start routine's return value is preserved at the stop");
    }
    {
        ResetData();
        set_version_data(0x16CE, 0x12345678, 0x89ABCDEF);
        const RouteRun run = RunSyntheticRoute(fixture, VersionCallerManifest(), 13, 64, 4, 4);
        Check(run.have && run.evidence.status == run::Status::kStopped &&
                  run.evidence.stop_pc == fixture::kVersionCallerStop,
              "the reviewed 5838 hardware/version branch reaches the post-call stop");
        Check(read_guest32(fixture::kPhysicalMarker) == 0x12345678 &&
                  read_guest32(fixture::kInitMarker) == 0x89ABCDEF,
              "the physical-window reads deliver the checked 8001003C and 8000FFF0 values");
        Check(read_guest32(0xF001003C) == 0x12345678 && read_guest32(0xF000FFF0) == 0x89ABCDEF,
              "the checked physical alias 0xF0000000 observes the same backing values");
    }
    {
        ResetData();
        set_version_data(0x16CF, 0, 0);
        const RouteRun run = RunSyntheticRoute(fixture, VersionCallerManifest(), 13, 64, 4, 4);
        Check(run.have && run.evidence.status == run::Status::kForbiddenBranch,
              "an unreviewed build stops at the version branch before reading KPRCB or physical memory");
    }
    {
        ResetData();
        set_version_data(0x16CE, 0, 0);
        const RouteRun run = RunSyntheticRoute(fixture, VersionCallerManifest(), 13, 64, 4, 4,
                                                [](kernel::Thread& thread) {
                                                    const uint32_t one = 1;
                                                    mem::Write(thread.kpcr + 0x278, &one, sizeof(one));
                                                });
        Check(run.have && run.evidence.status == run::Status::kForbiddenBranch,
              "a non-null KPRCB+0x250 stops before the physical probe");
    }
    {
        ResetData();
        const RouteRun cli = RunSyntheticRoute(fixture, CliManifest(), 1, 8, 1, 1);
        const RouteRun sti = RunSyntheticRoute(fixture, StiManifest(), 1, 8, 1, 1);
        Check(cli.have && cli.evidence.status == run::Status::kUnsupportedInstruction,
              "CLI remains a checked trap on the proposed boundary");
        Check(sti.have && sti.evidence.status == run::Status::kUnsupportedInstruction,
              "STI remains a checked trap on the proposed boundary");
    }
    {
        run::AuthenticKernelData data;
        const run::Status prepared = run::PrepareAuthenticKernelData(&data);
        Check(prepared == run::Status::kReady && data.hardware_info != 0 && data.kernel_version != 0,
              "the reviewed kernel data registers valid hardware/version addresses");
        Check(data.hardware_flags == 0x20 && data.build == 5838,
              "the production XboxHardwareInfo flags and XboxKrnlVersion build are initialised");
        Check(kernel::DataAddress(322) == data.hardware_info && kernel::DataAddress(324) == data.kernel_version,
              "the import data addresses resolve to the initialised kernel data");
        Check(read_guest32(data.hardware_info) == 0x20 &&
                  (read_guest32(data.kernel_version + 4) & 0xFFFF) == 5838,
              "the guest-visible hardware flags and build read back correctly");
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
