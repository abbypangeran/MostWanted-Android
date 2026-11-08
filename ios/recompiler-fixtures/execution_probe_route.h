#pragma once

#include <cstddef>
#include <cstdint>

// Phase 2C-15 audited startup prefix.
//
// The route mirrors the instruction shapes recorded in PORT_STATUS.md for
// 0x002B3916 -> 0x002B396A -> 0x002B5CA7, stopping before 0x002B5CE0.  The
// synthetic fixture builder and the execution probe both consume this table, so
// the allowed PCs, lengths, transitions and direct-call target stay in one
// place.  The addresses are real; the fixture bytes and their data values are
// synthetic and are not evidence about the retail image.
namespace execution_probe {

enum class StepKind : uint8_t { kSequential, kConditional, kCall, kStop };

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
    X(0x002B5CDF, 1, kStop, 0x002B5CE0, 0)

inline constexpr RouteStep kRoute[] = {
#define EXECUTION_PROBE_ROUTE_STEP(pc, len, kind, next, alt) {pc, len, StepKind::kind, next, alt},
    EXECUTION_PROBE_ROUTE(EXECUTION_PROBE_ROUTE_STEP)
#undef EXECUTION_PROBE_ROUTE_STEP
};
inline constexpr size_t kRouteCount = sizeof(kRoute) / sizeof(kRoute[0]);

inline constexpr uint32_t kEntry = 0x002B3916;
inline constexpr uint32_t kCallSite = 0x002B396A;
inline constexpr uint32_t kCallTarget = 0x002B5CA7;
inline constexpr uint32_t kLastInstruction = 0x002B5CDF;
inline constexpr uint32_t kStopBefore = 0x002B5CE0;
inline constexpr uint32_t kImportSlot = 0x003C9E68;
inline constexpr uint32_t kImportOrdinal = 255;
inline constexpr uint32_t kImportEncoding = 0x800000FF;

inline constexpr uint32_t kMaximumInstructions = 64;
inline constexpr uint32_t kMaximumDirectCalls = 1;
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
