// Phase 2C-31 synthetic fixture for the secondary-thread entry contract.
//
// The image places the reviewed system-routine prefix, the start-routine entry
// and the two unreviewed callee entries at their authentic addresses.  Only the
// first two system-routine instructions are executed; the other entries exist
// so the recompiler can name them and the callback-ticket contract can resolve
// the start routine without executing it.

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
    {0x002B5C0F, {0x6A, 0x18}, 2},                           // push 0x18
    {0x002B5C11, {0x68, 0xB8, 0xD5, 0x3E, 0x00}, 5},         // push 0x3ED5B8
    {0x002B5C16, {0xE8, 0xE9, 0x3B, 0x06, 0x00}, 5},         // call 0x00319804, stop boundary
    {0x002B38A2, {0xE8, 0x11, 0x20, 0x00, 0x00}, 5},         // call 0x002B58B8, trap boundary
    {0x002B58B8, {0xA1, 0x34, 0x9E, 0x3C, 0x00}, 5},         // mov eax, [0x003C9E34], trap boundary
    {0x00319804, {0x68, 0x58, 0x98, 0x31, 0x00}, 5},         // push 0x319858, trap boundary
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
              "end = 0x002B5C1B\n\n"
              "[[function]]\n"
              "at = 0x002B38A2\n"
              "end = 0x002B38A7\n\n"
              "[[function]]\n"
              "at = 0x002B58B8\n"
              "end = 0x002B58BD\n\n"
              "[[function]]\n"
              "at = 0x00319804\n"
              "end = 0x00319809\n";
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
