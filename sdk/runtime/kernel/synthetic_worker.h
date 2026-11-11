#pragma once

#include <array>
#include <cstdint>

#include "kernel/guest_callback.h"

namespace kernel {

#ifdef MOSTWANTED_SYNTHETIC_WORKER_TESTS

#if !defined(MOSTWANTED_GUEST_CALLBACK_TESTS)
#error "synthetic worker tests require the guest callback test hooks"
#endif

// One explicitly authorized synthetic guest worker. The target must lie inside
// the synthetic dispatch image and the ticket generation must still be current.
struct SyntheticWorkerAuthorization {
    uint32_t target = 0;
    uint32_t context = 0;
    uint32_t expected_exit = 0;
    uint32_t code_base = 0;
    uint32_t code_end = 0;
    uint64_t dispatch_generation = 0;
    uint32_t image_identity = 0;
    uint32_t continuation_target = 0;
    GuestCallbackTicket callback_ticket{};
};

inline constexpr uint32_t kSyntheticWorkerImageIdentity = 0x4C4C5632u;  // "LLV2"

// Phase 2D-15: one claim identity per managed worker. A claim is bound to the
// Thread that made it (plus its id and memory generation), so a recycled
// thread address or allocation cannot inherit an older claim.
inline constexpr uint32_t kMaximumSyntheticWorkers = 4;

// Test-only child-creation bridge carried through ordinal 255. The caller must
// already hold a claim, so the production path is reached only when the tag is
// absent; authentic images never pass it.
inline constexpr uint32_t kSyntheticCreateThreadTag = 0x53435431u;      // "SCT1" in ThreadExtensionSize
inline constexpr uint32_t kSyntheticChildDescriptorMagic = 0x53435444u;  // "SCTD"

struct Thread;

struct SyntheticWorkerHandle {
    uint64_t id = 0;
    explicit operator bool() const { return id != 0; }
};

enum class SyntheticWorkerClaim : uint8_t { kNotAuthorized, kClaimed, kRejected };

// Persistent callback policy for one synthetic worker session. The policy is
// copied on arm; no caller pointer survives. Each admitted call is revalidated
// against the current owner, memory generation, dispatch generation and (for a
// table entry) the slot contents, so one session never authorises every mapped
// function. A union of these entries is the complete reachable call surface.
inline constexpr uint32_t kMaximumSyntheticSessionSlots = 12;
inline constexpr uint32_t kMaximumSyntheticSessionDepth = 8;

enum class SyntheticCallbackEntry : uint8_t {
    kIndirectSlot,    // call [slot_address]; the slot is re-read at admission
    kRegisterTarget,  // call reg; the captured target must match exactly
    kDirectCall,      // call rel32; the call site and target must both match
};

struct SyntheticCallbackSlotPolicy {
    SyntheticCallbackEntry entry = SyntheticCallbackEntry::kIndirectSlot;
    uint32_t slot_address = 0;
    uint32_t target = 0;
    uint32_t call_pc = 0;
    uint32_t argument_count = 0;
    bool callee_cleanup = false;
};

struct SyntheticSessionPolicy {
    uint32_t image_identity = 0;
    uint32_t code_base = 0;
    uint32_t code_end = 0;
    uint32_t max_depth = 0;
    uint64_t dispatch_generation = 0;
    uint32_t slot_count = 0;
    std::array<SyntheticCallbackSlotPolicy, kMaximumSyntheticSessionSlots> slots{};
};

enum class SyntheticCallbackAdmission : uint8_t {
    kAdmitted,
    kNoSession,
    kInvalidOwner,
    kStaleMemory,
    kStaleDispatch,
    kUnknownEntry,
    kSlotChanged,
    kUnauthorizedTarget,
    kInvalidPolicy,
    kAbiMismatch,
    kDepthExceeded,
};

const char* SyntheticCallbackAdmissionName(SyntheticCallbackAdmission admission);

// Test-only continuation boundary. kResumed means validated generated code ran
// at the authorized continuation and returned through its host sentinel.
enum class SyntheticContinuationStatus : uint8_t {
    kResumed,
    kNotAuthorized,
    kInvalidOwner,
    kInvalidState,
    kStaleMemory,
    kStaleDispatch,
    kUnauthorizedTarget,
    kInvalidTarget,
    kInvalidContext,
    kInvalidStack,
    kLeaseDenied,
    kInvalidReturn,
    kShutdown,
    kReentrant,
};

enum class SyntheticContinuationFault : uint8_t {
    kNone,
    kUnauthorizedTarget,
    kInvalidStack,
    kInvalidFlags,
    kStaleDispatch,
    kStaleMemory,
};

bool ArmSyntheticWorker(const SyntheticWorkerAuthorization& authorization);
// Binds the authorization to one prepared Thread. The worker that eventually
// starts on that thread claims it; no other thread can.
bool ArmSyntheticWorkerForThread(Thread* thread, const SyntheticWorkerAuthorization& authorization,
                                 SyntheticWorkerHandle* handle = nullptr);
bool RevokeSyntheticWorker(const SyntheticWorkerHandle& handle);
SyntheticWorkerHandle SyntheticWorkerIdentityForTest();
uint32_t SyntheticWorkerCountForTest();
void DisarmSyntheticWorker();
SyntheticWorkerClaim ClaimSyntheticWorker(uint32_t target, uint32_t context, uint64_t dispatch_generation);
bool SyntheticWorkerClaimHeld();
bool SyntheticWorkerContinuationAuthorized(uint32_t target, uint64_t dispatch_generation);
SyntheticContinuationStatus LastSyntheticContinuationStatusForTest();
void SetLastSyntheticContinuationStatusForTest(SyntheticContinuationStatus status);
void SetSyntheticContinuationFaultForTest(SyntheticContinuationFault fault);
SyntheticContinuationFault SyntheticContinuationFaultForTest();
using SyntheticContinuationAdmissionHook = void (*)();
void SetSyntheticContinuationAdmissionHookForTest(SyntheticContinuationAdmissionHook hook);
void CallSyntheticContinuationAdmissionHookForTest();
bool SyntheticWorkerHasNestedCallback();
bool SyntheticWorkerAuthorizesNestedCallback(uint32_t target, uint64_t dispatch_generation);

bool ArmSyntheticSession(const SyntheticSessionPolicy& policy);
// Binds the session to one worker's claim instead of the single armed worker.
bool ArmSyntheticSessionForThread(Thread* thread, const SyntheticSessionPolicy& policy);
void DisarmSyntheticSession();
bool SyntheticSessionArmed();
SyntheticCallbackAdmission AdmitSyntheticCallback(SyntheticCallbackEntry entry_kind, uint32_t slot_address,
                                                  uint32_t target, uint32_t call_pc, uint64_t dispatch_generation,
                                                  uint32_t esp, uint32_t* expected_esp);
void ReleaseSyntheticCallbackAdmission();
uint32_t SyntheticSessionDepthForTest();
uint32_t SyntheticSessionAdmissionsForTest();
uint32_t SyntheticSessionMaxDepthForTest();
using SyntheticSessionAdmissionHook = void (*)();
void SetSyntheticSessionAdmissionHookForTest(SyntheticSessionAdmissionHook hook);
uint32_t SyntheticSessionAdmissionsForThread(Thread* worker);
uint32_t SyntheticSessionDepthForThread(Thread* worker);
uint32_t SyntheticSessionMaxDepthForThread(Thread* worker);
using SyntheticWorkerPreCallbackHook = void (*)();
void SetSyntheticWorkerPreCallbackHookForTest(SyntheticWorkerPreCallbackHook hook);
void CallSyntheticWorkerPreCallbackHookForTest();

// Called on the creating worker's host thread once the child Thread is prepared
// and armed, before StartThread. Returning false cancels the child.
using SyntheticChildPreparedHook = bool (*)(Thread* child);
void SetSyntheticChildPreparedHookForTest(SyntheticChildPreparedHook hook);

// Ordinal-255 synthetic bridge. descriptor points at the 16-byte child record.
uint32_t CreateSyntheticChildThread(uint32_t handle_out, uint32_t stack_size, uint32_t tls_size, uint32_t id_out,
                                    uint32_t start_routine, uint32_t start_context, uint32_t suspended,
                                    uint32_t system_routine, uint32_t descriptor);
void SetSyntheticWorkerThreadIdentityForTest(Thread* thread);
bool SyntheticWorkerExitMatches(uint32_t code);
void FinishSyntheticWorker();
bool SyntheticWorkerArmed();

#endif

}  // namespace kernel
