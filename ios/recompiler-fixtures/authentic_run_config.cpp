// Phase 2D-19: writes the recompiler config for the authentic experiment.
// Every reviewed instruction is its own generated function, so every control
// transfer is a checked dispatch boundary and the terminal boundary can be
// refused before the target executes. The tool writes configuration only; it
// never reads or executes the image.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "kernel/authentic_run_route.h"

namespace route = kernel::authentic::route;

namespace {

void EmitFunction(std::ostringstream& config, uint32_t at, uint32_t end) {
    config << "\n[[function]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << at
           << "\nend = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << end << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: authentic_run_config <config.toml>\n");
        return 2;
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
              "entry_seeds = false\n"
              "checked_memory = true\n";
    for (size_t i = 0; i < route::kParentRouteCount; ++i)
        EmitFunction(config, route::kParentRoute[i].pc, route::kParentRoute[i].pc + route::kParentRoute[i].length);
    for (size_t i = 0; i < route::kChildRouteCount; ++i)
        EmitFunction(config, route::kChildRoute[i].pc, route::kChildRoute[i].pc + route::kChildRoute[i].length);
    // Trap-only entries: they are present so the prepared-thread admission can
    // resolve the reviewed start routine and handler, but no route edge targets
    // them. The boundary gate refuses every transfer into them.
    // Trap-only entries. The two stops and the SEH handler make the prepared
    // start routine and handler resolvable; the forbidden branch arms make the
    // recompiler emit a checked edge for every reviewed conditional target so
    // the gate can classify and refuse it. None of these is ever executed.
    EmitFunction(config, route::kParentStop, route::kParentStop + 1);
    EmitFunction(config, 0x002B5CEC, 0x002B5CEC + 8);
    EmitFunction(config, 0x002B3975, 0x002B3975 + 1);
    EmitFunction(config, 0x002B276C, 0x002B276C + 1);
    EmitFunction(config, route::kChildStop, route::kChildStop + 1);
    EmitFunction(config, route::kSehHandler, route::kSehHandler + 1);
    EmitFunction(config, 0x002B5999, 0x002B5999 + 4);
    EmitFunction(config, 0x002B59A1, 0x002B59A1 + 3);
    std::ofstream file(argv[1]);
    file << config.str();
    return file ? 0 : 1;
}
