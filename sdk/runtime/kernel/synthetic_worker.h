#pragma once

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

enum class SyntheticWorkerClaim : uint8_t { kNotAuthorized, kClaimed, kRejected };

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
bool SyntheticWorkerExitMatches(uint32_t code);
void FinishSyntheticWorker();
bool SyntheticWorkerArmed();

using SyntheticWorkerPreCallbackHook = void (*)();
void SetSyntheticWorkerPreCallbackHookForTest(SyntheticWorkerPreCallbackHook hook);
void CallSyntheticWorkerPreCallbackHookForTest();

#endif

}  // namespace kernel
