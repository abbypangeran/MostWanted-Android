#pragma once

#include <cstddef>
#include <cstdint>

// Phase 2C-15 audited startup prefix, extended by Phase 2C-18 by one
// instruction (the ordinal-255 import call at 0x002B5CE0), by Phase 2C-20
// through the helper's successful return path, by Phase 2C-22 through its
// RET 0x18, by Phase 2C-24 through the caller's handle check, and by Phase
// 2C-26 through the NtClose wrapper, stopping before the caller's own RET at
// 0x002B3987.
//
// The route mirrors the instruction shapes recorded in PORT_STATUS.md for
// 0x002B3916 -> 0x002B396A -> 0x002B5CA7 -> 0x002B5CE0 -> the success arm at
// 0x002B5D0E..0x002B5D12 -> 0x002B396F..0x002B3986.  The synthetic fixture
// builder and the execution probe both consume this table, so the allowed PCs,
// lengths, transitions and call targets stay in one place.  The addresses are
// real; the fixture bytes and their data values are synthetic and are not
// evidence about the retail image.
namespace execution_probe {

// kKernelCall is the reviewed indirect call through the import slot; it is
// admissible only through the test-only ordinal-255 bridge.  kForbidden marks a
// PC that must have a hook but must never be reached: the debug arm of the
// post-import branch is outside this experiment.  kStop is a pre-instruction
// boundary: the hook records the returning state and stops before the PC runs.
enum class StepKind : uint8_t { kSequential, kConditional, kCall, kKernelCall, kForbidden, kStop };

struct RouteStep {
    uint32_t pc;
    uint8_t length;
    StepKind kind;
    uint32_t next;       // fallthrough successor, call target, or the forbidden stop PC
    uint32_t alternate;  // conditional taken target, 0 otherwise
};

// pc, length, kind, next, alternate
#define EXECUTION_PROBE_ROUTE(X)                                                             \
    X(0x002B3916, 6, kSequential, 0x002B391C, 0)                                             \
    X(0x002B391C, 5, kSequential, 0x002B3921, 0)                                             \
    X(0x002B3921, 2, kSequential, 0x002B3923, 0)                                             \
    X(0x002B3923, 5, kSequential, 0x002B3928, 0)                                             \
    X(0x002B3928, 2, kSequential, 0x002B392A, 0)                                             \
    X(0x002B392A, 2, kConditional, 0x002B392C, 0x002B392E)                                   \
    X(0x002B392C, 2, kSequential, 0x002B392E, 0)                                             \
    X(0x002B392E, 5, kSequential, 0x002B3933, 0)                                             \
    X(0x002B3933, 6, kSequential, 0x002B3939, 0)                                             \
    X(0x002B3939, 6, kSequential, 0x002B393F, 0)                                             \
    X(0x002B393F, 1, kSequential, 0x002B3940, 0)                                             \
    X(0x002B3940, 4, kSequential, 0x002B3944, 0)                                             \
    X(0x002B3944, 1, kSequential, 0x002B3945, 0)                                             \
    X(0x002B3945, 3, kSequential, 0x002B3948, 0)                                             \
    X(0x002B3948, 2, kSequential, 0x002B394A, 0)                                             \
    X(0x002B394A, 1, kSequential, 0x002B394B, 0)                                             \
    X(0x002B394B, 3, kSequential, 0x002B394E, 0)                                             \
    X(0x002B394E, 5, kSequential, 0x002B3953, 0)                                             \
    X(0x002B3953, 1, kSequential, 0x002B3954, 0)                                             \
    X(0x002B3954, 2, kSequential, 0x002B3956, 0)                                             \
    X(0x002B3956, 6, kSequential, 0x002B395C, 0)                                             \
    X(0x002B395C, 2, kSequential, 0x002B395E, 0)                                             \
    X(0x002B395E, 1, kSequential, 0x002B395F, 0)                                             \
    X(0x002B395F, 1, kSequential, 0x002B3960, 0)                                             \
    X(0x002B3960, 1, kSequential, 0x002B3961, 0)                                             \
    X(0x002B3961, 5, kSequential, 0x002B3966, 0)                                             \
    X(0x002B3966, 1, kSequential, 0x002B3967, 0)                                             \
    X(0x002B3967, 1, kSequential, 0x002B3968, 0)                                             \
    X(0x002B3968, 2, kSequential, 0x002B396A, 0)                                             \
    X(0x002B396A, 5, kCall, 0x002B5CA7, 0)                                                   \
    X(0x002B5CA7, 1, kSequential, 0x002B5CA8, 0)                                             \
    X(0x002B5CA8, 2, kSequential, 0x002B5CAA, 0)                                             \
    X(0x002B5CAA, 3, kSequential, 0x002B5CAD, 0)                                             \
    X(0x002B5CAD, 2, kSequential, 0x002B5CAF, 0)                                             \
    X(0x002B5CAF, 2, kConditional, 0x002B5CB1, 0x002B5CB6)                                   \
    X(0x002B5CB1, 5, kSequential, 0x002B5CB6, 0)                                             \
    X(0x002B5CB6, 3, kSequential, 0x002B5CB9, 0)                                             \
    X(0x002B5CB9, 5, kSequential, 0x002B5CBE, 0)                                             \
    X(0x002B5CBE, 2, kSequential, 0x002B5CC0, 0)                                             \
    X(0x002B5CC0, 3, kSequential, 0x002B5CC3, 0)                                             \
    X(0x002B5CC3, 6, kSequential, 0x002B5CC9, 0)                                             \
    X(0x002B5CC9, 1, kSequential, 0x002B5CCA, 0)                                             \
    X(0x002B5CCA, 3, kSequential, 0x002B5CCD, 0)                                             \
    X(0x002B5CCD, 3, kSequential, 0x002B5CD0, 0)                                             \
    X(0x002B5CD0, 3, kSequential, 0x002B5CD3, 0)                                             \
    X(0x002B5CD3, 6, kSequential, 0x002B5CD9, 0)                                             \
    X(0x002B5CD9, 1, kSequential, 0x002B5CDA, 0)                                             \
    X(0x002B5CDA, 2, kSequential, 0x002B5CDC, 0)                                             \
    X(0x002B5CDC, 3, kSequential, 0x002B5CDF, 0)                                             \
    X(0x002B5CDF, 1, kSequential, 0x002B5CE0, 0)                                             \
    X(0x002B5CE0, 6, kKernelCall, 0x002B5CE6, 0)                                             \
    X(0x002B5CE6, 2, kSequential, 0x002B5CE8, 0)                                             \
    X(0x002B5CE8, 2, kSequential, 0x002B5CEA, 0)                                             \
    X(0x002B5CEA, 2, kConditional, 0x002B5CEC, 0x002B5D0E)                                   \
    X(0x002B5CEC, 8, kForbidden, 0, 0)                                                       \
    X(0x002B5D0E, 3, kSequential, 0x002B5D11, 0)                                             \
    X(0x002B5D11, 1, kSequential, 0x002B5D12, 0)                                             \
    X(0x002B5D12, 3, kSequential, 0x002B396F, 0)                                             \
    X(0x002B396F, 2, kSequential, 0x002B3971, 0)                                             \
    X(0x002B3971, 2, kSequential, 0x002B3973, 0)                                             \
    X(0x002B3973, 2, kConditional, 0x002B3975, 0x002B397F)                                   \
    X(0x002B3975, 1, kForbidden, 0, 0)                                                       \
    X(0x002B397F, 1, kSequential, 0x002B3980, 0)                                             \
    X(0x002B3980, 5, kCall, 0x002B2759, 0)                                                   \
    X(0x002B3985, 1, kSequential, 0x002B3986, 0)                                             \
    X(0x002B3986, 1, kSequential, 0x002B3987, 0)                                             \
    X(0x002B3987, 1, kStop, 0, 0)                                                            \
    X(0x002B2759, 4, kSequential, 0x002B275D, 0)                                             \
    X(0x002B275D, 6, kKernelCall, 0x002B2763, 0)                                             \
    X(0x002B2763, 2, kSequential, 0x002B2765, 0)                                             \
    X(0x002B2765, 2, kConditional, 0x002B2767, 0x002B276C)                                   \
    X(0x002B2767, 2, kSequential, 0x002B2769, 0)                                             \
    X(0x002B2769, 1, kSequential, 0x002B276A, 0)                                             \
    X(0x002B276A, 2, kSequential, 0x002B2774, 0)                                             \
    X(0x002B276C, 1, kForbidden, 0, 0)                                                       \
    X(0x002B2774, 3, kSequential, 0x002B3985, 0)                                             \
    X(0x002B55EE, 1, kForbidden, 0, 0)                                                       \
    X(0x002B3826, 4, kForbidden, 0, 0)

inline constexpr RouteStep kRoute[] = {
#define EXECUTION_PROBE_ROUTE_STEP(pc, len, kind, next, alt) {pc, len, StepKind::kind, next, alt},
    EXECUTION_PROBE_ROUTE(EXECUTION_PROBE_ROUTE_STEP)
#undef EXECUTION_PROBE_ROUTE_STEP
};
inline constexpr size_t kRouteCount = sizeof(kRoute) / sizeof(kRoute[0]);

inline constexpr uint32_t kEntry = 0x002B3916;
inline constexpr uint32_t kCallSite = 0x002B396A;
inline constexpr uint32_t kCallTarget = 0x002B5CA7;
inline constexpr uint32_t kImportCall = 0x002B5CE0;
inline constexpr uint32_t kImportReturn = 0x002B5CE6;
inline constexpr uint32_t kPostImportBranch = 0x002B5CEA;  // jnl successor of the kernel status
inline constexpr uint32_t kDebugPath = 0x002B5CEC;         // forbidden arm, a trap-only boundary function
inline constexpr uint32_t kHandleRead = 0x002B5D0E;        // mov eax, [ebp+0x0C]
inline constexpr uint32_t kLastInstruction = 0x002B3986;   // pop esi, executed
inline constexpr uint32_t kStopBefore = 0x002B3987;        // caller ret, never executed
inline constexpr uint32_t kImportSlot = 0x003C9E68;
inline constexpr uint32_t kImportOrdinal = 255;
inline constexpr uint32_t kImportEncoding = 0x800000FF;

// the caller's arm after the helper returns: it moves the published handle to
// EDI, tests it against ESI (zero), and calls the NtClose wrapper only when the
// handle is non-zero.  The wrapper's NtClose call is the first reviewed kernel
// service; the caller's own RET is the new mandatory stop.
inline constexpr uint32_t kCallerBranch = 0x002B3973;         // jnz 0x002B397F
inline constexpr uint32_t kCallerOffPath = 0x002B3975;        // zero-handle arm
inline constexpr uint32_t kCallerContinuation = 0x002B397F;   // push edi
inline constexpr uint32_t kCloseCall = 0x002B3980;            // call 0x002B2759
inline constexpr uint32_t kCloseContinue = 0x002B3985;        // pop edi, wrapper return
inline constexpr uint32_t kCloseTarget = 0x002B2759;          // NtClose wrapper
inline constexpr uint32_t kOffPathCall = 0x002B397A;          // call 0x002B55EE, off path
inline constexpr uint32_t kOffPathTarget = 0x002B55EE;
inline constexpr uint32_t kCloseImportCall = 0x002B275D;      // call [0x003C9D4C]
inline constexpr uint32_t kCloseImportSlot = 0x003C9D4C;
inline constexpr uint32_t kCloseOrdinal = 187;                // NtClose
inline constexpr uint32_t kCloseImportEncoding = 0x800000BB;
inline constexpr uint32_t kCloseImportReturn = 0x002B2763;
inline constexpr uint32_t kCloseStatusTest = 0x002B2763;      // test eax, eax
inline constexpr uint32_t kCloseBranch = 0x002B2765;          // jl 0x002B276C
inline constexpr uint32_t kCloseSuccess = 0x002B2767;         // xor eax, eax
inline constexpr uint32_t kCloseRet = 0x002B2774;             // ret 0x04
inline constexpr uint32_t kCloseErrorPath = 0x002B276C;       // forbidden
inline constexpr uint32_t kCloseErrorCall = 0x002B276D;       // call 0x002B3826, forbidden
inline constexpr uint32_t kCloseErrorTarget = 0x002B3826;     // forbidden stub

// authentic arguments the reviewed helper pushes; both routines are data here
// and are never entered.
inline constexpr uint32_t kStartRoutine = 0x002B38A2;
inline constexpr uint32_t kSystemRoutine = 0x002B5C0F;

// stack the reviewed route consumes between the initial ESP and 0x002B5CE0.
inline constexpr uint32_t kImportStackBytes = 0x50;
inline constexpr uint32_t kImportArg0Offset = 0x34;  // lea eax, [ebp+0x0C] with ebp = esp + 0x28

// the helper's RET 0x18 contract: executed, returning to the direct call's
// continuation after popping the caller's six arguments.
inline constexpr uint32_t kRetInstruction = 0x002B5D12;
inline constexpr uint32_t kRetTarget = 0x002B396F;
inline constexpr uint32_t kRetPopBytes = 0x18;
inline constexpr uint32_t kStopInstruction = 0x002B3987;  // caller ret, never executed

// Root completion uses the same non-executable host-return sentinel as
// CallGuest: it is pushed only to give the checked RET a reviewed address to
// validate, and it is never used as a guest target.
inline constexpr uint32_t kRootReturnSentinel = 0xFFFFFFF0;

// one reviewed import call; the observer counts each slot separately.
struct ImportStep {
    uint32_t pc, slot, return_pc, ordinal, encoding;
};
inline constexpr ImportStep kImportSteps[] = {
    {kImportCall, kImportSlot, kImportReturn, kImportOrdinal, kImportEncoding},
    {kCloseImportCall, kCloseImportSlot, kCloseImportReturn, kCloseOrdinal, kCloseImportEncoding},
};
inline constexpr size_t kImportStepCount = sizeof(kImportSteps) / sizeof(kImportSteps[0]);

inline constexpr uint32_t kMaximumInstructions = 80;
inline constexpr uint32_t kMaximumDirectCalls = 2;
inline constexpr uint32_t kMaximumKernelCalls = 2;
inline constexpr uint32_t kMaximumImportReads = 2;
inline constexpr uint32_t kMaximumMemoryOperations = 256;
inline constexpr uint32_t kMaximumTraceRecords = kMaximumInstructions;

// Synthetic fixture layout.  The image base and section are placed so the
// audited addresses land in the real places while the XBE header keeps its own
// page, as the loader expects.
inline constexpr uint32_t kImageBase = 0x002B0000;  // 64 KB aligned, as the image reservation expects
inline constexpr uint32_t kImageSize = 0x6000;
inline constexpr uint32_t kHeaderSize = 0x1000;
inline constexpr uint32_t kSectionVa = 0x002B1000;
inline constexpr uint32_t kSectionSize = 0x5000;
inline constexpr uint32_t kSectionRaw = 0x1000;

inline constexpr uint32_t kDataPage = 0x00010000;
inline constexpr uint32_t kPointerPage = 0x00020000;
inline constexpr uint32_t kAllocationPage = 0x00021000;
inline constexpr uint32_t kGlobalPage = 0x003ED000;
inline constexpr uint32_t kCounterPage = 0x004ED000;
inline constexpr uint32_t kImportPage = 0x003C9000;
inline constexpr uint32_t kStackPage = 0x0050F000;
inline constexpr uint32_t kStackTop = 0x00510000;

inline constexpr uint32_t kGlobalReadA = 0x003ED3FC;
inline constexpr uint32_t kGlobalReadB = 0x003ED3EC;
inline constexpr uint32_t kGlobalLength = 0x003ED3F0;
inline constexpr uint32_t kGlobalStore = 0x003ED3F4;
inline constexpr uint32_t kCounterAddress = 0x004EDC74;
inline constexpr uint32_t kEntryPointer = 0x00010118;
inline constexpr uint32_t kEntryValue = 0x00010108;
inline constexpr uint32_t kHelperValue = 0x00010130;

inline constexpr uint32_t kInstructionPageCount = (kSectionSize + 0xFFF) / 0x1000;

}  // namespace execution_probe
