#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "execution_probe_route.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: execution_probe_real_config <config.toml> <default.xbe>\n");
        return 2;
    }
    const std::filesystem::path xbe = argv[2];
    if (!std::filesystem::exists(xbe)) {
        std::fprintf(stderr, "execution probe: real XBE is missing: %s\n", xbe.string().c_str());
        return 1;
    }
    std::ostringstream config;
    config << "[xbe]\n"
              "path = \"default.xbe\"\n"
              "title = 0x4541007B\n"
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
               << "\ncall = \"ProbeStep_0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << step.pc << "\"\n";
    }
    config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << execution_probe::kLastInstruction << "\ncall = \"ProbeStop_0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill('0') << execution_probe::kLastInstruction << "\"\nafter = true\n";
    std::ofstream file(argv[1]);
    file << config.str();
    return file ? 0 : 1;
}
