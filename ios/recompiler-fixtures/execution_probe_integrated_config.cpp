#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "execution_probe_route.h"
#include "execution_probe_secondary_route.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: execution_probe_integrated_config <config.toml> <default.xbe>\n");
        return 2;
    }
    const std::filesystem::path xbe = argv[2];
    if (!std::filesystem::exists(xbe)) {
        std::fprintf(stderr, "integrated probe: real XBE is missing: %s\n", xbe.string().c_str());
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
              "end = 0x002B3988\n\n"
              "[[function]]\n"
              "at = 0x002B5CA7\n"
              "end = 0x002B5D15\n\n"
              "[[function]]\n"
              "at = 0x002B5CEC\n"
              "end = 0x002B5CF4\n\n"
              "[[function]]\n"
              "at = 0x002B55EE\n"
              "end = 0x002B55EF\n\n"
              "[[function]]\n"
              "at = 0x002B2759\n"
              "end = 0x002B2777\n\n"
              "[[function]]\n"
              "at = 0x002B3826\n"
              "end = 0x002B382A\n\n"
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
    for (const auto& step : execution_probe::kRoute) {
        config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.pc
               << "\ncall = \"ProbeStep_0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << step.pc << "\"\n";
    }
    for (const auto& step : execution_probe_secondary::kRoute) {
        config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.pc
               << "\ncall = \"SecondaryStep_0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << step.pc << "\"\n";
    }
    for (uint32_t pc : execution_probe_secondary::kTrapPcs) {
        config << "\n[[hook]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << pc
               << "\ncall = \"SecondaryStep_0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << pc << "\"\n";
    }
    std::ofstream file(argv[1]);
    file << config.str();
    return file ? 0 : 1;
}
