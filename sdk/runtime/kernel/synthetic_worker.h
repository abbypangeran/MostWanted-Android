#pragma once

#include <cstdint>

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
};

inline constexpr uint32_t kSyntheticWorkerImageIdentity = 0x4C4C5632u;  // "LLV2"

enum class SyntheticWorkerClaim : uint8_t { kNotAuthorized, kClaimed, kRejected };

bool ArmSyntheticWorker(const SyntheticWorkerAuthorization& authorization);
void DisarmSyntheticWorker();
SyntheticWorkerClaim ClaimSyntheticWorker(uint32_t target, uint32_t context, uint64_t dispatch_generation);
bool SyntheticWorkerClaimHeld();
bool SyntheticWorkerExitMatches(uint32_t code);
void FinishSyntheticWorker();
bool SyntheticWorkerArmed();

using SyntheticWorkerPreCallbackHook = void (*)();
void SetSyntheticWorkerPreCallbackHookForTest(SyntheticWorkerPreCallbackHook hook);
void CallSyntheticWorkerPreCallbackHookForTest();

#endif

}  // namespace kernel
