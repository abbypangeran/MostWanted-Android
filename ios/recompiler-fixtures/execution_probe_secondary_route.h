#pragma once

#include <cstddef>
#include <cstdint>

// Phase 2C-31 reviewed secondary-thread entry.  The authentic
// PsCreateSystemThreadEx call prepares an inactive thread whose system routine
// is 0x002B5C0F and whose start routine is 0x002B38A2.  This route covers only
// the first two system-routine instructions and stops before the unreviewed
// SEH-prolog call at 0x002B5C16.
//
// The start routine and both unreviewed callees are mapped as trap-only
// boundary functions so callback-ticket validation can resolve them without
// allowing them to execute.  Their addresses are real; the fixture bytes are
// synthetic encodings of the reviewed instruction shapes.
namespace execution_probe_secondary {

enum class StepKind : uint8_t { kSequential, kStop, kForbidden };

struct RouteStep {
    uint32_t pc;
    uint8_t length;
    StepKind kind;
    uint32_t next;
};

// pc, length, kind, next
#define EXECUTION_PROBE_SECONDARY_ROUTE(X)  \
    X(0x002B5C0F, 2, kSequential, 0x002B5C11) \
    X(0x002B5C11, 5, kSequential, 0x002B5C16) \
    X(0x002B5C16, 5, kStop, 0)

inline constexpr RouteStep kRoute[] = {
#define EXECUTION_PROBE_SECONDARY_ROUTE_STEP(pc, len, kind, next) {pc, len, StepKind::kind, next},
    EXECUTION_PROBE_SECONDARY_ROUTE(EXECUTION_PROBE_SECONDARY_ROUTE_STEP)
#undef EXECUTION_PROBE_SECONDARY_ROUTE_STEP
};
inline constexpr size_t kRouteCount = sizeof(kRoute) / sizeof(kRoute[0]);

inline constexpr uint32_t kEntry = 0x002B5C0F;
inline constexpr uint32_t kSystemRoutine = 0x002B5C0F;
inline constexpr uint32_t kStartRoutine = 0x002B38A2;
inline constexpr uint32_t kStartContext = 0;
inline constexpr uint32_t kStackHelper = 0x002B58B8;  // first callee of the start routine
inline constexpr uint32_t kSehHelper = 0x00319804;    // first callee of the system routine

// Trap-only boundaries.  These functions must exist in the generated mapping so
// the kSystemRoutine argument contract can resolve the start routine, but no
// instruction at any of these PCs may run.
inline constexpr uint32_t kTrapPcs[] = {kStartRoutine, kStackHelper, kSehHelper};
inline constexpr size_t kTrapCount = sizeof(kTrapPcs) / sizeof(kTrapPcs[0]);

inline constexpr uint32_t kStopBefore = 0x002B5C16;     // call 0x00319804, pre-instruction stop
inline constexpr uint32_t kStopCallReturn = 0x002B5C1B; // the call's return PC, never reached
inline constexpr uint32_t kStartCallReturn = 0x002B38A7;

// Authentic entry values the system routine pushes before the SEH prolog.
inline constexpr uint32_t kFirstPushValue = 0x00000018;
inline constexpr uint32_t kSecondPushValue = 0x003ED5B8;

// The host callback frame is written below the prepared thread's initial ESP:
// [esp] = 0xFFFFFFF0, [esp+4] = start routine, [esp+8] = start context.
inline constexpr uint32_t kHostCallbackReturn = 0xFFFFFFF0;
inline constexpr uint32_t kCallbackFrameBytes = 12;
inline constexpr uint32_t kGuestStackBytes = 8;  // the route's two pushes

inline constexpr uint32_t kMaximumInstructions = 2;
inline constexpr uint32_t kMaximumMemoryOperations = 8;
inline constexpr uint32_t kMaximumTraceRecords = 2;
inline constexpr uint32_t kMaximumDirectCalls = 0;
inline constexpr uint32_t kMaximumKernelCalls = 0;
inline constexpr uint32_t kMaximumImportReads = 0;

// Synthetic fixture layout.  One executable section backs the startup PCs and
// the SEH helper's first instruction, so the reviewed call target is
// file-backed without mapping any of the helper's later instructions.
inline constexpr uint32_t kImageBase = 0x002B0000;
inline constexpr uint32_t kImageSize = 0x6B000;
inline constexpr uint32_t kHeaderSize = 0x1000;
inline constexpr uint32_t kSectionVa = 0x002B1000;
inline constexpr uint32_t kSectionSize = 0x6A000;
inline constexpr uint32_t kSectionRaw = 0x1000;
inline constexpr uint32_t kDataPage = 0x00010000;
inline constexpr uint32_t kHandleOut = 0x00010000;
inline constexpr uint32_t kIdOut = 0x00010008;
inline constexpr uint32_t kStackRequest = 0x10000;
inline constexpr uint32_t kTlsSize = 0x94;

// Reviewed text of the executed route and of the three trap-only boundaries.
inline constexpr const char* kExpectedRouteText[] = {
    "push 0x18",
    "push 0x3ED5B8",
    "call 0x00319804",
};
inline constexpr size_t kExpectedRouteTextCount =
    sizeof(kExpectedRouteText) / sizeof(kExpectedRouteText[0]);

inline constexpr const char* kExpectedTrapText[] = {
    "call 0x002B58B8",          // 002B38A2 start routine entry
    "mov eax, [0x003C9E34]",    // 002B58B8 first callee
    "push 0x319858",            // 00319804 SEH prolog
};

// FNV-1a over the 12 reviewed route bytes (the trap boundaries are validated by
// their own text and call target, not by the route hash).
inline constexpr uint32_t kExpectedRouteBytes = 12;
inline constexpr uint64_t kExpectedRouteHash = 0x59728DC843F34996ULL;

}  // namespace execution_probe_secondary
