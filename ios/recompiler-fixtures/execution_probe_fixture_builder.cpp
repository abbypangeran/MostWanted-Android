#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "execution_probe_route.h"
#include "xbe/xbe.h"

namespace {

using execution_probe::kHeaderSize;
using execution_probe::kImageBase;
using execution_probe::kImageSize;
using execution_probe::kSectionRaw;
using execution_probe::kSectionSize;
using execution_probe::kSectionVa;

struct Instruction {
    uint32_t va;
    std::array<uint8_t, 7> bytes;
    uint8_t length;
};

// Synthetic encoding of the audited instruction shapes.  Lengths are the
// recompiler's route lengths from execution_probe_route.h.
constexpr Instruction kInstructions[] = {
    {0x002B3916, {0x8B, 0x0D, 0x18, 0x01, 0x01, 0x00}, 6},              // mov ecx, [0x10118]
    {0x002B391C, {0xA1, 0x08, 0x01, 0x01, 0x00}, 5},                    // mov eax, [0x10108]
    {0x002B3921, {0x2B, 0xC1}, 2},                                      // sub eax, ecx
    {0x002B3923, {0x05, 0x00, 0x00, 0x01, 0x00}, 5},                    // add eax, 0x10000
    {0x002B3928, {0x3B, 0x01}, 2},                                      // cmp eax, [ecx]
    {0x002B392A, {0x73, 0x02}, 2},                                      // jnb 0x2B392E
    {0x002B392C, {0x89, 0x01}, 2},                                      // mov [ecx], eax
    {0x002B392E, {0xA1, 0xFC, 0xD3, 0x3E, 0x00}, 5},                    // mov eax, [0x3ED3FC]
    {0x002B3933, {0x2B, 0x05, 0xEC, 0xD3, 0x3E, 0x00}, 6},              // sub eax, [0x3ED3EC]
    {0x002B3939, {0x8B, 0x0D, 0xF0, 0xD3, 0x3E, 0x00}, 6},              // mov ecx, [0x3ED3F0]
    {0x002B393F, {0x56}, 1},                                            // push esi
    {0x002B3940, {0x8D, 0x44, 0x08, 0x0F}, 4},                          // lea eax, [eax+ecx+0xF]
    {0x002B3944, {0x57}, 1},                                            // push edi
    {0x002B3945, {0x83, 0xE0, 0xF0}, 3},                                // and eax, 0xFFFFFFF0
    {0x002B3948, {0x6A, 0xFC}, 2},                                      // push -4
    {0x002B394A, {0x59}, 1},                                            // pop ecx
    {0x002B394B, {0x83, 0xC0, 0x04}, 3},                                // add eax, 4
    {0x002B394E, {0xA3, 0x74, 0xDC, 0x4E, 0x00}, 5},                    // mov [0x4EDC74], eax
    {0x002B3953, {0x99}, 1},                                            // cdq
    {0x002B3954, {0xF7, 0xF9}, 2},                                      // idiv ecx
    {0x002B3956, {0x8B, 0x0D, 0xF4, 0xD3, 0x3E, 0x00}, 6},              // mov ecx, [0x3ED3F4]
    {0x002B395C, {0x31, 0xF6}, 2},                                      // xor esi, esi
    {0x002B395E, {0x56}, 1},                                            // push esi
    {0x002B395F, {0x56}, 1},                                            // push esi
    {0x002B3960, {0x56}, 1},                                            // push esi
    {0x002B3961, {0x68, 0xA2, 0x38, 0x2B, 0x00}, 5},                    // push 0x2B38A2
    {0x002B3966, {0x56}, 1},                                            // push esi
    {0x002B3967, {0x56}, 1},                                            // push esi
    {0x002B3968, {0x89, 0x01}, 2},                                      // mov [ecx], eax
    {0x002B396A, {0xE8, 0x38, 0x23, 0x00, 0x00}, 5},                    // call 0x2B5CA7
    {0x002B5CA7, {0x55}, 1},                                            // push ebp
    {0x002B5CA8, {0x8B, 0xEC}, 2},                                      // mov ebp, esp
    {0x002B5CAA, {0x8B, 0x45, 0x0C}, 3},                                // mov eax, [ebp+0xC]
    {0x002B5CAD, {0x85, 0xC0}, 2},                                      // test eax, eax
    {0x002B5CAF, {0x75, 0x05}, 2},                                      // jnz 0x2B5CB6
    {0x002B5CB1, {0xA1, 0x30, 0x01, 0x01, 0x00}, 5},                    // mov eax, [0x10130]
    {0x002B5CB6, {0x8B, 0x4D, 0x18}, 3},                                // mov ecx, [ebp+0x18]
    {0x002B5CB9, {0x68, 0x0F, 0x5C, 0x2B, 0x00}, 5},                    // push 0x2B5C0F
    {0x002B5CBE, {0x6A, 0x00}, 2},                                      // push 0
    {0x002B5CC0, {0xC1, 0xE9, 0x02}, 3},                                // shr ecx, 2
    {0x002B5CC3, {0x81, 0xE1, 0x01, 0xFF, 0xFF, 0xFF}, 6},              // and ecx, 0xFFFFFF01
    {0x002B5CC9, {0x51}, 1},                                            // push ecx
    {0x002B5CCA, {0xFF, 0x75, 0x14}, 3},                                // push [ebp+0x14]
    {0x002B5CCD, {0xFF, 0x75, 0x10}, 3},                                // push [ebp+0x10]
    {0x002B5CD0, {0xFF, 0x75, 0x1C}, 3},                                // push [ebp+0x1C]
    {0x002B5CD3, {0xFF, 0x35, 0x74, 0xDC, 0x4E, 0x00}, 6},              // push [0x4EDC74]
    {0x002B5CD9, {0x50}, 1},                                            // push eax
    {0x002B5CDA, {0x6A, 0x00}, 2},                                      // push 0
    {0x002B5CDC, {0x8D, 0x45, 0x0C}, 3},                                // lea eax, [ebp+0xC]
    {0x002B5CDF, {0x50}, 1},                                            // push eax
    {0x002B5CE0, {0xFF, 0x15, 0x68, 0x9E, 0x3C, 0x00}, 6},              // call [0x3C9E68] - forbidden
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
    Put32(image, 0x128, execution_probe::kEntry ^ xbe::kRetailEntryKey);
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
              "path = \"execution_probe_fixture.xbe\"\n"
              "entry = 0x002B3916\n"
              "out = \"generated\"\n"
              "partitions = 1\n"
              "code_sections = [\".text\"]\n"
              "pointer_seeds = false\n"
              "gap_fill = false\n"
              "callback_seeds = false\n"
              "checked_memory = true\n\n"
              "[[function]]\n"
              "at = 0x002B3916\n"
              "end = 0x002B396F\n\n"
              "[[function]]\n"
              "at = 0x002B5CA7\n"
              "end = 0x002B5CE0\n";
    for (const auto& step : execution_probe::kRoute) {
        config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.pc
               << "\ncall = \"ProbeStep_0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.pc
               << "\"\n";
    }
    config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << execution_probe::kLastInstruction << "\ncall = \"ProbeStop_0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill('0') << execution_probe::kLastInstruction << "\"\nafter = true\n";
    std::ofstream file(path);
    file << config.str();
    return bool(file);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: execution_probe_fixture_builder <xbe> <config>\n");
        return 2;
    }
    std::vector<uint8_t> image;
    if (!BuildImage(image)) {
        std::fprintf(stderr, "fixture code does not fit its section\n");
        return 1;
    }
    std::ofstream xbe(argv[1], std::ios::binary);
    xbe.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    if (!xbe) return 1;
    if (!WriteConfig(argv[2])) return 1;
    return 0;
}
