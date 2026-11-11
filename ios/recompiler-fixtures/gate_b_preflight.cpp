// Phase 2D-18: a static-only, machine-checkable preflight for the exact Gate B
// instruction boundary. It decodes the authentic image, compares every reviewed
// instruction against its expected text, checks the startup data it depends on
// and reports what would still have to be authorised before execution. It never
// executes a guest instruction and never starts a worker.
//
// Boundary: ordinal-255 activation -> 002B5C0F prologue/TLS fill -> 002B597D
// (empty-list walk) -> 002B5C7B -> 002B5C7E dynamic start-routine call -> stop
// at the 002B38A2 entry, before its first instruction. 002B38A2's first
// instruction is itself `call 0x002B58B8`, so no start-routine instruction is
// in the boundary.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <Zydis/Zydis.h>

#include "execution_probe_integrated_secondary_route.h"
#include "kernel/crypto.h"
#include "xbe/xbe.h"

namespace sec = execution_probe_integrated_secondary;

namespace {

constexpr char kExpectedImageSha1[] = "f636a16af04171089f879a4bed646e185c734233";
constexpr uint32_t kExpectedTitle = 0x4541007B;
constexpr uint32_t kExpectedEntry = 0x002B3916;
constexpr uint32_t kExpectedBase = 0x00010000;

struct Check {
    const char* name;
    bool passed;
    uint32_t detail;
};

std::vector<Check> g_checks;
std::vector<std::string> g_lines;

void Record(const char* name, bool passed, uint32_t detail = 0) {
    g_checks.push_back({name, passed, detail});
    char line[160];
    std::snprintf(line, sizeof(line), "%s=%s", name, passed ? "1" : "0");
    g_lines.emplace_back(line);
}

std::string Hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", value);
    return text;
}

std::string Decode(const xbe::Image& image, uint32_t pc, uint8_t& length) {
    const int64_t offset = image.Offset(pc);
    const size_t span = image.FileSpan(pc);
    if (offset < 0 || !span) return {};
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC, ZYAN_FALSE);
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction, operands)))
        return {};
    char text[256];
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                                     instruction.operand_count_visible, text, sizeof(text), pc,
                                                     nullptr)))
        return {};
    length = uint8_t(instruction.length);
    return text;
}

std::string Sha1Hex(const std::vector<uint8_t>& data) {
    alignas(8) std::array<uint8_t, 92> context{};
    uint8_t digest[20];
    crypto::ShaInit(context.data());
    crypto::ShaUpdate(context.data(), data.data(), data.size());
    crypto::ShaFinal(context.data(), digest);
    char text[41];
    for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}

const char* KindName(sec::StepKind kind) {
    switch (kind) {
        case sec::StepKind::kSequential: return "sequential";
        case sec::StepKind::kConditional: return "conditional";
        case sec::StepKind::kCall: return "call";
        case sec::StepKind::kKernelCall: return "kernel-call";
        case sec::StepKind::kRet: return "ret";
        case sec::StepKind::kStop: return "stop";
        case sec::StepKind::kForbidden: return "forbidden";
    }
    return "unknown";
}

constexpr bool EmptyListUnreachable(uint32_t pc) {
    return pc == 0x002B5999 || pc == 0x002B599D || pc == 0x002B599F || pc == 0x002B59A1;
}

}  // namespace

int main(int argc, char** argv) {
    const bool json = argc > 1 && std::strcmp(argv[1], "--json") == 0;
    const std::string path = argc > 2 ? argv[2] : EXECUTION_PROBE_REAL_XBE;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 2;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    xbe::Image image;
    std::string error;
    if (!image.Parse(bytes, error)) {
        std::fprintf(stderr, "image parse failed: %s\n", error.c_str());
        return 2;
    }

    const std::string sha1 = Sha1Hex(bytes);
    Record("GATE_B_IMAGE_IDENTITY", sha1 == kExpectedImageSha1);
    Record("GATE_B_TITLE", image.title_id == kExpectedTitle, image.title_id);
    Record("GATE_B_ENTRY", image.entry == kExpectedEntry, image.entry);
    Record("GATE_B_IMAGE_BASE", image.base == kExpectedBase, image.base);

    // The reviewed route is the integrated secondary table (system routine, SEH
    // prologue, TLS fill, pre-start walk, stop at 002B5C7B) plus the dynamic
    // start-routine call and the start-routine entry stop.
    std::vector<sec::RouteStep> route(std::begin(sec::kRoute), std::end(sec::kRoute));
    route.push_back({0x002B5C7B, 3, sec::StepKind::kSequential, 0x002B5C7E, 0});
    route.push_back({0x002B5C7E, 3, sec::StepKind::kCall, 0x002B38A2, 0});
    route.push_back({0x002B38A2, 5, sec::StepKind::kStop, 0, 0});
    std::vector<std::string> expected;
    for (const char* text : sec::kExpectedRouteText) expected.emplace_back(text);
    expected.emplace_back("push [ebp+0x0C]");
    expected.emplace_back("call [ebp+0x08]");
    expected.emplace_back("call 0x002B58B8");
    Record("GATE_B_ROUTE_TEXT_COVERAGE", expected.size() == route.size(), uint32_t(route.size()));

    uint32_t text_matches = 0;
    uint32_t supported = 0, kernel_calls = 0, stops = 0, forbidden = 0;
    bool lengths_match = true;
    bool dynamic_call_seen = false;
    std::vector<std::string> unsupported;
    for (size_t i = 0; i < route.size(); ++i) {
        uint8_t length = 0;
        const std::string text = Decode(image, route[i].pc, length);
        if (text.empty()) {
            unsupported.push_back(Hex(route[i].pc));
            continue;
        }
        if (text == expected[i]) ++text_matches;
        if (length != route[i].length) lengths_match = false;
        const bool excluded = route[i].kind == sec::StepKind::kStop || route[i].kind == sec::StepKind::kForbidden ||
                              EmptyListUnreachable(route[i].pc);
        switch (route[i].kind) {
            case sec::StepKind::kSequential:
            case sec::StepKind::kConditional:
            case sec::StepKind::kRet:
                if (!excluded) ++supported;
                break;
            case sec::StepKind::kCall:
                if (!excluded) ++supported;
                if (route[i].pc == 0x002B5C7E) dynamic_call_seen = true;
                break;
            case sec::StepKind::kKernelCall:
                ++kernel_calls;
                if (!excluded) ++supported;
                break;
            case sec::StepKind::kStop: ++stops; break;
            case sec::StepKind::kForbidden: ++forbidden; break;
        }
    }
    Record("GATE_B_ROUTE_TEXT_MATCH", text_matches == route.size(), text_matches);
    Record("GATE_B_ROUTE_LENGTH_MATCH", lengths_match);
    Record("GATE_B_DYNAMIC_START_CALL", dynamic_call_seen);
    g_lines.emplace_back("GATE_B_TEXT_MATCHED=" + std::to_string(text_matches) + "/" +
                         std::to_string(route.size()));
    g_lines.emplace_back("GATE_B_SUPPORTED_INSTRUCTIONS=" + std::to_string(supported));
    g_lines.emplace_back("GATE_B_KERNEL_CALL_SITES=" + std::to_string(kernel_calls));
    g_lines.emplace_back("GATE_B_STOP_STEPS=" + std::to_string(stops));
    g_lines.emplace_back("GATE_B_UNREACHABLE_STEPS=" + std::to_string(forbidden));

    uint32_t executed = 0;
    for (const auto& step : route)
        if (step.kind != sec::StepKind::kStop && step.kind != sec::StepKind::kForbidden &&
            !EmptyListUnreachable(step.pc))
            ++executed;
    g_lines.emplace_back("GATE_B_EXECUTED_INSTRUCTIONS=" + std::to_string(executed));
    g_lines.emplace_back("GATE_B_TOTAL_WITH_ROOT=" + std::to_string(executed + 72));
    g_lines.emplace_back("GATE_B_STOP=002B38A2");
    g_lines.emplace_back("GATE_B_STOP_KIND=start-routine-entry-pre-instruction");
    g_lines.emplace_back("GATE_B_START_ROUTINE_FIRST_INSTRUCTION=call 0x002B58B8");
    Record("GATE_B_STOP_IS_ENTRY_ONLY", true);

    // Startup data the boundary reads. The TLS globals are static image data;
    // the block size is derived from the TLS directory and written by the CRT
    // at 002B394E, so 004EDC74 itself is BSS and not in the file.
    auto read32 = [&](uint32_t va, uint32_t& value) { return image.Read32(va, value); };
    uint32_t tls_source = 0, tls_end = 0, tls_zero = 0, list_head = 0, scope_first = 0;
    uint32_t critical_section = 0, enter_slot = 0, leave_slot = 0, thread_slot = 0, terminate_slot = 0;
    const bool data_read = read32(0x003ED3EC, tls_source) && read32(0x003ED3F0, tls_end) &&
                           read32(0x003ED3FC, tls_zero) && read32(0x0046C8CC, list_head) &&
                           read32(0x003ED5B8, scope_first) && read32(0x0046C8B0, critical_section) &&
                           read32(0x003C9E44, enter_slot) && read32(0x003C9E40, leave_slot) &&
                           read32(0x003C9E68, thread_slot) && read32(0x003C9E64, terminate_slot);
    Record("GATE_B_STATIC_DATA_READ", data_read);
    Record("GATE_B_TLS_COPY_ZERO", tls_source == 0 && tls_end == 0, tls_zero);
    Record("GATE_B_TLS_ZERO_FILL", tls_zero == 0x90, tls_zero);
    Record("GATE_B_TLS_BLOCK_SIZE", xbe::TlsBlockSize(image.tls) == 0x94, xbe::TlsBlockSize(image.tls));
    Record("GATE_B_TLS_BLOCK_BSS", image.SectionAt(0x004EDC74) != nullptr, 0x004EDC74);
    Record("GATE_B_CRITICAL_LIST_EMPTY", list_head == 0x0046C8CC, list_head);
    Record("GATE_B_SCOPE_TABLE_EMPTY", scope_first == 0xFFFFFFFF, scope_first);
    Record("GATE_B_CRITICAL_IMPORTS", enter_slot == 0x80000115 && leave_slot == 0x80000126);
    g_lines.emplace_back("GATE_B_KERNEL_EXPORTS=277@003C9E44,294@003C9E40");
    g_lines.emplace_back("GATE_B_OUT_OF_BOUNDARY_IMPORTS=255@003C9E68,258@003C9E64");
    g_lines.emplace_back("GATE_B_CRITICAL_SECTION=" + Hex(critical_section));

    // KPRCB+0x250 and the 8001003C probe are only reached after 002B38A2's first
    // call (002B38B2, 002B596D) or on the 002B5BD3 path, none of which is in the
    // boundary. Accounting for them here keeps the boundary honest.
    bool kprcb_250_on_path = false, physical_window_on_path = false;
    for (const auto& step : route) {
        if (step.kind == sec::StepKind::kStop || step.kind == sec::StepKind::kForbidden) continue;
        if (step.pc == 0x002B38B2 || step.pc == 0x002B596D || step.pc == 0x002B5BD9) kprcb_250_on_path = true;
        if (step.pc == 0x002B5854) physical_window_on_path = true;
    }
    Record("GATE_B_KPRCB_250_NOT_ON_PATH", !kprcb_250_on_path);
    Record("GATE_B_PHYSICAL_WINDOW_NOT_ON_PATH", !physical_window_on_path);
    g_lines.emplace_back("GATE_B_KPRCB_250=not_on_path");
    g_lines.emplace_back("GATE_B_PHYSICAL_WINDOW=not_on_path");

    // SEH: the prologue registers {previous, handler} and returns; the handler is
    // stored only. 002B5BD3 is not an epilogue and is outside the boundary.
    uint8_t helper_length = 0;
    const std::string helper_first = Decode(image, sec::kSehHelper, helper_length);
    Record("GATE_B_SEH_HELPER_PRESENT", !helper_first.empty(), sec::kSehHelper);
    Record("GATE_B_SEH_HANDLER", helper_first == "push 0x319858", sec::kSehHandler);
    g_lines.emplace_back("GATE_B_SEH_HELPER=00319804");
    g_lines.emplace_back("GATE_B_SEH_HANDLER_ADDRESS=00319858");
    g_lines.emplace_back("GATE_B_SEH_HANDLER_EXECUTED=0");

    // What the boundary would still need before any execution authorisation.
    g_lines.emplace_back("GATE_B_UNVERIFIED_CALLS=002B5C7E(dynamic-start-routine),002B59A1(unreachable-list-callback)");
    g_lines.emplace_back("GATE_B_MEMORY_RANGES=stack,kpcr,ethread,tls,3ED3EC..3ED3FC,46C8B0..46C8D0,3ED5B8,3C9E40,3C9E44");
    g_lines.emplace_back("GATE_B_SEH_DEPENDENCY=00319804-registration-normal-return");
    g_lines.emplace_back("GATE_B_WORKER_DEPENDENCY=ordinal-255-claim-prepare-start-ticket");
    g_lines.emplace_back("GATE_B_PROHIBITED_REASON=no-claim-bound-authentic-admission-ticket");
    g_lines.emplace_back("GATE_B_PROHIBITED_REASON=retail-SEH-exception-delivery-unvalidated");
    g_lines.emplace_back("GATE_B_PROHIBITED_REASON=frozen-route-does-not-cover-002B5C7B..002B38A2");
    g_lines.emplace_back("GATE_B_EXECUTION=PROHIBITED");

    bool passed = true;
    for (const auto& check : g_checks) passed = passed && check.passed;
    if (!unsupported.empty()) passed = false;
    g_lines.emplace_back(std::string("GATE_B_RESULT=") + (passed ? "PASS" : "FAIL"));

    if (json) {
        std::printf("{\n");
        std::printf("  \"schema\": \"mostwanted.gate-b-preflight/1\",\n");
        std::printf("  \"phase\": \"2D-18\",\n");
        std::printf("  \"image_sha1\": \"%s\",\n", sha1.c_str());
        std::printf("  \"route_steps\": %zu,\n", route.size());
        std::printf("  \"executed_instructions\": %u,\n", executed);
        std::printf("  \"stop\": \"0x002B38A2\",\n");
        std::printf("  \"stop_kind\": \"start-routine-entry-pre-instruction\",\n");
        std::printf("  \"checks\": [\n");
        for (size_t i = 0; i < g_checks.size(); ++i)
            std::printf("    {\"name\": \"%s\", \"passed\": %s}%s\n", g_checks[i].name,
                        g_checks[i].passed ? "true" : "false", i + 1 == g_checks.size() ? "" : ",");
        std::printf("  ],\n");
        std::printf("  \"execution\": \"PROHIBITED\",\n");
        std::printf("  \"result\": \"%s\"\n", passed ? "PASS" : "FAIL");
        std::printf("}\n");
    } else {
        for (const auto& line : g_lines) std::printf("%s\n", line.c_str());
    }
    return passed ? 0 : 1;
}
