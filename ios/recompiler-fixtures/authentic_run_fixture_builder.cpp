// Phase 2D-19 synthetic rehearsal fixture builder.
// Emits a synthetic XBE whose reviewed route occupies known addresses,
// and a recompiler config that gives every reviewed instruction its own
// generated function. No authentic game bytes are used.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "xbe/xbe.h"

namespace {

constexpr uint32_t kBase = 0x10000;
constexpr uint32_t kEntry = 0x11000;
constexpr uint32_t kSectionTable = 0x180;
constexpr uint32_t kSectionName = 0x240;
constexpr uint32_t kRawCode = 0x1000;
constexpr uint32_t kSectionSize = 0x1000;

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
    bytes[offset + 2] = uint8_t(value >> 16);
    bytes[offset + 3] = uint8_t(value >> 24);
}

struct Chunk { uint32_t address; std::vector<uint8_t> bytes; };

bool Write(const std::filesystem::path& xbe_path, const std::filesystem::path& config_path) {
    std::vector<Chunk> chunks = {
        {0x00011000u, {0x68, 0x34, 0x12, 0x00, 0x00}},
        {0x00011005u, {0x59}},
        {0x00011006u, {0x81, 0xF9, 0x34, 0x12, 0x00, 0x00}},
        {0x0001100Cu, {0x74, 0x06}},
        {0x00011014u, {0x68, 0x00, 0x11, 0x01, 0x00}},
        {0x00011019u, {0x6A, 0x00}},
        {0x0001101Bu, {0x6A, 0x00}},
        {0x0001101Du, {0x6A, 0x00}},
        {0x0001101Fu, {0x68, 0x80, 0x12, 0x01, 0x00}},
        {0x00011024u, {0x68, 0x04, 0x30, 0x01, 0x00}},
        {0x00011029u, {0x68, 0x94, 0x00, 0x00, 0x00}},
        {0x0001102Eu, {0x68, 0x00, 0x00, 0x01, 0x00}},
        {0x00011033u, {0x6A, 0x00}},
        {0x00011035u, {0x68, 0x00, 0x30, 0x01, 0x00}},
        {0x0001103Au, {0xFF, 0x15, 0x00, 0x40, 0x01, 0x00}},
        {0x00011040u, {0xA3, 0x08, 0x30, 0x01, 0x00}},
        {0x00011045u, {0xFF, 0x35, 0x00, 0x30, 0x01, 0x00}},
        {0x0001104Bu, {0xFF, 0x15, 0x04, 0x40, 0x01, 0x00}},
        {0x00011051u, {0xA3, 0x0C, 0x30, 0x01, 0x00}},
        {0x00011200u, {0x68, 0x90, 0x12, 0x01, 0x00}},
        {0x00011205u, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}},
        {0x0001120Bu, {0x50}},
        {0x0001120Cu, {0x8B, 0x44, 0x24, 0x10}},
        {0x00011210u, {0x89, 0x6C, 0x24, 0x10}},
        {0x00011214u, {0x8D, 0x6C, 0x24, 0x10}},
        {0x00011218u, {0x29, 0xC4}},
        {0x0001121Au, {0x53}},
        {0x0001121Bu, {0x56}},
        {0x0001121Cu, {0x57}},
        {0x0001121Du, {0x8B, 0x45, 0xF8}},
        {0x00011220u, {0x89, 0x65, 0xE8}},
        {0x00011223u, {0x50}},
        {0x00011224u, {0x8B, 0x45, 0xFC}},
        {0x00011227u, {0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF}},
        {0x0001122Eu, {0x89, 0x45, 0xF8}},
        {0x00011231u, {0x8D, 0x45, 0xF0}},
        {0x00011234u, {0x64, 0xA3, 0x00, 0x00, 0x00, 0x00}},
        {0x0001123Au, {0xC3}},
        {0x00011100u, {0x68, 0x18, 0x00, 0x00, 0x00}},
        {0x00011105u, {0x68, 0x10, 0x30, 0x01, 0x00}},
        {0x0001110Au, {0xE8, 0xF1, 0x00, 0x00, 0x00}},
        {0x0001110Fu, {0x64, 0xA1, 0x28, 0x00, 0x00, 0x00}},
        {0x00011115u, {0x8B, 0x50, 0x28}},
        {0x00011118u, {0x8D, 0x4A, 0x04}},
        {0x0001111Bu, {0x89, 0x0A}},
        {0x0001111Du, {0x68, 0x20, 0x30, 0x01, 0x00}},
        {0x00011122u, {0xFF, 0x15, 0x08, 0x40, 0x01, 0x00}},
        {0x00011128u, {0x68, 0x20, 0x30, 0x01, 0x00}},
        {0x0001112Du, {0xFF, 0x15, 0x0C, 0x40, 0x01, 0x00}},
        {0x00011133u, {0x8B, 0x55, 0x08}},
        {0x00011136u, {0xFF, 0x75, 0x0C}},
        {0x00011139u, {0xFF, 0xD2}},
        {0x00011056u, {0xC3}},
        {0x00011280u, {0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0xC3}},
        {0x00011290u, {0xC7, 0x05, 0x14, 0x30, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0xC0, 0xC3}},
        {0x0001100Eu, {0xBB, 0xAD, 0x0B, 0x00, 0x00}},
        {0x00011300u, {0x87, 0x07}},
        {0x00011302u, {0xC3}},
        {0x00011310u, {0xA1, 0x00, 0x00, 0x02, 0x00}},
        {0x00011315u, {0xC3}},
    };
    std::vector<uint8_t> code(kSectionSize, 0xCC);
    for (const Chunk& chunk : chunks) {
        if (chunk.address < kEntry || chunk.address + chunk.bytes.size() > kEntry + kSectionSize) return false;
        std::copy(chunk.bytes.begin(), chunk.bytes.end(), code.begin() + (chunk.address - kEntry));
    }
    constexpr size_t header_size = 0x1000;
    std::vector<uint8_t> image(kRawCode + code.size(), 0);
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
    Put32(image, kSectionTable + 8, kSectionSize);
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
              "path = \"authentic_run_fixture.xbe\"\n"
              "entry = 0x11000\n"
              "out = \"generated\"\n"
              "partitions = 1\n"
              "code_sections = [\".text\"]\n"
              "pointer_seeds = false\n"
              "gap_fill = false\n"
              "callback_seeds = false\n"
              "entry_seeds = false\n"
              "checked_memory = true\n";
    config << "\n[[function]]\nat = 0x00011000\nend = 0x00011005\n";
    config << "\n[[function]]\nat = 0x00011005\nend = 0x00011006\n";
    config << "\n[[function]]\nat = 0x00011006\nend = 0x0001100C\n";
    config << "\n[[function]]\nat = 0x0001100C\nend = 0x0001100E\n";
    config << "\n[[function]]\nat = 0x0001100E\nend = 0x00011013\n";
    config << "\n[[function]]\nat = 0x00011014\nend = 0x00011019\n";
    config << "\n[[function]]\nat = 0x00011019\nend = 0x0001101B\n";
    config << "\n[[function]]\nat = 0x0001101B\nend = 0x0001101D\n";
    config << "\n[[function]]\nat = 0x0001101D\nend = 0x0001101F\n";
    config << "\n[[function]]\nat = 0x0001101F\nend = 0x00011024\n";
    config << "\n[[function]]\nat = 0x00011024\nend = 0x00011029\n";
    config << "\n[[function]]\nat = 0x00011029\nend = 0x0001102E\n";
    config << "\n[[function]]\nat = 0x0001102E\nend = 0x00011033\n";
    config << "\n[[function]]\nat = 0x00011033\nend = 0x00011035\n";
    config << "\n[[function]]\nat = 0x00011035\nend = 0x0001103A\n";
    config << "\n[[function]]\nat = 0x0001103A\nend = 0x00011040\n";
    config << "\n[[function]]\nat = 0x00011040\nend = 0x00011045\n";
    config << "\n[[function]]\nat = 0x00011045\nend = 0x0001104B\n";
    config << "\n[[function]]\nat = 0x0001104B\nend = 0x00011051\n";
    config << "\n[[function]]\nat = 0x00011051\nend = 0x00011056\n";
    config << "\n[[function]]\nat = 0x00011056\nend = 0x00011057\n";
    config << "\n[[function]]\nat = 0x00011100\nend = 0x00011105\n";
    config << "\n[[function]]\nat = 0x00011105\nend = 0x0001110A\n";
    config << "\n[[function]]\nat = 0x0001110A\nend = 0x0001110F\n";
    config << "\n[[function]]\nat = 0x0001110F\nend = 0x00011115\n";
    config << "\n[[function]]\nat = 0x00011115\nend = 0x00011118\n";
    config << "\n[[function]]\nat = 0x00011118\nend = 0x0001111B\n";
    config << "\n[[function]]\nat = 0x0001111B\nend = 0x0001111D\n";
    config << "\n[[function]]\nat = 0x0001111D\nend = 0x00011122\n";
    config << "\n[[function]]\nat = 0x00011122\nend = 0x00011128\n";
    config << "\n[[function]]\nat = 0x00011128\nend = 0x0001112D\n";
    config << "\n[[function]]\nat = 0x0001112D\nend = 0x00011133\n";
    config << "\n[[function]]\nat = 0x00011133\nend = 0x00011136\n";
    config << "\n[[function]]\nat = 0x00011136\nend = 0x00011139\n";
    config << "\n[[function]]\nat = 0x00011139\nend = 0x0001113B\n";
    config << "\n[[function]]\nat = 0x00011200\nend = 0x00011205\n";
    config << "\n[[function]]\nat = 0x00011205\nend = 0x0001120B\n";
    config << "\n[[function]]\nat = 0x0001120B\nend = 0x0001120C\n";
    config << "\n[[function]]\nat = 0x0001120C\nend = 0x00011210\n";
    config << "\n[[function]]\nat = 0x00011210\nend = 0x00011214\n";
    config << "\n[[function]]\nat = 0x00011214\nend = 0x00011218\n";
    config << "\n[[function]]\nat = 0x00011218\nend = 0x0001121A\n";
    config << "\n[[function]]\nat = 0x0001121A\nend = 0x0001121B\n";
    config << "\n[[function]]\nat = 0x0001121B\nend = 0x0001121C\n";
    config << "\n[[function]]\nat = 0x0001121C\nend = 0x0001121D\n";
    config << "\n[[function]]\nat = 0x0001121D\nend = 0x00011220\n";
    config << "\n[[function]]\nat = 0x00011220\nend = 0x00011223\n";
    config << "\n[[function]]\nat = 0x00011223\nend = 0x00011224\n";
    config << "\n[[function]]\nat = 0x00011224\nend = 0x00011227\n";
    config << "\n[[function]]\nat = 0x00011227\nend = 0x0001122E\n";
    config << "\n[[function]]\nat = 0x0001122E\nend = 0x00011231\n";
    config << "\n[[function]]\nat = 0x00011231\nend = 0x00011234\n";
    config << "\n[[function]]\nat = 0x00011234\nend = 0x0001123A\n";
    config << "\n[[function]]\nat = 0x0001123A\nend = 0x0001123B\n";
    config << "\n[[function]]\nat = 0x00011280\nend = 0x00011286\n";
    config << "\n[[function]]\nat = 0x00011290\nend = 0x0001129D\n";
    config << "\n[[function]]\nat = 0x00011300\nend = 0x00011302\n";
    config << "\n[[function]]\nat = 0x00011302\nend = 0x00011303\n";
    config << "\n[[function]]\nat = 0x00011310\nend = 0x00011315\n";
    config << "\n[[function]]\nat = 0x00011315\nend = 0x00011316\n";
    return bool(config);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    return Write(argv[1], argv[2]) ? 0 : 1;
}
