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

struct Thread;

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
void SetSyntheticWorkerThreadIdentityForTest(Thread* thread);
bool SyntheticWorkerExitMatches(uint32_t code);
void FinishSyntheticWorker();
bool SyntheticWorkerArmed();

using SyntheticWorkerPreCallbackHook = void (*)();
void SetSyntheticWorkerPreCallbackHookForTest(SyntheticWorkerPreCallbackHook hook);
void CallSyntheticWorkerPreCallbackHookForTest();

#endif

}  // namespace kernel
