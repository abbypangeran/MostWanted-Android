// Phase 2D-19 authentic-run route manifest. Reviewed addresses and
// descriptive decodes only; no guest bytes. Extracted from the Phase 2C/2D
// route tables and checked against the image by authentic_run_preflight.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kernel::authentic::route {

enum class StepKind : uint8_t { kSequential, kConditional, kCall, kKernelCall, kRet };
enum class Segment : uint8_t { kParentRoot, kSystemRoutine, kSehPrologue, kPreStartWalk, kStartCallSetup, kStartRoutine, kHardwareProbe, kKernelInit };

struct Step {
    uint32_t pc;
    uint8_t length;
    StepKind kind;
    uint32_t next;      // fallthrough, or the return continuation for a call/ret
    uint32_t target;    // reviewed call/conditional target, 0 otherwise
    uint32_t slot;      // kernel call operand slot, 0 otherwise
    uint32_t encoding;  // authentic slot encoding for a kernel call, 0 otherwise
    uint16_t ordinal;   // kernel call ordinal, 0 otherwise
    uint8_t arguments;  // kernel call argument count, 0 otherwise
    bool taken;         // expected outcome for a conditional
    Segment segment;
    const char* text;
    uint8_t ret_pop_bytes = 0;  // reviewed RET imm16 cleanup, 0 for RET
};

inline constexpr Step kParentRoute[] = {
    {0x002B3916u, 6, StepKind::kSequential, 0x002B391Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov ecx, [0x00010118]"},
    {0x002B391Cu, 5, StepKind::kSequential, 0x002B3921u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov eax, [0x00010108]"},
    {0x002B3921u, 2, StepKind::kSequential, 0x002B3923u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "sub eax, ecx"},
    {0x002B3923u, 5, StepKind::kSequential, 0x002B3928u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "add eax, 0x10000"},
    {0x002B3928u, 2, StepKind::kSequential, 0x002B392Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "cmp eax, [ecx]"},
    {0x002B392Au, 2, StepKind::kConditional, 0x002B392Cu, 0x002B392Eu, 0x00000000u, 0x00000000u, 0, 0, true, Segment::kParentRoot, "jnb 0x002B392E"},
    {0x002B392Eu, 5, StepKind::kSequential, 0x002B3933u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov eax, [0x003ED3FC]"},
    {0x002B3933u, 6, StepKind::kSequential, 0x002B3939u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "sub eax, [0x003ED3EC]"},
    {0x002B3939u, 6, StepKind::kSequential, 0x002B393Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov ecx, [0x003ED3F0]"},
    {0x002B393Fu, 1, StepKind::kSequential, 0x002B3940u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push esi"},
    {0x002B3940u, 4, StepKind::kSequential, 0x002B3944u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "lea eax, [eax+ecx*1+0x0F]"},
    {0x002B3944u, 1, StepKind::kSequential, 0x002B3945u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push edi"},
    {0x002B3945u, 3, StepKind::kSequential, 0x002B3948u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "and eax, 0xFFFFFFF0"},
    {0x002B3948u, 2, StepKind::kSequential, 0x002B394Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push 0xFFFFFFFC"},
    {0x002B394Au, 1, StepKind::kSequential, 0x002B394Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "pop ecx"},
    {0x002B394Bu, 3, StepKind::kSequential, 0x002B394Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "add eax, 0x04"},
    {0x002B394Eu, 5, StepKind::kSequential, 0x002B3953u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov [0x004EDC74], eax"},
    {0x002B3953u, 1, StepKind::kSequential, 0x002B3954u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "cdq"},
    {0x002B3954u, 2, StepKind::kSequential, 0x002B3956u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "idiv ecx"},
    {0x002B3956u, 6, StepKind::kSequential, 0x002B395Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov ecx, [0x003ED3F4]"},
    {0x002B395Cu, 2, StepKind::kSequential, 0x002B395Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "xor esi, esi"},
    {0x002B395Eu, 1, StepKind::kSequential, 0x002B395Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push esi"},
    {0x002B395Fu, 1, StepKind::kSequential, 0x002B3960u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push esi"},
    {0x002B3960u, 1, StepKind::kSequential, 0x002B3961u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push esi"},
    {0x002B3961u, 5, StepKind::kSequential, 0x002B3966u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push 0x2B38A2"},
    {0x002B3966u, 1, StepKind::kSequential, 0x002B3967u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push esi"},
    {0x002B3967u, 1, StepKind::kSequential, 0x002B3968u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push esi"},
    {0x002B3968u, 2, StepKind::kSequential, 0x002B396Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov [ecx], eax"},
    {0x002B396Au, 5, StepKind::kCall, 0x002B396Fu, 0x002B5CA7u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "call 0x002B5CA7"},
    {0x002B5CA7u, 1, StepKind::kSequential, 0x002B5CA8u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push ebp"},
    {0x002B5CA8u, 2, StepKind::kSequential, 0x002B5CAAu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov ebp, esp"},
    {0x002B5CAAu, 3, StepKind::kSequential, 0x002B5CADu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov eax, [ebp+0x0C]"},
    {0x002B5CADu, 2, StepKind::kSequential, 0x002B5CAFu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "test eax, eax"},
    {0x002B5CAFu, 2, StepKind::kConditional, 0x002B5CB1u, 0x002B5CB6u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "jnz 0x002B5CB6"},
    {0x002B5CB1u, 5, StepKind::kSequential, 0x002B5CB6u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov eax, [0x00010130]"},
    {0x002B5CB6u, 3, StepKind::kSequential, 0x002B5CB9u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov ecx, [ebp+0x18]"},
    {0x002B5CB9u, 5, StepKind::kSequential, 0x002B5CBEu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push 0x2B5C0F"},
    {0x002B5CBEu, 2, StepKind::kSequential, 0x002B5CC0u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push 0x00"},
    {0x002B5CC0u, 3, StepKind::kSequential, 0x002B5CC3u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "shr ecx, 0x02"},
    {0x002B5CC3u, 6, StepKind::kSequential, 0x002B5CC9u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "and ecx, 0xFFFFFF01"},
    {0x002B5CC9u, 1, StepKind::kSequential, 0x002B5CCAu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push ecx"},
    {0x002B5CCAu, 3, StepKind::kSequential, 0x002B5CCDu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push [ebp+0x14]"},
    {0x002B5CCDu, 3, StepKind::kSequential, 0x002B5CD0u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push [ebp+0x10]"},
    {0x002B5CD0u, 3, StepKind::kSequential, 0x002B5CD3u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push [ebp+0x1C]"},
    {0x002B5CD3u, 6, StepKind::kSequential, 0x002B5CD9u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push [0x004EDC74]"},
    {0x002B5CD9u, 1, StepKind::kSequential, 0x002B5CDAu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push eax"},
    {0x002B5CDAu, 2, StepKind::kSequential, 0x002B5CDCu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push 0x00"},
    {0x002B5CDCu, 3, StepKind::kSequential, 0x002B5CDFu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "lea eax, [ebp+0x0C]"},
    {0x002B5CDFu, 1, StepKind::kSequential, 0x002B5CE0u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push eax"},
    {0x002B5CE0u, 6, StepKind::kKernelCall, 0x002B5CE6u, 0x00000000u, 0x003C9E68u, 0x800000FFu, 255, 10, false, Segment::kParentRoot, "call [0x003C9E68]"},
    {0x002B5CE6u, 2, StepKind::kSequential, 0x002B5CE8u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov ecx, eax"},
    {0x002B5CE8u, 2, StepKind::kSequential, 0x002B5CEAu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "test ecx, ecx"},
    {0x002B5CEAu, 2, StepKind::kConditional, 0x002B5CECu, 0x002B5D0Eu, 0x00000000u, 0x00000000u, 0, 0, true, Segment::kParentRoot, "jnl 0x002B5D0E"},
    {0x002B5D0Eu, 3, StepKind::kSequential, 0x002B5D11u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov eax, [ebp+0x0C]"},
    {0x002B5D11u, 1, StepKind::kSequential, 0x002B5D12u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "pop ebp"},
    {0x002B5D12u, 3, StepKind::kRet, 0x002B396Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "ret 0x18", 0x18},
    {0x002B396Fu, 2, StepKind::kSequential, 0x002B3971u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "mov edi, eax"},
    {0x002B3971u, 2, StepKind::kSequential, 0x002B3973u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "cmp edi, esi"},
    {0x002B3973u, 2, StepKind::kConditional, 0x002B3975u, 0x002B397Fu, 0x00000000u, 0x00000000u, 0, 0, true, Segment::kParentRoot, "jnz 0x002B397F"},
    {0x002B397Fu, 1, StepKind::kSequential, 0x002B3980u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push edi"},
    {0x002B3980u, 5, StepKind::kCall, 0x002B3985u, 0x002B2759u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "call 0x002B2759"},
    {0x002B2759u, 4, StepKind::kSequential, 0x002B275Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "push [esp+0x04]"},
    {0x002B275Du, 6, StepKind::kKernelCall, 0x002B2763u, 0x00000000u, 0x003C9D4Cu, 0x800000BBu, 187, 1, false, Segment::kParentRoot, "call [0x003C9D4C]"},
    {0x002B2763u, 2, StepKind::kSequential, 0x002B2765u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "test eax, eax"},
    {0x002B2765u, 2, StepKind::kConditional, 0x002B2767u, 0x002B276Cu, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "jl 0x002B276C"},
    {0x002B2767u, 2, StepKind::kSequential, 0x002B2769u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "xor eax, eax"},
    {0x002B2769u, 1, StepKind::kSequential, 0x002B276Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "inc eax"},
    {0x002B276Au, 2, StepKind::kSequential, 0x002B2774u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "jmp 0x002B2774"},
    {0x002B2774u, 3, StepKind::kRet, 0x002B3985u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "ret 0x04", 0x04},
    {0x002B3985u, 1, StepKind::kSequential, 0x002B3986u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "pop edi"},
    {0x002B3986u, 1, StepKind::kSequential, 0x002B3987u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kParentRoot, "pop esi"},
};
inline constexpr size_t kParentRouteCount = sizeof(kParentRoute) / sizeof(kParentRoute[0]);

inline constexpr Step kChildRoute[] = {
    {0x002B5C0Fu, 2, StepKind::kSequential, 0x002B5C11u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "push 0x18"},
    {0x002B5C11u, 5, StepKind::kSequential, 0x002B5C16u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "push 0x3ED5B8"},
    {0x002B5C16u, 5, StepKind::kCall, 0x002B5C1Bu, 0x00319804u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "call 0x00319804"},
    {0x00319804u, 5, StepKind::kSequential, 0x00319809u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "push 0x319858"},
    {0x00319809u, 6, StepKind::kSequential, 0x0031980Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov eax, fs:[0x00000000]"},
    {0x0031980Fu, 1, StepKind::kSequential, 0x00319810u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "push eax"},
    {0x00319810u, 4, StepKind::kSequential, 0x00319814u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov eax, [esp+0x10]"},
    {0x00319814u, 4, StepKind::kSequential, 0x00319818u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov [esp+0x10], ebp"},
    {0x00319818u, 4, StepKind::kSequential, 0x0031981Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "lea ebp, [esp+0x10]"},
    {0x0031981Cu, 2, StepKind::kSequential, 0x0031981Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "sub esp, eax"},
    {0x0031981Eu, 1, StepKind::kSequential, 0x0031981Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "push ebx"},
    {0x0031981Fu, 1, StepKind::kSequential, 0x00319820u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "push esi"},
    {0x00319820u, 1, StepKind::kSequential, 0x00319821u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "push edi"},
    {0x00319821u, 3, StepKind::kSequential, 0x00319824u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov eax, [ebp-0x08]"},
    {0x00319824u, 3, StepKind::kSequential, 0x00319827u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov [ebp-0x18], esp"},
    {0x00319827u, 1, StepKind::kSequential, 0x00319828u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "push eax"},
    {0x00319828u, 3, StepKind::kSequential, 0x0031982Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov eax, [ebp-0x04]"},
    {0x0031982Bu, 7, StepKind::kSequential, 0x00319832u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov dword ptr [ebp-0x04], 0xFFFFFFFF"},
    {0x00319832u, 3, StepKind::kSequential, 0x00319835u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov [ebp-0x08], eax"},
    {0x00319835u, 3, StepKind::kSequential, 0x00319838u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "lea eax, [ebp-0x10]"},
    {0x00319838u, 6, StepKind::kSequential, 0x0031983Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "mov fs:[0x00000000], eax"},
    {0x0031983Eu, 1, StepKind::kRet, 0x002B5C1Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSehPrologue, "ret"},
    {0x002B5C1Bu, 4, StepKind::kSequential, 0x002B5C1Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "and dword ptr [ebp-0x04], 0x00"},
    {0x002B5C1Fu, 6, StepKind::kSequential, 0x002B5C25u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov eax, fs:[0x00000028]"},
    {0x002B5C25u, 3, StepKind::kSequential, 0x002B5C28u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov [ebp-0x20], eax"},
    {0x002B5C28u, 3, StepKind::kSequential, 0x002B5C2Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov edx, [eax+0x28]"},
    {0x002B5C2Bu, 3, StepKind::kSequential, 0x002B5C2Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "add edx, 0x04"},
    {0x002B5C2Eu, 3, StepKind::kSequential, 0x002B5C31u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov [ebp-0x24], edx"},
    {0x002B5C31u, 3, StepKind::kSequential, 0x002B5C34u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov [edx-0x04], edx"},
    {0x002B5C34u, 6, StepKind::kSequential, 0x002B5C3Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov ebx, [0x003ED3F0]"},
    {0x002B5C3Au, 6, StepKind::kSequential, 0x002B5C40u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov esi, [0x003ED3EC]"},
    {0x002B5C40u, 2, StepKind::kSequential, 0x002B5C42u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "sub ebx, esi"},
    {0x002B5C42u, 3, StepKind::kSequential, 0x002B5C45u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov [ebp-0x28], ebx"},
    {0x002B5C45u, 2, StepKind::kSequential, 0x002B5C47u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov ecx, ebx"},
    {0x002B5C47u, 2, StepKind::kSequential, 0x002B5C49u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov edi, edx"},
    {0x002B5C49u, 2, StepKind::kSequential, 0x002B5C4Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov eax, ecx"},
    {0x002B5C4Bu, 3, StepKind::kSequential, 0x002B5C4Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "shr ecx, 0x02"},
    {0x002B5C4Eu, 2, StepKind::kSequential, 0x002B5C50u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "rep movsd"},
    {0x002B5C50u, 2, StepKind::kSequential, 0x002B5C52u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov ecx, eax"},
    {0x002B5C52u, 3, StepKind::kSequential, 0x002B5C55u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "and ecx, 0x03"},
    {0x002B5C55u, 2, StepKind::kSequential, 0x002B5C57u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "rep movsb"},
    {0x002B5C57u, 6, StepKind::kSequential, 0x002B5C5Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov ecx, [0x003ED3FC]"},
    {0x002B5C5Du, 2, StepKind::kSequential, 0x002B5C5Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "test ecx, ecx"},
    {0x002B5C5Fu, 2, StepKind::kConditional, 0x002B5C61u, 0x002B5C74u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "jz 0x002B5C74"},
    {0x002B5C61u, 2, StepKind::kSequential, 0x002B5C63u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "xor eax, eax"},
    {0x002B5C63u, 3, StepKind::kSequential, 0x002B5C66u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "lea edi, [ebx+edx*1]"},
    {0x002B5C66u, 2, StepKind::kSequential, 0x002B5C68u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov edx, ecx"},
    {0x002B5C68u, 3, StepKind::kSequential, 0x002B5C6Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "shr ecx, 0x02"},
    {0x002B5C6Bu, 2, StepKind::kSequential, 0x002B5C6Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "rep stosd"},
    {0x002B5C6Du, 2, StepKind::kSequential, 0x002B5C6Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "mov ecx, edx"},
    {0x002B5C6Fu, 3, StepKind::kSequential, 0x002B5C72u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "and ecx, 0x03"},
    {0x002B5C72u, 2, StepKind::kSequential, 0x002B5C74u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "rep stosb"},
    {0x002B5C74u, 2, StepKind::kSequential, 0x002B5C76u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "push 0x01"},
    {0x002B5C76u, 5, StepKind::kCall, 0x002B5C7Bu, 0x002B597Du, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kSystemRoutine, "call 0x002B597D"},
    {0x002B597Du, 1, StepKind::kSequential, 0x002B597Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "push ebx"},
    {0x002B597Eu, 1, StepKind::kSequential, 0x002B597Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "push esi"},
    {0x002B597Fu, 1, StepKind::kSequential, 0x002B5980u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "push edi"},
    {0x002B5980u, 5, StepKind::kSequential, 0x002B5985u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "mov ebx, 0x46C8B0"},
    {0x002B5985u, 1, StepKind::kSequential, 0x002B5986u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "push ebx"},
    {0x002B5986u, 6, StepKind::kKernelCall, 0x002B598Cu, 0x00000000u, 0x003C9E44u, 0x80000115u, 277, 1, false, Segment::kPreStartWalk, "call [0x003C9E44]"},
    {0x002B598Cu, 6, StepKind::kSequential, 0x002B5992u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "mov esi, [0x0046C8CC]"},
    {0x002B5992u, 5, StepKind::kSequential, 0x002B5997u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "mov edi, 0x46C8CC"},
    {0x002B5997u, 2, StepKind::kSequential, 0x002B59A4u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "jmp 0x002B59A4"},
    {0x002B59A4u, 2, StepKind::kSequential, 0x002B59A6u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "cmp esi, edi"},
    {0x002B59A6u, 2, StepKind::kConditional, 0x002B59A8u, 0x002B5999u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "jnz 0x002B5999"},
    {0x002B59A8u, 1, StepKind::kSequential, 0x002B59A9u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "push ebx"},
    {0x002B59A9u, 6, StepKind::kKernelCall, 0x002B59AFu, 0x00000000u, 0x003C9E40u, 0x80000126u, 294, 1, false, Segment::kPreStartWalk, "call [0x003C9E40]"},
    {0x002B59AFu, 1, StepKind::kSequential, 0x002B59B0u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "pop edi"},
    {0x002B59B0u, 1, StepKind::kSequential, 0x002B59B1u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "pop esi"},
    {0x002B59B1u, 1, StepKind::kSequential, 0x002B59B2u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "pop ebx"},
    {0x002B59B2u, 3, StepKind::kRet, 0x002B5C7Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kPreStartWalk, "ret 0x04", 0x04},
    {0x002B5C7Bu, 3, StepKind::kSequential, 0x002B5C7Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kStartCallSetup, "push [ebp+0x0C]"},
    {0x002B5C7Eu, 3, StepKind::kCall, 0x002B5C81u, 0x002B38A2u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kStartCallSetup, "call [ebp+0x08]"},
    {0x002B38A2u, 5, StepKind::kCall, 0x002B38A7u, 0x002B58B8u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kStartRoutine, "call 0x002B58B8"},
    {0x002B58B8u, 5, StepKind::kSequential, 0x002B58BDu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "mov eax, [0x003C9E34]"},
    {0x002B58BDu, 2, StepKind::kSequential, 0x002B58BFu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "mov eax, [eax]"},
    {0x002B58BFu, 6, StepKind::kSequential, 0x002B58C5u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "mov ecx, [0x003C9E3C]"},
    {0x002B58C5u, 4, StepKind::kSequential, 0x002B58C9u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "movzx ecx, word ptr [ecx+0x04]"},
    {0x002B58C9u, 2, StepKind::kSequential, 0x002B58CBu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "shr eax, 0x01"},
    {0x002B58CBu, 2, StepKind::kSequential, 0x002B58CDu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "not al"},
    {0x002B58CDu, 2, StepKind::kSequential, 0x002B58CFu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "and al, 0x01"},
    {0x002B58CFu, 6, StepKind::kSequential, 0x002B58D5u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "cmp ecx, 0xF68"},
    {0x002B58D5u, 1, StepKind::kSequential, 0x002B58D6u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "push esi"},
    {0x002B58D6u, 2, StepKind::kConditional, 0x002B58D8u, 0x002B5923u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "jz 0x002B5923"},
    {0x002B58D8u, 6, StepKind::kSequential, 0x002B58DEu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "cmp ecx, 0xFC2"},
    {0x002B58DEu, 2, StepKind::kConditional, 0x002B58E0u, 0x002B5918u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "jz 0x002B5918"},
    {0x002B58E0u, 6, StepKind::kSequential, 0x002B58E6u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "cmp ecx, 0xFC7"},
    {0x002B58E6u, 2, StepKind::kConditional, 0x002B58E8u, 0x002B5908u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "jz 0x002B5908"},
    {0x002B58E8u, 6, StepKind::kSequential, 0x002B58EEu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "cmp ecx, 0x12D0"},
    {0x002B58EEu, 2, StepKind::kConditional, 0x002B58F0u, 0x002B5967u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "jle 0x002B5967"},
    {0x002B58F0u, 6, StepKind::kSequential, 0x002B58F6u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "cmp ecx, 0x12D2"},
    {0x002B58F6u, 2, StepKind::kConditional, 0x002B58F8u, 0x002B5967u, 0x00000000u, 0x00000000u, 0, 0, true, Segment::kHardwareProbe, "jnle 0x002B5967"},
    {0x002B5967u, 6, StepKind::kSequential, 0x002B596Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "mov eax, fs:[0x00000020]"},
    {0x002B596Du, 7, StepKind::kSequential, 0x002B5974u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "cmp dword ptr [eax+0x250], 0x00"},
    {0x002B5974u, 1, StepKind::kSequential, 0x002B5975u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "pop esi"},
    {0x002B5975u, 2, StepKind::kConditional, 0x002B5977u, 0x002B597Cu, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "jnz 0x002B597C"},
    {0x002B5977u, 5, StepKind::kSequential, 0x002B5854u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "jmp 0x002B5854"},
    {0x002B5854u, 1, StepKind::kSequential, 0x002B5855u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "push ebp"},
    {0x002B5855u, 2, StepKind::kSequential, 0x002B5857u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "mov ebp, esp"},
    {0x002B5857u, 1, StepKind::kSequential, 0x002B5858u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "push ecx"},
    {0x002B5858u, 1, StepKind::kSequential, 0x002B5859u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "push ecx"},
    {0x002B5859u, 5, StepKind::kSequential, 0x002B585Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "mov eax, [0x8001003C]"},
    {0x002B585Eu, 5, StepKind::kSequential, 0x002B5863u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "mov edx, 0x7FFF0000"},
    {0x002B5863u, 2, StepKind::kSequential, 0x002B5865u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "sub eax, edx"},
    {0x002B5865u, 4, StepKind::kSequential, 0x002B5869u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "movzx ecx, word ptr [eax+0x06]"},
    {0x002B5869u, 1, StepKind::kSequential, 0x002B586Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "push esi"},
    {0x002B586Au, 4, StepKind::kSequential, 0x002B586Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "movzx esi, word ptr [eax+0x14]"},
    {0x002B586Eu, 2, StepKind::kSequential, 0x002B5870u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "add esi, eax"},
    {0x002B5870u, 3, StepKind::kSequential, 0x002B5873u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "lea ecx, [ecx+ecx*4]"},
    {0x002B5873u, 4, StepKind::kSequential, 0x002B5877u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "lea eax, [esi+ecx*8-0x10]"},
    {0x002B5877u, 6, StepKind::kSequential, 0x002B587Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "cmp dword ptr [eax], 0x54494E49"},
    {0x002B587Du, 1, StepKind::kSequential, 0x002B587Eu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "pop esi"},
    {0x002B587Eu, 2, StepKind::kConditional, 0x002B5880u, 0x002B58B6u, 0x00000000u, 0x00000000u, 0, 0, true, Segment::kHardwareProbe, "jnz 0x002B58B6"},
    {0x002B58B6u, 1, StepKind::kSequential, 0x002B58B7u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "leave"},
    {0x002B58B7u, 1, StepKind::kRet, 0x002B38A7u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kHardwareProbe, "ret"},
    {0x002B38A7u, 5, StepKind::kCall, 0x002B38ACu, 0x002B5664u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kStartRoutine, "call 0x002B5664"},
    {0x002B5664u, 1, StepKind::kSequential, 0x002B5665u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push ebp"},
    {0x002B5665u, 2, StepKind::kSequential, 0x002B5667u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "mov ebp, esp"},
    {0x002B5667u, 3, StepKind::kSequential, 0x002B566Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "sub esp, 0x34"},
    {0x002B566Au, 1, StepKind::kSequential, 0x002B566Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push ebx"},
    {0x002B566Bu, 1, StepKind::kSequential, 0x002B566Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push esi"},
    {0x002B566Cu, 1, StepKind::kSequential, 0x002B566Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push edi"},
    {0x002B566Du, 2, StepKind::kSequential, 0x002B566Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "xor ebx, ebx"},
    {0x002B566Fu, 1, StepKind::kSequential, 0x002B5670u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "inc ebx"},
    {0x002B5670u, 1, StepKind::kSequential, 0x002B5671u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push ebx"},
    {0x002B5671u, 2, StepKind::kSequential, 0x002B5673u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "xor esi, esi"},
    {0x002B5673u, 5, StepKind::kSequential, 0x002B5678u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x46CD30"},
    {0x002B5678u, 3, StepKind::kSequential, 0x002B567Bu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "mov [ebp-0x04], esi"},
    {0x002B567Bu, 6, StepKind::kKernelCall, 0x002B5681u, 0x00000000u, 0x003C9E38u, 0x8000002Fu, 47, 2, false, Segment::kKernelInit, "call [0x003C9E38]"},
    {0x002B5681u, 5, StepKind::kCall, 0x002B5686u, 0x002B5E12u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "call 0x002B5E12"},
    {0x002B5E12u, 1, StepKind::kSequential, 0x002B5E13u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push ebp"},
    {0x002B5E13u, 2, StepKind::kSequential, 0x002B5E15u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "mov ebp, esp"},
    {0x002B5E15u, 3, StepKind::kSequential, 0x002B5E18u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "sub esp, 0x0C"},
    {0x002B5E18u, 2, StepKind::kSequential, 0x002B5E1Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x00"},
    {0x002B5E1Au, 5, StepKind::kSequential, 0x002B5E1Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x2B5E00"},
    {0x002B5E1Fu, 5, StepKind::kSequential, 0x002B5E24u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x4EDBA0"},
    {0x002B5E24u, 6, StepKind::kKernelCall, 0x002B5E2Au, 0x00000000u, 0x003C9E30u, 0x8000006Bu, 107, 3, false, Segment::kKernelInit, "call [0x003C9E30]"},
    {0x002B5E2Au, 2, StepKind::kSequential, 0x002B5E2Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x00"},
    {0x002B5E2Cu, 5, StepKind::kSequential, 0x002B5E31u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x4EDBC0"},
    {0x002B5E31u, 6, StepKind::kKernelCall, 0x002B5E37u, 0x00000000u, 0x003C9E1Cu, 0x80000071u, 113, 2, false, Segment::kKernelInit, "call [0x003C9E1C]"},
    // Beyond the fifth-run boundary: the timer/DPC helper continues. The stop
    // is before the device-query call at 0x002B5E5F, which would enter the
    // ExQueryNonVolatileSetting tail path; the following call at 0x002B5E7F
    // schedules the timer DPC and is outside this boundary.
    {0x002B5E37u, 5, StepKind::kSequential, 0x002B5E3Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "mov eax, [0x003C9E34]"},
    {0x002B5E3Cu, 7, StepKind::kSequential, 0x002B5E43u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "and dword ptr [0x004EDBE8], 0x00"},
    {0x002B5E43u, 7, StepKind::kSequential, 0x002B5E4Au, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "and dword ptr [0x004EDBEC], 0x00"},
    {0x002B5E4Au, 3, StepKind::kSequential, 0x002B5E4Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "test byte ptr [eax], 0x08"},
    {0x002B5E4Du, 2, StepKind::kConditional, 0x002B5E4Fu, 0x002B5E7Fu, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "jnz 0x002B5E7F"},
    {0x002B5E4Fu, 3, StepKind::kSequential, 0x002B5E52u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "lea eax, [ebp-0x08]"},
    {0x002B5E52u, 1, StepKind::kSequential, 0x002B5E53u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push eax"},
    {0x002B5E53u, 2, StepKind::kSequential, 0x002B5E55u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x04"},
    {0x002B5E55u, 3, StepKind::kSequential, 0x002B5E58u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "lea eax, [ebp-0x04]"},
    {0x002B5E58u, 1, StepKind::kSequential, 0x002B5E59u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push eax"},
    {0x002B5E59u, 3, StepKind::kSequential, 0x002B5E5Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "lea eax, [ebp-0x0C]"},
    {0x002B5E5Cu, 1, StepKind::kSequential, 0x002B5E5Du, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push eax"},
    {0x002B5E5Du, 2, StepKind::kSequential, 0x002B5E5Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0, false, Segment::kKernelInit, "push 0x11"},
};
inline constexpr size_t kChildRouteCount = sizeof(kChildRoute) / sizeof(kChildRoute[0]);

inline constexpr uint32_t kParentEntry = 0x002B3916;
inline constexpr uint32_t kParentStop = 0x002B3987;
inline constexpr uint32_t kChildEntry = 0x002B5C0F;
inline constexpr uint32_t kChildStop = 0x002B5E5F;
inline constexpr uint32_t kChildStartRoutine = 0x002B38A2;
inline constexpr uint32_t kChildStartContext = 0;
inline constexpr uint32_t kChildSystemRoutine = 0x002B5C0F;
inline constexpr uint32_t kOrdinal255Call = 0x002B5CE0;
inline constexpr uint32_t kOrdinal255Return = 0x002B5CE6;
inline constexpr uint32_t kOrdinal187Call = 0x002B275D;
inline constexpr uint32_t kOrdinal187Return = 0x002B2763;
inline constexpr uint32_t kOrdinal277Call = 0x002B5986;
inline constexpr uint32_t kOrdinal277Return = 0x002B598C;
inline constexpr uint32_t kOrdinal294Call = 0x002B59A9;
inline constexpr uint32_t kOrdinal294Return = 0x002B59AF;
inline constexpr uint32_t kSehCall = 0x002B5C16;
inline constexpr uint32_t kSehEntry = 0x00319804;
inline constexpr uint32_t kSehHandler = 0x00319858;
inline constexpr uint32_t kSehScopeTable = 0x003ED5B8;
inline constexpr uint32_t kSehScopeBytes = 0x18;
// 0x002B5C1B clears the prologue's -1 scope flag after it returns; the SEH
// record and scope table stay live at this stop.
inline constexpr uint32_t kSehScopeFlag = 0x00000000;
inline constexpr uint32_t kCriticalSectionObject = 0x0046C8B0;
inline constexpr uint32_t kTlsBlockSize = 0x94;
inline constexpr uint32_t kParentExecutedInstructions = 71;
inline constexpr uint32_t kChildExecutedInstructions = 153;
inline constexpr uint32_t kTotalExecutedInstructions = 224;
inline constexpr uint32_t kParentBoundarySteps = 72;
inline constexpr uint32_t kChildBoundarySteps = 154;
inline constexpr uint32_t kTotalBoundarySteps = 226;

// Every reviewed instruction after the first is entered through one transfer,
// each RET is one additional transfer that does not enter an instruction, and
// the terminal stop is itself one transfer. The budget must accommodate all of
// them; a budget that covers only the instruction entries stops at the terminal
// boundary instead of reaching the reviewed stop.
inline constexpr uint32_t ComputeRequiredTransfers(const Step* steps, size_t count) {
    if (!steps || !count) return 0;
    uint32_t transfers = uint32_t(count - 1);
    for (size_t i = 0; i < count; ++i)
        if (steps[i].kind == StepKind::kRet) ++transfers;
    return transfers + 1;
}

inline constexpr uint32_t kParentRequiredTransfers = ComputeRequiredTransfers(kParentRoute, kParentRouteCount);
inline constexpr uint32_t kChildRequiredTransfers = ComputeRequiredTransfers(kChildRoute, kChildRouteCount);
static_assert(kParentRequiredTransfers == 73, "the reviewed parent route needs 73 transfers");
static_assert(kChildRequiredTransfers == 156, "the reviewed child route needs 156 transfers");
inline constexpr uint32_t kParentTrapEntries[] = {kParentStop, 0x002B5CEC, 0x002B3975, 0x002B276C};
inline constexpr uint32_t kChildTrapEntries[] = {kChildStop, 0x002B5E7F, 0x002B39B9, 0x002B5DE5,
                                                0x002B95AC, kSehHandler, 0x002B5999, 0x002B59A1,
                                                0x002B5923, 0x002B5918, 0x002B5908, 0x002B58F8,
                                                0x002B5880, 0x002B597C};
inline constexpr uint32_t kAllTrapEntries[] = {kParentStop, 0x002B5CEC, 0x002B3975, 0x002B276C,
                                                kChildStop, 0x002B5E7F, 0x002B39B9, 0x002B5DE5,
                                                0x002B95AC, kSehHandler, 0x002B5999, 0x002B59A1,
                                                0x002B5923, 0x002B5918, 0x002B5908, 0x002B58F8,
                                                0x002B5880, 0x002B597C};

inline constexpr uint64_t ComputeManifestIdentity(const Step* steps, size_t count, uint32_t entry, uint32_t stop,
                                                 uint32_t seh_call = 0, uint32_t seh_entry = 0,
                                                 uint32_t seh_handler = 0, uint32_t seh_scope_table = 0,
                                                 uint32_t seh_scope_bytes = 0, uint32_t seh_scope_flag = 0,
                                                 uint32_t tls_block_size = 0, uint32_t critical_section_object = 0,
                                                 uint32_t start_routine = 0, uint32_t required_transfers = 0) {
    if (!steps) return 0;
    uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= uint8_t(value >> shift);
            hash *= 0x100000001B3ull;
        }
    };
    mix(entry);
    mix(stop);
    mix(uint32_t(count));
    for (size_t i = 0; i < count; ++i) {
        const Step& step = steps[i];
        mix(step.pc);
        mix(step.length);
        mix(uint32_t(step.kind));
        mix(step.next);
        mix(step.target);
        mix(step.slot);
        mix(step.encoding);
        mix(step.ordinal);
        mix(step.arguments);
        mix(step.taken ? 1u : 0u);
        mix(step.ret_pop_bytes);
    }
    mix(seh_call);
    mix(seh_entry);
    mix(seh_handler);
    mix(seh_scope_table);
    mix(seh_scope_bytes);
    mix(seh_scope_flag);
    mix(tls_block_size);
    mix(critical_section_object);
    mix(start_routine);
    mix(required_transfers);
    return hash;
}

}  // namespace kernel::authentic::route
