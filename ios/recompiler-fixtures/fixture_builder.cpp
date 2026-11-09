#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xbe/xbe.h"

namespace {

constexpr uint32_t kBase = 0x10000;
constexpr uint32_t kEntry = 0x11000;
constexpr uint32_t kSectionTable = 0x180;
constexpr uint32_t kSectionName = 0x240;
constexpr uint32_t kRawCode = 0x1000;

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
    bytes[offset + 2] = uint8_t(value >> 16);
    bytes[offset + 3] = uint8_t(value >> 24);
}

bool Write(const std::filesystem::path& xbe_path, const std::filesystem::path& config_path) {
    std::vector<uint8_t> code = {
        0x8B, 0x03,                   // mov eax, [ebx]
        0x89, 0x07,                   // mov [edi], eax
        0x88, 0x47, 0x04,             // mov [edi+4], al
        0x66, 0x89, 0x4F, 0x05,       // mov [edi+5], cx
        0x0F, 0xB6, 0x0E,             // movzx ecx, byte [esi]
        0x0F, 0xBE, 0x16,             // movsx edx, byte [esi]
        0x0F, 0xB7, 0x5E, 0x05,       // movzx ebx, word [esi+5]
        0x0F, 0xBF, 0x6E, 0x05,       // movsx ebp, word [esi+5]
        0x0F, 0x6F, 0x46, 0x08,       // movq mm0, [esi+8]
        0x0F, 0x7F, 0x47, 0x08,       // movq [edi+8], mm0
        0xC3,                         // ret
        0x83, 0x07, 0x01,             // add dword [edi], 1
        0x74, 0x05,                   // jz ret
        0xB9, 0x78, 0x56, 0x34, 0x12, // mov ecx, 0x12345678
        0xC3,                         // ret
        0x87, 0x07,                   // xchg eax, [edi] (must trap in checked mode)
        0xC3,                         // ret
        0x0F, 0x10, 0x06, 0xC3,       // movups xmm0, [esi]; ret
        0xDB, 0x2E, 0xC3,             // fld tbyte [esi]; ret
        0xF0, 0x0F, 0xC1, 0x07, 0xC3, // lock xadd [edi], eax; ret
        0xF3, 0xA5, 0xC3,             // rep movsd; ret
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, // mov eax, 1; ret
        0x8B, 0x03, 0x89, 0x07, 0xC3, // mov eax, [ebx]; mov [edi], eax; ret
        0x83, 0x2F, 0x01, 0xC3,       // sub dword [edi], 1; ret
        0xFF, 0x07, 0xC3,             // inc dword [edi]; ret
        0x39, 0xD8, 0xC3,             // cmp eax, ebx; ret
        0x38, 0xD8, 0xC3,             // cmp al, bl; ret
        0x66, 0x39, 0xD8, 0xC3,       // cmp ax, bx; ret
        0x3D, 0x00, 0x00, 0x00, 0x80, 0xC3, // cmp eax, 0x80000000; ret
        0x83, 0xF8, 0xFF, 0xC3,       // cmp eax, sign-extended -1; ret
        0x39, 0x06, 0xC3,             // cmp dword [esi], eax; ret
        0x3B, 0x06, 0xC3,             // cmp eax, dword [esi]; ret
        0x38, 0x06, 0xC3,             // cmp al, byte [esi]; ret
        0x66, 0x39, 0x06, 0xC3,       // cmp ax, word [esi]; ret
        0x39, 0x44, 0xB3, 0xFC, 0xC3, // cmp dword [ebx+esi*4-4], eax; ret
        0x83, 0x3E, 0xFF, 0xC3,       // cmp dword [esi], sign-extended -1; ret
        0x3B, 0x06, 0x73, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3,
        0x3B, 0x06, 0x7A, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3,
        0x3B, 0x06, 0x7C, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3,
        0x56, 0xC3,                         // push esi; ret
        0x6A, 0xFC, 0xC3,                   // push sign-extended imm8 -4; ret
        0x68, 0x78, 0x56, 0x34, 0x12, 0xC3, // push imm32; ret
        0x54, 0xC3,                         // push esp; ret
        0xFF, 0x36, 0xC3,                   // push dword [esi]; ret (blocked checked shape)
        0x56, 0x6A, 0xFC, 0xC3,             // push esi; push -4; ret
        0x8D, 0x44, 0x08, 0x0F, 0xC3,       // lea eax, [eax+ecx+0x0F]; ret
        0x8D, 0x44, 0xB3, 0xFC, 0xC3,       // lea eax, [ebx+esi*4-4]; ret
        0x67, 0x8D, 0x47, 0x01, 0xC3,       // lea eax, [bx+1]; ret
        0x21, 0xD8, 0xC3,                   // and eax, ebx; ret
        0x83, 0xE0, 0x80, 0xC3,             // and eax, sign-extended -128; ret
        0x21, 0x06, 0xC3,                   // and dword [esi], eax; ret
        0x23, 0x06, 0xC3,                   // and eax, dword [esi]; ret
        0x24, 0xF0, 0xC3,                   // and al, 0xF0; ret
        0x66, 0x21, 0xD8, 0xC3,             // and ax, bx; ret
        0x83, 0x26, 0x0F, 0xC3,             // and dword [esi], 0x0F; ret
        0x21, 0xD8, 0x23, 0x0E, 0xC3,       // and eax, ebx; and ecx, [esi]; ret
        0x21, 0xD8, 0x56, 0xC3,             // and eax, ebx; push esi; ret
        0xB8, 0x78, 0x56, 0x34, 0x12, 0xC3, // call target: mov eax, 0x12345678; ret
        0xE8, 0xF5, 0xFF, 0xFF, 0xFF,       // direct caller: call 0x110EF
        0xBB, 0x88, 0x77, 0x66, 0x55, 0xC3, // caller continuation; ret
        0x83, 0xC0, 0x01, 0xC3,             // nested leaf: add eax, 1; ret
        0xE8, 0xF7, 0xFF, 0xFF, 0xFF,       // nested middle: call 0x11100
        0x83, 0xC0, 0x02, 0xC3,             // add eax, 2; ret
        0xB8, 0, 0, 0, 0,                   // nested outer: mov eax, 0
        0xE8, 0xED, 0xFF, 0xFF, 0xFF,       // call 0x11104
        0xBB, 0x11, 0x22, 0x33, 0x44, 0xC3, // continuation marker; ret
        0xC2, 0x08, 0x00,                   // callee cleanup: ret 8
        0xE8, 0xF8, 0xFF, 0xFF, 0xFF,       // caller: call ret 8
        0x89, 0x27, 0xC3,                   // save post-call ESP; ret
        0xC7, 0x04, 0x24, 0xEF, 0xBE, 0xAD, 0xDE, 0xC3, // corrupt return address; ret
        0xE8, 0xF3, 0xFF, 0xFF, 0xFF, 0xC3, // call corrupting callee; ret
        0xB9, 0x78, 0x56, 0x34, 0x12, 0xC3, // continuation target for dispatch tests
        0xFF, 0xD0, 0xC3,                   // indirect CALL remains unsupported
        0xE8, 0xBC, 0x1E, 0x00, 0x00, 0xC3, // direct CALL to unmapped target remains blocked
        0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3, // CALL $+5 remains blocked in checked mode
        0x89, 0xDC, 0xC3,                   // RET read-fault callee: mov esp, ebx; ret
        0xE8, 0xF8, 0xFF, 0xFF, 0xFF, 0xC3, // RET read-fault caller
        0x59, 0xC3,                         // checked POP ECX
        0x58, 0xC3,                         // POP EAX remains unsupported
        0x99, 0xC3,                         // checked CDQ
        0xF7, 0xF9, 0xC3,                   // checked IDIV ECX
        0x31, 0xF6, 0xC3,                   // checked XOR ESI, ESI
        0x66, 0x99, 0xC3,                   // CWD remains unsupported
        0xF7, 0x3E, 0xC3,                   // IDIV dword [ESI] remains unsupported
        0x85, 0xC0, 0xC3,                   // checked TEST EAX, EAX
        0xC1, 0xE9, 0x02, 0xC3,             // checked SHR ECX, 2
        0xFF, 0x75, 0x14, 0xC3,             // checked PUSH dword [EBP+0x14]
        0xFF, 0x35, 0x00, 0x20, 0x01, 0x00, 0xC3, // checked PUSH dword [0x12000]
        0xFF, 0x34, 0x24, 0xC3,             // checked PUSH dword [ESP]
        0x85, 0x06, 0xC3,                   // TEST memory remains unsupported
        0xD3, 0xE9, 0xC3,                   // SHR ECX, CL remains unsupported
        0xC1, 0x2E, 0x02, 0xC3,             // SHR memory remains unsupported
        0x66, 0xFF, 0x36, 0xC3,             // PUSH word [ESI] remains unsupported
        0x64, 0xFF, 0x36, 0xC3,             // PUSH FS:[ESI] remains unsupported
        0xC1, 0xE8, 0x01, 0xC3,             // checked SHR EAX, 1
        0xC1, 0xEB, 0x00, 0xC3,             // checked SHR EBX, 0
        0xC1, 0xE8, 0x1F, 0xC3,             // checked SHR EAX, 31
        0xC1, 0xE8, 0x20, 0xC3,             // checked SHR EAX, 32 masks count to zero
        0xFF, 0x15, 0x00, 0x20, 0x01, 0x00, 0xC3, // indirect outer: call [0x12000]; ret
        0xFF, 0x15, 0x04, 0x20, 0x01, 0x00, 0xC3, // nested indirect: call [0x12004]; ret
        0xE8, 0x01, 0x00, 0x00, 0x00, 0xC3,       // nested direct call 0x111B2; ret
        0x83, 0xC0, 0x01, 0xC3,                   // nested leaf: add eax, 1; ret
        0xC2, 0x08, 0x00,                         // indirect callee cleanup: ret 8
        0xFF, 0x15, 0x08, 0x20, 0x01, 0x00, 0xC3, // call [0x12008]; ret
        0xFF, 0x15, 0x00, 0x40, 0x01, 0x00, 0xC3, // uncommitted source: call [0x14000]; ret
        0xFF, 0x15, 0xFE, 0x5F, 0x01, 0x00, 0xC3, // cross-page source: call [0x15FFE]; ret
        0xFF, 0x15, 0xFE, 0xFF, 0xFF, 0xFF, 0xC3, // overflowing source: call [0xFFFFFFFE]; ret
        0xFF, 0x16, 0xC3,                           // unsupported indirect form: call [esi]; ret
        0x85, 0xC9, 0xC3,                           // checked TEST ECX, ECX; ret
        0x5D, 0xC3,                                 // checked POP EBP; ret
        0xEB, 0x05, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, // checked short JMP skips EAX sentinel
        0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3,       // checked short JMP target returns 42
        0x5F, 0x5E, 0xC3                            // checked POP EDI, POP ESI; ret
    };
    if (code.size() != 0x1ED) return false;
    // 0x111ED: checked POP EBX, kept in the first padding bytes so every
    // existing fixture address stays unchanged.
    code.insert(code.end(), {0x5B, 0xC3});
    code.insert(code.end(), 0x21, 0);
    code.insert(code.end(), {
        0xF6, 0xD0, 0xC3,                         // NOT AL; ret
        0xF7, 0xD0, 0xC3,                         // NOT EAX must trap
        0xF6, 0xD3, 0xC3,                         // NOT BL must trap
        0xF6, 0xD4, 0xC3,                         // NOT AH must trap
        0xF6, 0x16, 0xC3,                         // NOT [ESI] must trap
        0xF3, 0xA4, 0xC3,                         // REP MOVSB
        0xF3, 0xA5, 0xC3,                         // REP MOVSD
        0xF3, 0xAA, 0xC3,                         // REP STOSB
        0xF3, 0xAB, 0xC3,                         // REP STOSD
        0xA4, 0xC3,                               // MOVSB without REP must trap
        0xF2, 0xA4, 0xC3,                         // REPNE MOVSB must trap
        0x66, 0xF3, 0xA5, 0xC3,                   // REP MOVSW must trap
        0x67, 0xF3, 0xA4, 0xC3,                   // 16-bit address REP MOVSB must trap
        0x64, 0xF3, 0xA4, 0xC3,                   // FS override REP MOVSB must trap
        0xF2, 0xAA, 0xC3,                         // REPNE STOSB must trap
        0x64, 0xA1, 0x28, 0x00, 0x00, 0x00, 0xC3, // MOV EAX, FS:[0x28]
        0x64, 0x8B, 0x44, 0x08, 0x28, 0xC3,       // MOV EAX, FS:[EAX+ECX+0x28]
        0x65, 0xA1, 0x28, 0x00, 0x00, 0x00, 0xC3, // GS memory must trap
        0x67, 0x64, 0xA1, 0x28, 0x00, 0xC3,       // 16-bit FS memory must trap
        0x64, 0x0F, 0xB7, 0x05, 0x28, 0, 0, 0, 0xC3, // MOVZX EAX, word FS:[0x28]
        0x65, 0x0F, 0xB6, 0x05, 0x28, 0, 0, 0, 0xC3, // MOVZX GS memory must trap
        0x67, 0x0F, 0xB6, 0x00, 0xC3,                 // 16-bit MOVZX memory must trap
        0x65, 0x0F, 0xBE, 0x05, 0x28, 0, 0, 0, 0xC3, // MOVSX GS memory must trap
        0x67, 0x0F, 0xBE, 0x00, 0xC3                  // 16-bit MOVSX memory must trap
    });
    code.insert(code.end(), {0x8B, 0x44, 0x24, 0x04, 0xC3});  // mov eax, [esp+4]; ret
    // 0x11283: terminate through the checked ordinal-258 slot at 0xA5100; the
    // marker at 0xA5104 must stay untouched when termination succeeds.
    code.insert(code.end(), {
        0x8B, 0x44, 0x24, 0x04,                          // mov eax, [esp+4]
        0x50,                                            // push eax
        0xFF, 0x15, 0x00, 0x51, 0x0A, 0x00,              // call dword ptr [0xA5100]
        0xC7, 0x05, 0x04, 0x51, 0x0A, 0x00, 0xEF, 0xBE, 0xAD, 0xDE, // mov dword ptr [0xA5104], 0xDEADBEEF
        0xC3                                             // ret
    });
    if (code.size() != 0x299) return false;
    constexpr size_t header_size = 0x1000;
    std::vector<uint8_t> image(header_size + code.size(), 0);
    std::copy_n("XBEH", 4, image.begin());
    Put32(image, 0x104, kBase);
    Put32(image, 0x108, header_size);
    Put32(image, 0x10C, 0x2000);
    Put32(image, 0x11C, 1);
    Put32(image, 0x120, kBase + kSectionTable);
    Put32(image, 0x128, kEntry ^ xbe::kRetailEntryKey);
    Put32(image, 0x158, (kBase + 0x280) ^ xbe::kRetailThunkKey);

    Put32(image, kSectionTable + 0, xbe::kExecutable | xbe::kPreload);
    Put32(image, kSectionTable + 4, kEntry);
    Put32(image, kSectionTable + 8, 0x1000);
    Put32(image, kSectionTable + 12, kRawCode);
    Put32(image, kSectionTable + 16, uint32_t(code.size()));
    Put32(image, kSectionTable + 20, kBase + kSectionName);
    std::copy_n(".text", 6, image.begin() + kSectionName);
    std::copy(code.begin(), code.end(), image.begin() + kRawCode);

    std::ofstream xbe(xbe_path, std::ios::binary);
    xbe.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    if (!xbe) return false;
    std::ofstream config(config_path);
    config << "[xbe]\n"
              "path = \"checked_fixture.xbe\"\n"
              "entry = 0x11000\n"
              "out = \"generated\"\n"
              "partitions = 1\n"
              "code_sections = [\".text\"]\n"
              "pointer_seeds = false\n"
              "gap_fill = false\n"
              "checked_memory = true\n\n"
              "[[function]]\n"
              "at = 0x11022\n"
              "end = 0x1102D\n\n"
              "[[function]]\n"
              "at = 0x1102D\n"
              "end = 0x11030\n\n"
              "[[function]]\n"
              "at = 0x11030\n"
              "end = 0x11034\n\n"
              "[[function]]\n"
              "at = 0x11034\n"
              "end = 0x11037\n\n"
              "[[function]]\n"
              "at = 0x11037\n"
              "end = 0x1103C\n\n"
              "[[function]]\n"
              "at = 0x1103C\n"
              "end = 0x1103F\n\n"
              "[[function]]\n"
              "at = 0x1103F\n"
              "end = 0x11045\n\n"
              "[[function]]\n"
              "at = 0x11045\n"
              "end = 0x1104A\n\n"
              "[[function]]\n"
              "at = 0x1104A\n"
              "end = 0x1104E\n\n"
              "[[function]]\n"
              "at = 0x1104E\n"
              "end = 0x11051\n\n"
              "[[function]]\n"
              "at = 0x11051\n"
              "end = 0x11054\n\n"
              "[[function]]\n"
              "at = 0x11054\n"
              "end = 0x11057\n\n"
              "[[function]]\n"
              "at = 0x11057\n"
              "end = 0x1105B\n\n"
              "[[function]]\n"
              "at = 0x1105B\n"
              "end = 0x11061\n\n"
              "[[function]]\n"
              "at = 0x11061\n"
              "end = 0x11065\n\n"
              "[[function]]\n"
              "at = 0x11065\n"
              "end = 0x11068\n\n"
              "[[function]]\n"
              "at = 0x11068\n"
              "end = 0x1106B\n\n"
              "[[function]]\n"
              "at = 0x1106B\n"
              "end = 0x1106E\n\n"
              "[[function]]\n"
              "at = 0x1106E\n"
              "end = 0x11072\n\n"
              "[[function]]\n"
              "at = 0x11072\n"
              "end = 0x11077\n\n"
              "[[function]]\n"
              "at = 0x11077\n"
              "end = 0x1107B\n\n"
              "[[function]]\n"
              "at = 0x1107B\n"
              "end = 0x1108B\n\n"
              "[[function]]\n"
              "at = 0x1108B\n"
              "end = 0x1109B\n\n"
              "[[function]]\n"
              "at = 0x1109B\n"
              "end = 0x110AB\n\n"
              "[[function]]\n"
              "at = 0x110AB\n"
              "end = 0x110AD\n\n"
              "[[function]]\n"
              "at = 0x110AD\n"
              "end = 0x110B0\n\n"
              "[[function]]\n"
              "at = 0x110B0\n"
              "end = 0x110B6\n\n"
              "[[function]]\n"
              "at = 0x110B6\n"
              "end = 0x110B8\n\n"
              "[[function]]\n"
              "at = 0x110B8\n"
              "end = 0x110BB\n\n"
              "[[function]]\n"
              "at = 0x110BB\n"
              "end = 0x110BF\n\n"
              "[[function]]\n"
              "at = 0x110BF\n"
              "end = 0x110C4\n\n"
              "[[function]]\n"
              "at = 0x110C4\n"
              "end = 0x110C9\n\n"
              "[[function]]\n"
              "at = 0x110C9\n"
              "end = 0x110CE\n\n"
              "[[function]]\n"
              "at = 0x110CE\n"
              "end = 0x110D1\n\n"
              "[[function]]\n"
              "at = 0x110D1\n"
              "end = 0x110D5\n\n"
              "[[function]]\n"
              "at = 0x110D5\n"
              "end = 0x110D8\n\n"
              "[[function]]\n"
              "at = 0x110D8\n"
              "end = 0x110DB\n\n"
              "[[function]]\n"
              "at = 0x110DB\n"
              "end = 0x110DE\n\n"
              "[[function]]\n"
              "at = 0x110DE\n"
              "end = 0x110E2\n\n"
              "[[function]]\n"
              "at = 0x110E2\n"
              "end = 0x110E6\n\n"
              "[[function]]\n"
              "at = 0x110E6\n"
              "end = 0x110EB\n\n"
              "[[function]]\n"
              "at = 0x110EB\n"
              "end = 0x110EF\n\n"
              "[[function]]\n"
              "at = 0x110EF\n"
              "end = 0x110F5\n\n"
              "[[function]]\n"
              "at = 0x110F5\n"
              "end = 0x11100\n\n"
              "[[function]]\n"
              "at = 0x11100\n"
              "end = 0x11104\n\n"
              "[[function]]\n"
              "at = 0x11104\n"
              "end = 0x1110D\n\n"
              "[[function]]\n"
              "at = 0x1110D\n"
              "end = 0x1111D\n\n"
              "[[function]]\n"
              "at = 0x1111D\n"
              "end = 0x11120\n\n"
              "[[function]]\n"
              "at = 0x11120\n"
              "end = 0x11128\n\n"
              "[[function]]\n"
              "at = 0x11128\n"
              "end = 0x11130\n\n"
              "[[function]]\n"
              "at = 0x11130\n"
              "end = 0x11136\n\n"
              "[[function]]\n"
              "at = 0x11136\n"
              "end = 0x1113C\n\n"
              "[[function]]\n"
              "at = 0x1113C\n"
              "end = 0x1113F\n\n"
              "[[function]]\n"
              "at = 0x1113F\n"
              "end = 0x11145\n\n"
              "[[function]]\n"
              "at = 0x11145\n"
              "end = 0x1114B\n\n"
              "[[function]]\n"
              "at = 0x1114B\n"
              "end = 0x1114E\n\n"
              "[[function]]\n"
              "at = 0x1114E\n"
              "end = 0x11154\n\n"
              "[[function]]\n"
              "at = 0x11154\n"
              "end = 0x11156\n\n"
              "[[function]]\n"
              "at = 0x11156\n"
              "end = 0x11158\n\n"
              "[[function]]\n"
              "at = 0x11158\n"
              "end = 0x1115A\n\n"
              "[[function]]\n"
              "at = 0x1115A\n"
              "end = 0x1115D\n\n"
              "[[function]]\n"
              "at = 0x1115D\n"
              "end = 0x11160\n\n"
              "[[function]]\n"
              "at = 0x11160\n"
              "end = 0x11163\n\n"
              "[[function]]\n"
              "at = 0x11163\n"
              "end = 0x11166\n\n"
              "[[function]]\n"
              "at = 0x11166\n"
              "end = 0x11169\n\n"
              "[[function]]\n"
              "at = 0x11169\n"
              "end = 0x1116D\n\n"
              "[[function]]\n"
              "at = 0x1116D\n"
              "end = 0x11171\n\n"
              "[[function]]\n"
              "at = 0x11171\n"
              "end = 0x11178\n\n"
              "[[function]]\n"
              "at = 0x11178\n"
              "end = 0x1117C\n\n"
              "[[function]]\n"
              "at = 0x1117C\n"
              "end = 0x1117F\n\n"
              "[[function]]\n"
              "at = 0x1117F\n"
              "end = 0x11182\n\n"
              "[[function]]\n"
              "at = 0x11182\n"
              "end = 0x11186\n\n"
              "[[function]]\n"
              "at = 0x11186\n"
              "end = 0x1118A\n\n"
              "[[function]]\n"
              "at = 0x1118A\n"
              "end = 0x1118E\n\n"
              "[[function]]\n"
              "at = 0x1118E\n"
              "end = 0x11192\n\n"
              "[[function]]\n"
              "at = 0x11192\n"
              "end = 0x11196\n\n"
              "[[function]]\n"
              "at = 0x11196\n"
              "end = 0x1119A\n\n"
              "[[function]]\n"
              "at = 0x1119A\n"
              "end = 0x1119E\n\n"
              "[[function]]\n"
              "at = 0x1119E\n"
              "end = 0x111A5\n\n"
              "[[function]]\n"
              "at = 0x111A5\n"
              "end = 0x111AC\n\n"
              "[[function]]\n"
              "at = 0x111AC\n"
              "end = 0x111B2\n\n"
              "[[function]]\n"
              "at = 0x111B2\n"
              "end = 0x111B6\n\n"
              "[[function]]\n"
              "at = 0x111B6\n"
              "end = 0x111B9\n\n"
              "[[function]]\n"
              "at = 0x111B9\n"
              "end = 0x111C0\n\n"
              "[[function]]\n"
              "at = 0x111C0\n"
              "end = 0x111C7\n\n"
              "[[function]]\n"
              "at = 0x111C7\n"
              "end = 0x111CE\n\n"
              "[[function]]\n"
              "at = 0x111CE\n"
              "end = 0x111D5\n\n"
              "[[function]]\n"
              "at = 0x111D5\n"
              "end = 0x111D8\n\n"
              "[[function]]\n"
              "at = 0x111D8\n"
              "end = 0x111DB\n\n"
              "[[function]]\n"
              "at = 0x111DB\n"
              "end = 0x111DD\n\n"
              "[[function]]\n"
              "at = 0x111DD\n"
              "end = 0x111EA\n\n"
              "[[function]]\n"
              "at = 0x111EA\n"
              "end = 0x111ED\n\n"
              "[[function]]\n"
              "at = 0x111ED\n"
              "end = 0x111EF\n\n"
              "[[function]]\n"
              "at = 0x11210\n"
              "end = 0x11213\n\n"
              "[[function]]\n"
              "at = 0x11213\n"
              "end = 0x11216\n\n"
              "[[function]]\n"
              "at = 0x11216\n"
              "end = 0x11219\n\n"
              "[[function]]\n"
              "at = 0x11219\n"
              "end = 0x1121C\n\n"
              "[[function]]\n"
              "at = 0x1121C\n"
              "end = 0x1121F\n\n"
              "[[function]]\n"
              "at = 0x1121F\n"
              "end = 0x11222\n\n"
              "[[function]]\n"
              "at = 0x11222\n"
              "end = 0x11225\n\n"
              "[[function]]\n"
              "at = 0x11225\n"
              "end = 0x11228\n\n"
              "[[function]]\n"
              "at = 0x11228\n"
              "end = 0x1122B\n\n"
              "[[function]]\n"
              "at = 0x1122B\n"
              "end = 0x1122D\n\n"
              "[[function]]\n"
              "at = 0x1122D\n"
              "end = 0x11230\n\n"
              "[[function]]\n"
              "at = 0x11230\n"
              "end = 0x11234\n\n"
              "[[function]]\n"
              "at = 0x11234\n"
              "end = 0x11238\n\n"
              "[[function]]\n"
              "at = 0x11238\n"
              "end = 0x1123C\n\n"
              "[[function]]\n"
              "at = 0x1123C\n"
              "end = 0x1123F\n\n"
              "[[function]]\n"
              "at = 0x1123F\n"
              "end = 0x11246\n\n"
              "[[function]]\n"
              "at = 0x11246\n"
              "end = 0x1124C\n\n"
              "[[function]]\n"
              "at = 0x1124C\n"
              "end = 0x11253\n\n"
              "[[function]]\n"
              "at = 0x11253\n"
              "end = 0x11259\n";
    config << "\n[[function]]\nat = 0x11259\nend = 0x11262\n"
              "\n[[function]]\nat = 0x11262\nend = 0x1126B\n"
              "\n[[function]]\nat = 0x1126B\nend = 0x11270\n"
              "\n[[function]]\nat = 0x11270\nend = 0x11279\n"
              "\n[[function]]\nat = 0x11279\nend = 0x1127E\n"
              "\n[[function]]\nat = 0x1127E\nend = 0x11283\n"
              "\n[[function]]\nat = 0x11283\nend = 0x11299\n";
    return bool(config);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || !Write(argv[1], argv[2])) return 1;
    return 0;
}
