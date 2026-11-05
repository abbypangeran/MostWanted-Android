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
    const std::array<uint8_t, 81> code = {
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
        0xFF, 0x07, 0xC3              // inc dword [edi]; ret
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
              "end = 0x11051\n";
    return bool(config);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || !Write(argv[1], argv[2])) return 1;
    return 0;
}
