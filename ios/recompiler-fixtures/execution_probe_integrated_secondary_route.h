#pragma once

#include <cstddef>
#include <cstdint>

// Phase 2C-37 extended secondary route.  The Phase 2C-34 prefix is unchanged
// through 002B5C74; the former pre-call stop at 002B5C76 becomes the reviewed
// direct call into 002B597D.  The helper runs the complete empty-list path and
// returns to 002B5C7B, where the mandatory stop prevents the dynamic
// start-routine call at 002B5C7E.
//
// The four loop-body instructions are reviewed and hooked but are only
// reachable on a non-empty list.  002B59A1 is a forbidden pre-instruction
// boundary because its dynamic target is not resolved.
namespace execution_probe_integrated_secondary {

enum class StepKind : uint8_t { kSequential, kConditional, kCall, kKernelCall, kRet, kStop, kForbidden };

struct RouteStep {
    uint32_t pc;
    uint8_t length;
    StepKind kind;
    uint32_t next;       // fallthrough successor, call/return target
    uint32_t alternate;  // conditional taken target, 0 otherwise
};

// pc, length, kind, next, alternate
#define EXECUTION_PROBE_INTEGRATED_SECONDARY_ROUTE(X)                                          \
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
    X(0x002B5C76, 5, kCall, 0x002B597D, 0)                                                     \
    X(0x002B597D, 1, kSequential, 0x002B597E, 0)                                               \
    X(0x002B597E, 1, kSequential, 0x002B597F, 0)                                               \
    X(0x002B597F, 1, kSequential, 0x002B5980, 0)                                               \
    X(0x002B5980, 5, kSequential, 0x002B5985, 0)                                               \
    X(0x002B5985, 1, kSequential, 0x002B5986, 0)                                               \
    X(0x002B5986, 6, kKernelCall, 0x002B598C, 0)                                               \
    X(0x002B598C, 6, kSequential, 0x002B5992, 0)                                               \
    X(0x002B5992, 5, kSequential, 0x002B5997, 0)                                               \
    X(0x002B5997, 2, kSequential, 0x002B59A4, 0)                                               \
    X(0x002B59A4, 2, kSequential, 0x002B59A6, 0)                                               \
    X(0x002B59A6, 2, kConditional, 0x002B59A8, 0x002B5999)                                     \
    X(0x002B59A8, 1, kSequential, 0x002B59A9, 0)                                               \
    X(0x002B59A9, 6, kKernelCall, 0x002B59AF, 0)                                               \
    X(0x002B59AF, 1, kSequential, 0x002B59B0, 0)                                               \
    X(0x002B59B0, 1, kSequential, 0x002B59B1, 0)                                               \
    X(0x002B59B1, 1, kSequential, 0x002B59B2, 0)                                               \
    X(0x002B59B2, 3, kRet, 0x002B5C7B, 0)                                                      \
    X(0x002B5C7B, 3, kStop, 0, 0)                                                              \
    X(0x002B5999, 4, kSequential, 0x002B599D, 0)                                               \
    X(0x002B599D, 2, kSequential, 0x002B599F, 0)                                               \
    X(0x002B599F, 2, kSequential, 0x002B59A1, 0)                                               \
    X(0x002B59A1, 3, kForbidden, 0, 0)

inline constexpr RouteStep kRoute[] = {
#define EXECUTION_PROBE_INTEGRATED_SECONDARY_ROUTE_STEP(pc, len, kind, next, alt) \
    {pc, len, StepKind::kind, next, alt},
    EXECUTION_PROBE_INTEGRATED_SECONDARY_ROUTE(EXECUTION_PROBE_INTEGRATED_SECONDARY_ROUTE_STEP)
#undef EXECUTION_PROBE_INTEGRATED_SECONDARY_ROUTE_STEP
};
inline constexpr size_t kRouteCount = sizeof(kRoute) / sizeof(kRoute[0]);

inline constexpr uint32_t kEntry = 0x002B5C0F;
inline constexpr uint32_t kSystemRoutine = 0x002B5C0F;
inline constexpr uint32_t kStartRoutine = 0x002B38A2;
inline constexpr uint32_t kStartContext = 0;
inline constexpr uint32_t kSehHelper = 0x00319804;
inline constexpr uint32_t kSehHandler = 0x00319858;
inline constexpr uint32_t kSehReturn = 0x002B5C1B;
inline constexpr uint32_t kStackHelper = 0x002B58B8;
inline constexpr uint32_t kNextHelper = 0x002B597D;
inline constexpr uint32_t kStopBefore = 0x002B5C7B;
inline constexpr uint32_t kStartCallReturn = 0x002B38A7;
inline constexpr uint32_t kTlsFillBranch = 0x002B5C5F;
inline constexpr uint32_t kTlsFillFallthrough = 0x002B5C61;
inline constexpr uint32_t kTlsFillTarget = 0x002B5C74;

inline constexpr uint32_t kFirstPushValue = 0x00000018;
inline constexpr uint32_t kSecondPushValue = 0x003ED5B8;
inline constexpr uint32_t kScopeTable = 0x003ED5B8;

inline constexpr uint32_t kTlsGlobalSource = 0x003ED3EC;
inline constexpr uint32_t kTlsGlobalEnd = 0x003ED3F0;
inline constexpr uint32_t kTlsGlobalZeroFill = 0x003ED3FC;
inline constexpr uint32_t kTlsBlockSize = 0x94;
inline constexpr uint32_t kTlsPointerBytes = 4;

inline constexpr uint32_t kHostCallbackReturn = 0xFFFFFFF0;
inline constexpr uint32_t kCallbackFrameBytes = 12;

inline constexpr uint32_t kCriticalSection = 0x0046C8B0;
inline constexpr uint32_t kCriticalListHead = 0x0046C8CC;
inline constexpr uint32_t kCriticalListSentinel = 0x0046C8CC;
inline constexpr uint32_t kCriticalLockCount = kCriticalSection + 0x10;
inline constexpr uint32_t kCriticalRecursion = kCriticalSection + 0x14;
inline constexpr uint32_t kCriticalOwner = kCriticalSection + 0x18;
inline constexpr uint32_t kCriticalEnterOrdinal = 277;
inline constexpr uint32_t kCriticalLeaveOrdinal = 294;
inline constexpr uint32_t kCriticalEnterSlot = 0x003C9E44;
inline constexpr uint32_t kCriticalLeaveSlot = 0x003C9E40;
inline constexpr uint32_t kCriticalEnterEncoding = 0x80000115;
inline constexpr uint32_t kCriticalLeaveEncoding = 0x80000126;
inline constexpr uint32_t kCriticalEnterCall = 0x002B5986;
inline constexpr uint32_t kCriticalEnterReturn = 0x002B598C;
inline constexpr uint32_t kCriticalLeaveCall = 0x002B59A9;
inline constexpr uint32_t kCriticalLeaveReturn = 0x002B59AF;
inline constexpr uint32_t kCriticalListCallback = 0x002B59A1;
inline constexpr uint32_t kCriticalMaxListNodes = 16;

inline constexpr uint32_t kTrapPcs[] = {kStartRoutine, kStackHelper};
inline constexpr size_t kTrapCount = sizeof(kTrapPcs) / sizeof(kTrapPcs[0]);

inline constexpr uint32_t kMaximumInstructions = 96;
inline constexpr uint32_t kMaximumMemoryOperations = 256;
inline constexpr uint32_t kMaximumTraceRecords = kMaximumInstructions;
inline constexpr uint32_t kMaximumDirectCalls = 2;
inline constexpr uint32_t kMaximumKernelCalls = 2;
inline constexpr uint32_t kMaximumImportReads = 2;
inline constexpr uint32_t kExpectedPrepareInstructions = 71;

inline constexpr uint32_t kAuthenticCopyBytes = 0x00;
inline constexpr uint32_t kAuthenticZeroFillBytes = 0x90;

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
    "call 0x002B597D",                        // 002B5C76
    "push ebx",                               // 002B597D
    "push esi",                               // 002B597E
    "push edi",                               // 002B597F
    "mov ebx, 0x46C8B0",                      // 002B5980
    "push ebx",                               // 002B5985
    "call [0x003C9E44]",                      // 002B5986
    "mov esi, [0x0046C8CC]",                  // 002B598C
    "mov edi, 0x46C8CC",                      // 002B5992
    "jmp 0x002B59A4",                         // 002B5997
    "cmp esi, edi",                           // 002B59A4
    "jnz 0x002B5999",                         // 002B59A6
    "push ebx",                               // 002B59A8
    "call [0x003C9E40]",                      // 002B59A9
    "pop edi",                                // 002B59AF
    "pop esi",                                // 002B59B0
    "pop ebx",                                // 002B59B1
    "ret 0x04",                               // 002B59B2
    "push [ebp+0x0C]",                        // 002B5C7B
    "push [esp+0x10]",                        // 002B5999
    "mov eax, esi",                           // 002B599D
    "mov esi, [esi]",                         // 002B599F
    "call [eax+0x08]",                        // 002B59A1
};
inline constexpr size_t kExpectedRouteTextCount =
    sizeof(kExpectedRouteText) / sizeof(kExpectedRouteText[0]);

inline constexpr const char* kExpectedTrapText[] = {
    "call 0x002B58B8",          // 002B38A2 start routine entry
    "mov eax, [0x003C9E34]",    // 002B58B8 first callee
};

inline constexpr uint32_t kExpectedRouteBytes = 226;
inline constexpr uint64_t kExpectedRouteHash = 0xB3408A252D44C99DULL;

}  // namespace execution_probe_integrated_secondary
