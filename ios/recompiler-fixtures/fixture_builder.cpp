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
    const std::vector<uint8_t> code = {
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
        0x21, 0xD8, 0x56, 0xC3              // and eax, ebx; push esi; ret
    };
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
              "end = 0x110EF\n";
    return bool(config);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || !Write(argv[1], argv[2])) return 1;
    return 0;
}
