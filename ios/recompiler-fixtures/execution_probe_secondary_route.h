#pragma once

#include <cstddef>
#include <cstdint>

// Phase 2C-32 reviewed secondary-thread segment.  It extends Phase 2C-31's
// system-routine entry through the authentic SEH prolog at 0x00319804, back to
// 0x002B5C1B, through the system routine's TLS initialisation, and stops before
// the unreviewed helper call at 0x002B597D.
//
// The route is in execution order, not address order: the direct call at
// 0x002B5C16 enters the prolog, the prolog's RET returns to 0x002B5C1B, and the
// body then runs to the TLS zero-fill branch.  Both outcomes of that branch are
// reviewed.  The start routine, its first callee and the next system helper are
// trap-only boundaries.
namespace execution_probe_secondary {

enum class StepKind : uint8_t { kSequential, kCall, kConditional, kRet, kStop, kForbidden };

struct RouteStep {
    uint32_t pc;
    uint8_t length;
    StepKind kind;
    uint32_t next;       // fallthrough successor or call/return target
    uint32_t alternate;  // conditional taken target, 0 otherwise
};

// pc, length, kind, next, alternate
#define EXECUTION_PROBE_SECONDARY_ROUTE(X)                                                     \
    X(0x002B5C0F, 2, kSequential, 0x002B5C11, 0)                                               \
    X(0x002B5C11, 5, kSequential, 0x002B5C16, 0)                                               \
    X(0x002B5C16, 5, kCall, 0x00319804, 0)                                                     \
    X(0x00319804, 5, kSequential, 0x00319809, 0)                                               \
    X(0x00319809, 6, kSequential, 0x0031980F, 0)                                               \
    X(0x0031980F, 1, kSequential, 0x00319810, 0)                                               \
    X(0x00319810, 4, kSequential, 0x00319814, 0)                                               \
    X(0x00319814, 4, kSequential, 0x00319818, 0)                                               \
    X(0x00319818, 4, kSequential, 0x0031981C, 0)                                               \
    X(0x0031981C, 2, kSequential, 0x0031981E, 0)                                               \
    X(0x0031981E, 1, kSequential, 0x0031981F, 0)                                               \
    X(0x0031981F, 1, kSequential, 0x00319820, 0)                                               \
    X(0x00319820, 1, kSequential, 0x00319821, 0)                                               \
    X(0x00319821, 3, kSequential, 0x00319824, 0)                                               \
    X(0x00319824, 3, kSequential, 0x00319827, 0)                                               \
    X(0x00319827, 1, kSequential, 0x00319828, 0)                                               \
    X(0x00319828, 3, kSequential, 0x0031982B, 0)                                               \
    X(0x0031982B, 7, kSequential, 0x00319832, 0)                                               \
    X(0x00319832, 3, kSequential, 0x00319835, 0)                                               \
    X(0x00319835, 3, kSequential, 0x00319838, 0)                                               \
    X(0x00319838, 6, kSequential, 0x0031983E, 0)                                               \
    X(0x0031983E, 1, kRet, 0x002B5C1B, 0)                                                      \
    X(0x002B5C1B, 4, kSequential, 0x002B5C1F, 0)                                               \
    X(0x002B5C1F, 6, kSequential, 0x002B5C25, 0)                                               \
    X(0x002B5C25, 3, kSequential, 0x002B5C28, 0)                                               \
    X(0x002B5C28, 3, kSequential, 0x002B5C2B, 0)                                               \
    X(0x002B5C2B, 3, kSequential, 0x002B5C2E, 0)                                               \
    X(0x002B5C2E, 3, kSequential, 0x002B5C31, 0)                                               \
    X(0x002B5C31, 3, kSequential, 0x002B5C34, 0)                                               \
    X(0x002B5C34, 6, kSequential, 0x002B5C3A, 0)                                               \
    X(0x002B5C3A, 6, kSequential, 0x002B5C40, 0)                                               \
    X(0x002B5C40, 2, kSequential, 0x002B5C42, 0)                                               \
    X(0x002B5C42, 3, kSequential, 0x002B5C45, 0)                                               \
    X(0x002B5C45, 2, kSequential, 0x002B5C47, 0)                                               \
    X(0x002B5C47, 2, kSequential, 0x002B5C49, 0)                                               \
    X(0x002B5C49, 2, kSequential, 0x002B5C4B, 0)                                               \
    X(0x002B5C4B, 3, kSequential, 0x002B5C4E, 0)                                               \
    X(0x002B5C4E, 2, kSequential, 0x002B5C50, 0)                                               \
    X(0x002B5C50, 2, kSequential, 0x002B5C52, 0)                                               \
    X(0x002B5C52, 3, kSequential, 0x002B5C55, 0)                                               \
    X(0x002B5C55, 2, kSequential, 0x002B5C57, 0)                                               \
    X(0x002B5C57, 6, kSequential, 0x002B5C5D, 0)                                               \
    X(0x002B5C5D, 2, kSequential, 0x002B5C5F, 0)                                               \
    X(0x002B5C5F, 2, kConditional, 0x002B5C61, 0x002B5C74)                                     \
    X(0x002B5C61, 2, kSequential, 0x002B5C63, 0)                                               \
    X(0x002B5C63, 3, kSequential, 0x002B5C66, 0)                                               \
    X(0x002B5C66, 2, kSequential, 0x002B5C68, 0)                                               \
    X(0x002B5C68, 3, kSequential, 0x002B5C6B, 0)                                               \
    X(0x002B5C6B, 2, kSequential, 0x002B5C6D, 0)                                               \
    X(0x002B5C6D, 2, kSequential, 0x002B5C6F, 0)                                               \
    X(0x002B5C6F, 3, kSequential, 0x002B5C72, 0)                                               \
    X(0x002B5C72, 2, kSequential, 0x002B5C74, 0)                                               \
    X(0x002B5C74, 2, kSequential, 0x002B5C76, 0)                                               \
    X(0x002B5C76, 5, kStop, 0, 0)

inline constexpr RouteStep kRoute[] = {
#define EXECUTION_PROBE_SECONDARY_ROUTE_STEP(pc, len, kind, next, alt) {pc, len, StepKind::kind, next, alt},
    EXECUTION_PROBE_SECONDARY_ROUTE(EXECUTION_PROBE_SECONDARY_ROUTE_STEP)
#undef EXECUTION_PROBE_SECONDARY_ROUTE_STEP
};
inline constexpr size_t kRouteCount = sizeof(kRoute) / sizeof(kRoute[0]);

inline constexpr uint32_t kEntry = 0x002B5C0F;
inline constexpr uint32_t kSystemRoutine = 0x002B5C0F;
inline constexpr uint32_t kStartRoutine = 0x002B38A2;
inline constexpr uint32_t kStartContext = 0;
inline constexpr uint32_t kSehHelper = 0x00319804;
inline constexpr uint32_t kSehHandler = 0x00319858;
inline constexpr uint32_t kSehRetInstruction = 0x0031983E;
inline constexpr uint32_t kSehReturn = 0x002B5C1B;
inline constexpr uint32_t kStackHelper = 0x002B58B8;
inline constexpr uint32_t kNextHelper = 0x002B597D;
inline constexpr uint32_t kStopBefore = 0x002B5C76;    // call 0x002B597D, pre-instruction stop
inline constexpr uint32_t kStopCallReturn = 0x002B5C7B;
inline constexpr uint32_t kStartCallReturn = 0x002B38A7;
inline constexpr uint32_t kTlsFillBranch = 0x002B5C5F;      // jz 0x002B5C74
inline constexpr uint32_t kTlsFillFallthrough = 0x002B5C61;
inline constexpr uint32_t kTlsFillTarget = 0x002B5C74;

// Authentic entry values the system routine pushes before the SEH prolog.
inline constexpr uint32_t kFirstPushValue = 0x00000018;
inline constexpr uint32_t kSecondPushValue = 0x003ED5B8;
inline constexpr uint32_t kScopeTable = 0x003ED5B8;

// TLS initialisation sources and destination.
inline constexpr uint32_t kTlsGlobalSource = 0x003ED3EC;
inline constexpr uint32_t kTlsGlobalEnd = 0x003ED3F0;
inline constexpr uint32_t kTlsGlobalZeroFill = 0x003ED3FC;
inline constexpr uint32_t kTlsBlockSize = 0x94;
inline constexpr uint32_t kTlsPointerBytes = 4;

// The host callback frame is written below the prepared thread's initial ESP:
// [esp] = 0xFFFFFFF0, [esp+4] = start routine, [esp+8] = start context.
inline constexpr uint32_t kHostCallbackReturn = 0xFFFFFFF0;
inline constexpr uint32_t kCallbackFrameBytes = 12;

// Trap-only boundaries: mapped generated functions that must never run.
inline constexpr uint32_t kTrapPcs[] = {kStartRoutine, kStackHelper, kNextHelper};
inline constexpr size_t kTrapCount = sizeof(kTrapPcs) / sizeof(kTrapPcs[0]);

inline constexpr uint32_t kMaximumInstructions = 64;
inline constexpr uint32_t kMaximumMemoryOperations = 256;
inline constexpr uint32_t kMaximumTraceRecords = kMaximumInstructions;
inline constexpr uint32_t kMaximumDirectCalls = 1;
inline constexpr uint32_t kMaximumKernelCalls = 0;
inline constexpr uint32_t kMaximumImportReads = 0;

// Synthetic fixture layout.  One executable section backs the startup PCs and
// the SEH helper.  The TLS globals and the optional copy source are separate
// committed pages with their own reviewed permissions.
inline constexpr uint32_t kImageBase = 0x002B0000;
inline constexpr uint32_t kImageSize = 0x6B000;
inline constexpr uint32_t kHeaderSize = 0x1000;
inline constexpr uint32_t kSectionVa = 0x002B1000;
inline constexpr uint32_t kSectionSize = 0x6A000;
inline constexpr uint32_t kSectionRaw = 0x1000;
inline constexpr uint32_t kRdataPage = 0x003ED000;
inline constexpr uint32_t kDataPage = 0x00010000;
inline constexpr uint32_t kHandleOut = 0x00010000;
inline constexpr uint32_t kIdOut = 0x00010008;
inline constexpr uint32_t kSourcePage = 0x00020000;
inline constexpr uint32_t kSourceSize = 0x1000;
inline constexpr uint32_t kStackRequest = 0x10000;
inline constexpr uint32_t kTlsSize = 0x94;

// The authentic Image has no TLS copy and zero-fills 0x90 bytes.  The synthetic
// copy scenario uses 0x12 bytes and a 0x7E-byte zero-fill so both REP forms and
// both remainders run.
inline constexpr uint32_t kAuthenticCopyBytes = 0x00;
inline constexpr uint32_t kAuthenticZeroFillBytes = 0x90;
inline constexpr uint32_t kSyntheticCopyBytes = 0x12;
inline constexpr uint32_t kSyntheticZeroFillBytes = 0x7E;

inline constexpr uint32_t kExpectedPrepareInstructions = 53;
inline constexpr uint32_t kExpectedSkippedFillInstructions = 45;

// Reviewed text of the executed route (execution order) and of the three
// trap-only boundaries.  The real-image validator compares every decode.
inline constexpr const char* kExpectedRouteText[] = {
    "push 0x18",                              // 002B5C0F
    "push 0x3ED5B8",                          // 002B5C11
    "call 0x00319804",                        // 002B5C16
    "push 0x319858",                          // 00319804
    "mov eax, fs:[0x00000000]",               // 00319809
    "push eax",                               // 0031980F
    "mov eax, [esp+0x10]",                    // 00319810
    "mov [esp+0x10], ebp",                    // 00319814
    "lea ebp, [esp+0x10]",                    // 00319818
    "sub esp, eax",                           // 0031981C
    "push ebx",                               // 0031981E
    "push esi",                               // 0031981F
    "push edi",                               // 00319820
    "mov eax, [ebp-0x08]",                    // 00319821
    "mov [ebp-0x18], esp",                    // 00319824
    "push eax",                               // 00319827
    "mov eax, [ebp-0x04]",                    // 00319828
    "mov dword ptr [ebp-0x04], 0xFFFFFFFF",   // 0031982B
    "mov [ebp-0x08], eax",                    // 00319832
    "lea eax, [ebp-0x10]",                    // 00319835
    "mov fs:[0x00000000], eax",               // 00319838
    "ret",                                    // 0031983E
    "and dword ptr [ebp-0x04], 0x00",         // 002B5C1B
    "mov eax, fs:[0x00000028]",               // 002B5C1F
    "mov [ebp-0x20], eax",                    // 002B5C25
    "mov edx, [eax+0x28]",                    // 002B5C28
    "add edx, 0x04",                          // 002B5C2B
    "mov [ebp-0x24], edx",                    // 002B5C2E
    "mov [edx-0x04], edx",                    // 002B5C31
    "mov ebx, [0x003ED3F0]",                  // 002B5C34
    "mov esi, [0x003ED3EC]",                  // 002B5C3A
    "sub ebx, esi",                           // 002B5C40
    "mov [ebp-0x28], ebx",                    // 002B5C42
    "mov ecx, ebx",                           // 002B5C45
    "mov edi, edx",                           // 002B5C47
    "mov eax, ecx",                           // 002B5C49
    "shr ecx, 0x02",                          // 002B5C4B
    "rep movsd",                              // 002B5C4E
    "mov ecx, eax",                           // 002B5C50
    "and ecx, 0x03",                          // 002B5C52
    "rep movsb",                              // 002B5C55
    "mov ecx, [0x003ED3FC]",                  // 002B5C57
    "test ecx, ecx",                          // 002B5C5D
    "jz 0x002B5C74",                          // 002B5C5F
    "xor eax, eax",                           // 002B5C61
    "lea edi, [ebx+edx*1]",                   // 002B5C63
    "mov edx, ecx",                           // 002B5C66
    "shr ecx, 0x02",                          // 002B5C68
    "rep stosd",                              // 002B5C6B
    "mov ecx, edx",                           // 002B5C6D
    "and ecx, 0x03",                          // 002B5C6F
    "rep stosb",                              // 002B5C72
    "push 0x01",                              // 002B5C74
    "call 0x002B597D",                        // 002B5C76, stop
};
inline constexpr size_t kExpectedRouteTextCount =
    sizeof(kExpectedRouteText) / sizeof(kExpectedRouteText[0]);

inline constexpr const char* kExpectedTrapText[] = {
    "call 0x002B58B8",          // 002B38A2 start routine entry
    "mov eax, [0x003C9E34]",    // 002B58B8 first callee
    "push ebx",                 // 002B597D next system helper
};

// FNV-1a over the 167 reviewed route bytes in execution order.
inline constexpr uint32_t kExpectedRouteBytes = 167;
inline constexpr uint64_t kExpectedRouteHash = 0xEF9505E0CE8AE719ULL;

}  // namespace execution_probe_secondary
