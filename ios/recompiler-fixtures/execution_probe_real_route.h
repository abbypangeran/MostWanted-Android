#pragma once

#include <cstddef>
// Reviewed disassembly of the Phase 2C-15 route, formatted by the pinned Zydis
// v4.1.1 with the Intel style.  This is descriptive text, not a copy of the game
// bytes.  The real-image preflight compares the actual decode against it so a
// changed instruction, operand, effective address or branch target fails closed.
namespace execution_probe {

inline constexpr const char* kExpectedRouteText[] = {
    "mov ecx, [0x00010118]",       // 002B3916
    "mov eax, [0x00010108]",       // 002B391C
    "sub eax, ecx",                // 002B3921
    "add eax, 0x10000",            // 002B3923
    "cmp eax, [ecx]",              // 002B3928
    "jnb 0x002B392E",              // 002B392A
    "mov [ecx], eax",              // 002B392C
    "mov eax, [0x003ED3FC]",       // 002B392E
    "sub eax, [0x003ED3EC]",       // 002B3933
    "mov ecx, [0x003ED3F0]",       // 002B3939
    "push esi",                    // 002B393F
    "lea eax, [eax+ecx*1+0x0F]",   // 002B3940
    "push edi",                    // 002B3944
    "and eax, 0xFFFFFFF0",         // 002B3945
    "push 0xFFFFFFFC",             // 002B3948
    "pop ecx",                     // 002B394A
    "add eax, 0x04",               // 002B394B
    "mov [0x004EDC74], eax",       // 002B394E
    "cdq",                         // 002B3953
    "idiv ecx",                    // 002B3954
    "mov ecx, [0x003ED3F4]",       // 002B3956
    "xor esi, esi",                // 002B395C
    "push esi",                    // 002B395E
    "push esi",                    // 002B395F
    "push esi",                    // 002B3960
    "push 0x2B38A2",               // 002B3961
    "push esi",                    // 002B3966
    "push esi",                    // 002B3967
    "mov [ecx], eax",              // 002B3968
    "call 0x002B5CA7",             // 002B396A
    "push ebp",                    // 002B5CA7
    "mov ebp, esp",                // 002B5CA8
    "mov eax, [ebp+0x0C]",         // 002B5CAA
    "test eax, eax",               // 002B5CAD
    "jnz 0x002B5CB6",              // 002B5CAF
    "mov eax, [0x00010130]",       // 002B5CB1
    "mov ecx, [ebp+0x18]",         // 002B5CB6
    "push 0x2B5C0F",               // 002B5CB9
    "push 0x00",                   // 002B5CBE
    "shr ecx, 0x02",               // 002B5CC0
    "and ecx, 0xFFFFFF01",         // 002B5CC3
    "push ecx",                    // 002B5CC9
    "push [ebp+0x14]",             // 002B5CCA
    "push [ebp+0x10]",             // 002B5CCD
    "push [ebp+0x1C]",             // 002B5CD0
    "push [0x004EDC74]",           // 002B5CD3
    "push eax",                    // 002B5CD9
    "push 0x00",                   // 002B5CDA
    "lea eax, [ebp+0x0C]",         // 002B5CDC
    "push eax",                    // 002B5CDF
    "call [0x003C9E68]",           // 002B5CE0
    "mov ecx, eax",                // 002B5CE6
    "test ecx, ecx",               // 002B5CE8
    "jnl 0x002B5D0E",              // 002B5CEA
    "movzx eax, byte ptr fs:[0x00000024]",  // 002B5CEC, forbidden debug arm
    "mov eax, [ebp+0x0C]",         // 002B5D0E
    "pop ebp",                     // 002B5D11
};

inline constexpr size_t kExpectedRouteTextCount =
    sizeof(kExpectedRouteText) / sizeof(kExpectedRouteText[0]);

// FNV-1a over the 170 reviewed instruction bytes in route order.  A hash keeps
// the proprietary bytes out of tracked source while still pinning the exact
// encoding that the text table decodes.
inline constexpr uint32_t kExpectedRouteBytes = 170;
inline constexpr uint64_t kExpectedRouteHash = 0x51d66cd99d8cc7adULL;

// the debug arm's first instruction, reviewed as a forbidden boundary; it is a
// one-instruction function so the recompiler does not follow its call at all.
inline constexpr const char* kDebugPathText = "movzx eax, byte ptr fs:[0x00000024]";
inline constexpr uint32_t kDebugPathLength = 8;

}  // namespace execution_probe
