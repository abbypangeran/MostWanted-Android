// Phase 2C-32 synthetic fixture for the secondary-thread SEH/TLS segment.
//
// The image places the reviewed system-routine prefix, the authentic SEH prolog
// and the TLS-initialisation body at their real addresses.  The start routine
// and the two unreviewed callees exist only as trap-only entries so the
// recompiler can name them and the callback contract can resolve the start
// routine without executing it.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "execution_probe_secondary_route.h"
#include "xbe/xbe.h"

namespace {

using namespace execution_probe_secondary;

struct Instruction {
    uint32_t va;
    std::array<uint8_t, 8> bytes;
    uint8_t length;
};

constexpr Instruction kInstructions[] = {
    {0x002B5C0F, {0x6A, 0x18}, 2},                          // push 0x18
    {0x002B5C11, {0x68, 0xB8, 0xD5, 0x3E, 0x00}, 5},        // push 0x3ED5B8
    {0x002B5C16, {0xE8, 0xE9, 0x3B, 0x06, 0x00}, 5},        // call 0x00319804
    {0x002B5C1B, {0x83, 0x65, 0xFC, 0x00}, 4},              // and [ebp-4], 0
    {0x002B5C1F, {0x64, 0xA1, 0x28, 0x00, 0x00, 0x00}, 6},  // mov eax, fs:[0x28]
    {0x002B5C25, {0x89, 0x45, 0xE0}, 3},                    // mov [ebp-0x20], eax
    {0x002B5C28, {0x8B, 0x50, 0x28}, 3},                    // mov edx, [eax+0x28]
    {0x002B5C2B, {0x83, 0xC2, 0x04}, 3},                    // add edx, 4
    {0x002B5C2E, {0x89, 0x55, 0xDC}, 3},                    // mov [ebp-0x24], edx
    {0x002B5C31, {0x89, 0x52, 0xFC}, 3},                    // mov [edx-4], edx
    {0x002B5C34, {0x8B, 0x1D, 0xF0, 0xD3, 0x3E, 0x00}, 6},  // mov ebx, [0x3ED3F0]
    {0x002B5C3A, {0x8B, 0x35, 0xEC, 0xD3, 0x3E, 0x00}, 6},  // mov esi, [0x3ED3EC]
    {0x002B5C40, {0x2B, 0xDE}, 2},                          // sub ebx, esi
    {0x002B5C42, {0x89, 0x5D, 0xD8}, 3},                    // mov [ebp-0x28], ebx
    {0x002B5C45, {0x8B, 0xCB}, 2},                          // mov ecx, ebx
    {0x002B5C47, {0x8B, 0xFA}, 2},                          // mov edi, edx
    {0x002B5C49, {0x8B, 0xC1}, 2},                          // mov eax, ecx
    {0x002B5C4B, {0xC1, 0xE9, 0x02}, 3},                    // shr ecx, 2
    {0x002B5C4E, {0xF3, 0xA5}, 2},                          // rep movsd
    {0x002B5C50, {0x8B, 0xC8}, 2},                          // mov ecx, eax
    {0x002B5C52, {0x83, 0xE1, 0x03}, 3},                    // and ecx, 3
    {0x002B5C55, {0xF3, 0xA4}, 2},                          // rep movsb
    {0x002B5C57, {0x8B, 0x0D, 0xFC, 0xD3, 0x3E, 0x00}, 6},  // mov ecx, [0x3ED3FC]
    {0x002B5C5D, {0x85, 0xC9}, 2},                          // test ecx, ecx
    {0x002B5C5F, {0x74, 0x13}, 2},                          // jz 0x002B5C74
    {0x002B5C61, {0x33, 0xC0}, 2},                          // xor eax, eax
    {0x002B5C63, {0x8D, 0x3C, 0x13}, 3},                    // lea edi, [ebx+edx]
    {0x002B5C66, {0x8B, 0xD1}, 2},                          // mov edx, ecx
    {0x002B5C68, {0xC1, 0xE9, 0x02}, 3},                    // shr ecx, 2
    {0x002B5C6B, {0xF3, 0xAB}, 2},                          // rep stosd
    {0x002B5C6D, {0x8B, 0xCA}, 2},                          // mov ecx, edx
    {0x002B5C6F, {0x83, 0xE1, 0x03}, 3},                    // and ecx, 3
    {0x002B5C72, {0xF3, 0xAA}, 2},                          // rep stosb
    {0x002B5C74, {0x6A, 0x01}, 2},                          // push 1
    {0x002B5C76, {0xE8, 0x02, 0xFD, 0xFF, 0xFF}, 5},        // call 0x002B597D, stop
    {0x00319804, {0x68, 0x58, 0x98, 0x31, 0x00}, 5},        // push 0x319858
    {0x00319809, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}, 6},  // mov eax, fs:[0]
    {0x0031980F, {0x50}, 1},                                // push eax
    {0x00319810, {0x8B, 0x44, 0x24, 0x10}, 4},              // mov eax, [esp+0x10]
    {0x00319814, {0x89, 0x6C, 0x24, 0x10}, 4},              // mov [esp+0x10], ebp
    {0x00319818, {0x8D, 0x6C, 0x24, 0x10}, 4},              // lea ebp, [esp+0x10]
    {0x0031981C, {0x2B, 0xE0}, 2},                          // sub esp, eax
    {0x0031981E, {0x53}, 1},                                // push ebx
    {0x0031981F, {0x56}, 1},                                // push esi
    {0x00319820, {0x57}, 1},                                // push edi
    {0x00319821, {0x8B, 0x45, 0xF8}, 3},                    // mov eax, [ebp-8]
    {0x00319824, {0x89, 0x65, 0xE8}, 3},                    // mov [ebp-0x18], esp
    {0x00319827, {0x50}, 1},                                // push eax
    {0x00319828, {0x8B, 0x45, 0xFC}, 3},                    // mov eax, [ebp-4]
    {0x0031982B, {0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF}, 7},  // mov [ebp-4], -1
    {0x00319832, {0x89, 0x45, 0xF8}, 3},                    // mov [ebp-8], eax
    {0x00319835, {0x8D, 0x45, 0xF0}, 3},                    // lea eax, [ebp-0x10]
    {0x00319838, {0x64, 0xA3, 0x00, 0x00, 0x00, 0x00}, 6},  // mov fs:[0], eax
    {0x0031983E, {0xC3}, 1},                                // ret
    {0x002B38A2, {0xE8, 0x11, 0x20, 0x00, 0x00}, 5},        // call 0x002B58B8, trap
    {0x002B58B8, {0xA1, 0x34, 0x9E, 0x3C, 0x00}, 5},        // mov eax, [0x003C9E34], trap
    {0x002B597D, {0x53}, 1},                                // push ebx, trap
};

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
    bytes[offset + 2] = uint8_t(value >> 16);
    bytes[offset + 3] = uint8_t(value >> 24);
}

bool BuildSection(std::vector<uint8_t>& section) {
    section.assign(kSectionSize, 0x90);
    for (const Instruction& instruction : kInstructions) {
        if (instruction.va < kSectionVa || instruction.va - kSectionVa + instruction.length > section.size())
            return false;
        std::memcpy(section.data() + (instruction.va - kSectionVa), instruction.bytes.data(), instruction.length);
    }
    return true;
}

bool BuildImage(std::vector<uint8_t>& image) {
    std::vector<uint8_t> section;
    if (!BuildSection(section)) return false;
    image.assign(kHeaderSize + kSectionSize, 0);
    std::memcpy(image.data(), "XBEH", 4);
    Put32(image, 0x104, kImageBase);
    Put32(image, 0x108, kHeaderSize);
    Put32(image, 0x10C, kImageSize);
    Put32(image, 0x11C, 1);
    Put32(image, 0x120, kImageBase + 0x180);
    Put32(image, 0x128, kEntry ^ xbe::kRetailEntryKey);
    Put32(image, 0x158, (kImageBase + 0x280) ^ xbe::kRetailThunkKey);

    Put32(image, 0x180 + 0, xbe::kExecutable | xbe::kPreload);
    Put32(image, 0x180 + 4, kSectionVa);
    Put32(image, 0x180 + 8, kSectionSize);
    Put32(image, 0x180 + 12, kSectionRaw);
    Put32(image, 0x180 + 16, kSectionSize);
    Put32(image, 0x180 + 20, kImageBase + 0x240);
    std::memcpy(image.data() + 0x240, ".text", 6);
    std::memcpy(image.data() + kSectionRaw, section.data(), section.size());
    return true;
}

bool WriteConfig(const char* path) {
    std::ostringstream config;
    config << "[xbe]\n"
              "path = \"execution_probe_secondary_fixture.xbe\"\n"
              "entry = 0x002B5C0F\n"
              "out = \"generated\"\n"
              "partitions = 1\n"
              "code_sections = [\".text\"]\n"
              "pointer_seeds = false\n"
              "gap_fill = false\n"
              "callback_seeds = false\n"
              "checked_memory = true\n\n"
              "[[function]]\n"
              "at = 0x002B5C0F\n"
              "end = 0x002B5C7B\n\n"
              "[[function]]\n"
              "at = 0x00319804\n"
              "end = 0x0031983F\n\n"
              "[[function]]\n"
              "at = 0x002B38A2\n"
              "end = 0x002B38A7\n\n"
              "[[function]]\n"
              "at = 0x002B58B8\n"
              "end = 0x002B58BD\n\n"
              "[[function]]\n"
              "at = 0x002B597D\n"
              "end = 0x002B597E\n";
    for (const auto& step : kRoute) {
        config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.pc
               << "\ncall = \"SecondaryStep_0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << step.pc << "\"\n";
    }
    for (uint32_t pc : kTrapPcs) {
        config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << pc
               << "\ncall = \"SecondaryStep_0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << pc << "\"\n";
    }
    std::ofstream file(path);
    file << config.str();
    return bool(file);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: execution_probe_secondary_fixture_builder <xbe> <config>\n");
        return 2;
    }
    std::vector<uint8_t> image;
    if (!BuildImage(image)) {
        std::fprintf(stderr, "secondary fixture code does not fit its section\n");
        return 1;
    }
    std::ofstream xbe(argv[1], std::ios::binary);
    xbe.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    if (!xbe) return 1;
    if (!WriteConfig(argv[2])) return 1;
    return 0;
}
