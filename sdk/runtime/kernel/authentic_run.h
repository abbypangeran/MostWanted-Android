#pragma once

// Phase 2D-19: the separately gated authentic-execution harness. It is compiled
// only into targets that define MOSTWANTED_AUTHENTIC_RUN_HARNESS; ordinary
// Android and iOS builds never include it, and the production Boot() and
// CallGuestCallback Apple kExecutionDisabled paths are untouched.
//
// The harness does not change what a guest instruction means. It enforces an
// exact reviewed route at every generated dispatch boundary, counts the
// reviewed instructions, refuses the terminal boundary before the target
// executes, and owns both workers through claim-bound, revocable authorisations.
// Authentic-image authorisation is hard-refused by kAuthenticExecutionAuthorised;
// only synthetic fixture identities can be authorised, and only in builds that
// also define MOSTWANTED_AUTHENTIC_RUN_TESTS.

#include <cstddef>
#include <cstdint>

#include "kernel/authentic_run_route.h"
#include "kernel/kernel.h"

namespace kernel::authentic {

enum class Role : uint8_t { kParent = 1, kChild = 2 };

enum class Origin : uint8_t { kStaticPreflight = 0, kSyntheticFixture = 1, kAuthenticImage = 2 };

enum class Status : uint8_t {
    kReady,
    kStopped,
    kExecutionRefused,
    kAuthenticExecutionNotAuthorised,
    kSyntheticTestsDisabled,
    kInvalidImage,
    kInvalidRoute,
    kInvalidMapping,
    kInvalidPolicy,
    kInvalidThread,
    kStaleDispatch,
    kStaleMemory,
    kShutdown,
    kNotClaimed,
    kWrongWorker,
    kRevoked,
    kUnreviewedTransfer,
    kUnreviewedInstruction,
    kForbiddenBranch,
    kBudgetExceeded,
    kCallDepthExceeded,
    kKernelRejected,
    kKernelAbiMismatch,
    kMemoryViolation,
    kUnsupportedInstruction,
    kSehViolation,
    kTlsViolation,
    kCodePageDenied,
    kCancelled,
    kInternalError,
};

const char* StatusName(Status status);

// The single authentic-execution authorisation switch. It is false for Phase
// 2D-19: the capability, binding and refusal paths are implemented and tested,
// but no authentic-image authorisation exists until the user separately
// authorises a specific bounded run.
inline constexpr bool kAuthenticExecutionAuthorised = false;

inline constexpr uint32_t kSyntheticImageIdentity = 0x53324431u;  // "S2D1"

// A reviewed stop may require transient guest state that the worker releases
// at finalisation. The capture is performed by VerifyStopState while the
// worker still holds the stack lease, and is copied into RunEvidence.
enum class StopStackFrame : uint8_t {
    kNone = 0,
    kContinuationDeviceQuery = 1,
};

struct RouteManifest {
    const route::Step* steps = nullptr;
    size_t count = 0;
    uint32_t entry = 0;
    uint32_t stop = 0;
    uint32_t executed_instructions = 0;
    uint64_t identity = 0;
    const uint32_t* trap_entries = nullptr;
    size_t trap_count = 0;
    uint32_t seh_call = 0;          // the call that enters the reviewed SEH prologue, 0 if none
    uint32_t seh_entry = 0;
    uint32_t seh_handler = 0;
    uint32_t seh_scope_table = 0;
    uint32_t seh_scope_bytes = 0;
    uint32_t seh_scope_flag = 0xFFFFFFFFu;  // reviewed [E] value at the stop
    uint32_t tls_block_size = 0;         // 0 if the route does not initialise TLS
    uint32_t critical_section_object = 0;  // reviewed 277/294 object, 0 if none
    uint32_t start_routine = 0;          // the thread start routine this route serves, 0 if none
    uint32_t required_transfers = 0;     // manifest-derived total including the terminal stop; 0 for test-only routes
    StopStackFrame stop_stack_frame = StopStackFrame::kNone;
    uint8_t stop_stack_words = 0;        // 0 disables; kContinuationDeviceQuery requires 5
};

const RouteManifest& AuthenticParentManifest();
const RouteManifest& AuthenticChildManifest();
uint64_t ManifestIdentity(const RouteManifest& manifest);
// A reviewed manifest must declare the exact transfer total derived from its
// steps. A non-zero required_transfers guards the admission floor; zero marks a
// synthetic test route whose budgets are still enforced dynamically by the gate.
bool ManifestTransfersConsistent(const RouteManifest& manifest);
bool TransferBudgetSatisfies(const RouteManifest& manifest, uint32_t budget);
uint64_t RuntimeMappingIdentity();
uint32_t RuntimeMappingCount();

struct AuthorizationPolicy {
    Origin origin = Origin::kStaticPreflight;
    Role role = Role::kParent;
    const RouteManifest* manifest = nullptr;
    const RouteManifest* child_manifest = nullptr;  // required for a parent policy
    const char* image_sha1 = nullptr;       // exact image identity, never empty
    const char* policy_identity = nullptr;  // reviewed callback-policy identity for an authentic run
    uint32_t expected_thread_id = 0;        // 0 selects the prepared thread's id
    uint64_t mapping_identity = 0;          // exact generated mapping identity
    uint64_t dispatch_generation = 0;       // exact, never 0
    uint64_t memory_generation = 0;         // exact, never 0
    uint32_t instruction_budget = 0;        // a ceiling; the reviewed stop still ends the route
    uint32_t transfer_budget = 0;            // must be >= the manifest's required_transfers
    uint32_t kernel_budget = 0;
    uint32_t maximum_call_depth = 0;
    // the child created through the reviewed ordinal-255 edge
    uint32_t child_instruction_budget = 0;
    uint32_t child_transfer_budget = 0;
    uint32_t child_kernel_budget = 0;
    uint32_t child_maximum_call_depth = 0;
};

#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
struct ParentStartRequest;
#endif

class RunAuthorization {
public:
    RunAuthorization() = default;
    RunAuthorization(RunAuthorization&& other) noexcept : id_(other.id_) { other.id_ = 0; }
    RunAuthorization& operator=(RunAuthorization&& other) noexcept {
        if (this != &other) {
            id_ = other.id_;
            other.id_ = 0;
        }
        return *this;
    }
    RunAuthorization(const RunAuthorization&) = delete;
    RunAuthorization& operator=(const RunAuthorization&) = delete;
    uint64_t id() const { return id_; }
    explicit operator bool() const { return id_ != 0; }

private:
    explicit RunAuthorization(uint64_t id) : id_(id) {}
    uint64_t id_ = 0;
    friend Status AuthorizeRunForThread(Thread*, const AuthorizationPolicy&, RunAuthorization*);
    friend Status AuthorizeAuthenticRun(Role, const AuthorizationPolicy&, RunAuthorization*);
#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
    friend Status AuthorizeSyntheticRouteForThread(Thread*, const RouteManifest&, const char*, uint64_t, uint64_t,
                                                   uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, RunAuthorization*);
    friend Status StartParentWorkerForTest(const ParentStartRequest&, RunAuthorization*);
#endif
};

// Synthetic/test-only admission. Binds one claim to one prepared Thread. The
// worker that starts on that Thread claims it; no other worker can.
Status AuthorizeRunForThread(Thread* thread, const AuthorizationPolicy& policy, RunAuthorization* authorization);

// The authentic-image capability. It validates every binding it is given and
// then returns kAuthenticExecutionNotAuthorised while kAuthenticExecutionAuthorised
// is false; it never creates a claim.
Status AuthorizeAuthenticRun(Role role, const AuthorizationPolicy& policy, RunAuthorization* authorization);

Status RevokeRun(const RunAuthorization& authorization);
void CancelAllRuns();
bool AnyRunActive();
bool GateActive();
bool ThreadHasBoundRun(const Thread* thread);
// Number of live (armed or claimed) authorisations; test evidence only.
uint32_t LiveAuthorizationCount();

struct TraceRecord {
    uint64_t sequence = 0;
    uint32_t worker_id = 0;
    uint32_t claim_id = 0;
    uint8_t role = 0;
    uint8_t origin = 0;
    uint8_t event = 0;  // EventKind
    uint8_t status = 0; // Status at a stop/violation
    uint32_t pc = 0;
    uint32_t target = 0;
    uint32_t esp = 0;
    uint32_t ebp = 0;
    uint32_t eax = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t instructions = 0;
    uint32_t transfers = 0;
    uint32_t kernel_calls = 0;
    uint16_t ordinal = 0;
    uint16_t argument_count = 0;
    uint32_t seh_registration = 0;
    uint32_t tls = 0;
};

enum class EventKind : uint8_t {
    kWorkerStart = 1,
    kInstructionEntry = 2,
    kCallTransfer = 3,
    kJumpTransfer = 4,
    kReturnTransfer = 5,
    kKernelCall = 6,
    kKernelReturn = 7,
    kStop = 8,
    kViolation = 9,
    kCancel = 10,
    kFinalize = 11,
};

struct KernelObservation {
    uint32_t ordinal = 0;
    uint32_t call_pc = 0;
    uint32_t return_pc = 0;
    uint32_t slot = 0;
    uint32_t argument = 0;
    uint32_t esp_before = 0;
    uint32_t esp_after = 0;
    uint32_t result = 0;
    Status status = Status::kReady;
};

enum class SehCheckKind : uint8_t {
    kCapture,
    kEntryAddress,
    kStackBounds,
    kPostPrologueEsp,
    kCallerEbp,
    kPreviousRecord,
    kHandler,
    kScopeFlag,
    kScopeTable,
    kSavedEsp,
    kCallerEbpHeld,
    kChain,
    kHandlerResolve,
    kScopeTerminator,
};

enum class SehCheckStatus : uint8_t { kPass, kFail, kUnknown, kSkipped };

struct SehCheck {
    uint8_t kind = 0;
    uint8_t status = 0;
    uint32_t expected = 0;
    uint32_t observed = 0;
    uint32_t address = 0;
};

// TLS evidence is captured at the stop, while the worker's TLS allocation is
// still live, and kept as a host-owned snapshot. It is never re-read from guest
// memory after finalisation. The content hash is meaningful only when the full
// checked read succeeded and hash_recorded is true.
enum class TlsEvidenceStatus : uint8_t {
    kNotEvaluated,
    kValid,
    kUnavailable,
    kReadFailed,
    kMismatch,
};

const char* TlsEvidenceStatusName(TlsEvidenceStatus status);

struct TlsEvidence {
    uint32_t address = 0;           // thread->tls at the stop
    uint32_t self_pointer = 0;      // checked [address] value
    uint32_t expected_pointer = 0;  // reviewed self-pointer (address + 4)
    uint32_t block_size = 0;        // manifest tls_block_size
    uint32_t content_hash = 0;      // FNV-1a over the block, only when recorded
    TlsEvidenceStatus status = TlsEvidenceStatus::kNotEvaluated;
    bool pointer_read_ok = false;
    bool block_read_ok = false;
    bool hash_recorded = false;
};

// A host-owned snapshot of transient worker stack memory at a mandatory stop.
// capture status is independent of the semantic validation flags: a complete
// checked read may still contain the wrong frame, and a failed read never
// becomes a zero-filled valid snapshot.
enum class StopStackStatus : uint8_t {
    kNotEvaluated,
    kValid,
    kUnavailable,
    kReadFailed,
    kMismatch,
};

const char* StopStackStatusName(StopStackStatus status);

struct StopStackEvidence {
    uint32_t pc = 0;
    uint32_t esp = 0;
    uint32_t ebp = 0;
    uint32_t eax = 0;
    uint32_t requested_words = 0;
    uint32_t words[5] = {};
    uint32_t bytes_read = 0;
    uint32_t content_hash = 0;       // FNV-1a over the read bytes, only when recorded
    uint64_t memory_generation = 0;
    uint64_t memory_generation_after = 0;
    StopStackStatus status = StopStackStatus::kNotEvaluated;
    bool complete_read = false;
    bool hash_recorded = false;
    bool aligned = false;
    bool in_stack_bounds = false;
    bool registers_valid = false;
    bool layout_valid = false;
    bool values_valid = false;
    bool generation_stable = false;
};

struct RunEvidence {
    uint32_t role = 0;
    uint32_t origin = 0;
    uint32_t thread_id = 0;
    uint32_t claim_id = 0;
    Status status = Status::kReady;
    uint32_t stop_pc = 0;
    uint32_t stop_target = 0;
    uint32_t instructions = 0;
    uint32_t transfers = 0;
    uint32_t required_transfers = 0;
    uint32_t kernel_calls = 0;
    uint32_t maximum_depth = 0;
    uint32_t seh_registration = 0;
    uint32_t tls = 0;
    uint32_t tls_pointer = 0;  // mirrors tls_evidence.self_pointer
    TlsEvidence tls_evidence;
    StopStackEvidence stop_stack;
    bool stop_stack_verified = false;
    bool seh_verified = false;
    bool tls_verified = false;
    bool at_stop = false;
    size_t trace_count = 0;
    size_t kernel_count = 0;
    size_t seh_check_count = 0;
    SehCheck seh_checks[16] = {};
};

bool EvidenceForRole(Role role, RunEvidence* evidence);
const TraceRecord* TraceForRole(Role role, size_t index);
const KernelObservation* KernelObservationForRole(Role role, size_t index);

// Worker entry used by StartThread when a Thread has a bound run. It claims the
// authorisation, walks the reviewed route, and returns kInvoked only for the
// exact pre-instruction stop. Cancellation and violations return kCancelled.
GuestCallbackStatus RunBoundWorker(X86Context& ctx, Thread& thread);

// Ordinal-255 child admission, called by the production export before it starts
// the prepared child. It validates the reviewed call site, the ten arguments,
// the prepared Thread, the child manifest, the mapping and both generations,
// then binds a child authorisation to that Thread. Any mismatch is fail-closed.
Status BindChildThreadForRequest(const AuthenticSystemThreadRequest& request, const ThreadRef& child,
                                 uint32_t call_pc);
// The reviewed kernel bridge is installed only by the gated harness. It
// registers exact, checked implementations for ordinals 255, 187, 277 and 294;
// no other export is replaced or given a fabricated result.
Status InstallHarnessKernelExports();
// Registers and initialises the reviewed production kernel data used by the
// start routine's hardware/version selector. It writes only the reviewed
// InitializeKernelData values and returns their addresses; no guest
// instruction executes. The caller must patch the image's data import slots
// 003C9E34/003C9E3C to hardware_info/kernel_version before the reviewed run.
struct AuthenticKernelData {
    uint32_t hardware_info = 0;
    uint32_t kernel_version = 0;
    uint32_t hardware_flags = 0;
    uint32_t build = 0;
};
Status PrepareAuthenticKernelData(AuthenticKernelData* data);
// The kernel call currently crossing the gate on this worker, for the ordinal
// 255 bridge's call-site binding. False outside a reviewed kernel call.
bool PendingKernelCall(uint32_t ordinal, uint32_t& call_pc, uint32_t& return_pc);

// Test-only parent start. Present only under MOSTWANTED_AUTHENTIC_RUN_TESTS.
#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
struct ParentStartRequest {
    uint32_t stack_size = 0x10000;
    uint32_t tls_size = 0x94;
    uint32_t handle_out = 0;
    uint32_t id_out = 0;
    uint64_t dispatch_generation = 0;
    uint64_t memory_generation = 0;
    uint32_t instruction_budget = 0;
    uint32_t transfer_budget = 0;
    uint32_t kernel_budget = 0;
    uint32_t maximum_call_depth = 0;
    uint32_t child_instruction_budget = 0;
    uint32_t child_transfer_budget = 0;
    uint32_t child_kernel_budget = 0;
    uint32_t child_maximum_call_depth = 0;
    const RouteManifest* manifest = nullptr;
    const RouteManifest* child_manifest = nullptr;
    const char* image_sha1 = nullptr;
    uint64_t mapping_identity = 0;
    ThreadRef* prepared_thread = nullptr;  // receives the started parent Thread
};
Status StartParentWorkerForTest(const ParentStartRequest& request, RunAuthorization* authorization);
// A negative route whose entry is mapped but whose first instruction is an
// unreviewed/unsupported step; used for fail-closed tests.
Status AuthorizeSyntheticRouteForThread(Thread* thread, const RouteManifest& manifest, const char* image_sha1,
                                        uint64_t mapping_identity, uint64_t dispatch_generation,
                                        uint64_t memory_generation, uint32_t instruction_budget,
                                        uint32_t transfer_budget, uint32_t kernel_budget, uint32_t maximum_call_depth,
                                        RunAuthorization* authorization);
void SetRunCancelForTest(Role role);
// Deterministic capture failure injection; it never writes guest memory.
enum class StopStackCaptureFaultForTest : uint8_t { kNone = 0, kUnavailable, kReadFailed, kPartial };
void SetStopStackCaptureFaultForTest(Role role, StopStackCaptureFaultForTest fault);
#endif

// Transfer gate entry points called from cpu/dispatch.cpp. They throw
// AuthenticRunStop or AuthenticRunViolation; the worker wrapper turns those into
// a stop report and deterministic finalisation.
enum class TransferKind : uint8_t { kCall, kJump, kRegisterJump, kIndirectJump };

void AdmitGeneratedTransfer(X86Context& ctx, uint32_t target, uint32_t return_address, uint32_t transfer_pc,
                            TransferKind kind);
void AdmitGuestReturn(X86Context& ctx, uint32_t pc, uint32_t pop_bytes);
// Runs the reviewed kernel export through the checked stdcall path and records
// the transition. slot_address is the call operand when the transfer came from
// memory, 0 for a register target.
void AdmitKernelCall(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t return_address, uint32_t transfer_pc,
                     uint32_t slot_address);

// Test-only injection hooks. They fire once at the named boundary so a test can
// mutate a page, revoke a claim or stop a worker deterministically.
#ifdef MOSTWANTED_AUTHENTIC_RUN_TESTS
using InjectionHook = void (*)(X86Context&);
void SetInjectionHookForTest(Role role, uint32_t at_pc, InjectionHook hook);
void ClearInjectionHooksForTest();
#endif

}  // namespace kernel::authentic

// The harness supplies these instead of the aborting production versions so a
// checked fault or trap becomes a stop report rather than a process abort.
// Declared by cpu/x86_ops.h; defined in authentic_run.cpp.
