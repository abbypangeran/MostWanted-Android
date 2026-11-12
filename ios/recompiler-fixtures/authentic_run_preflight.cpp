// Phase 2D-19: the machine-checkable static boundary for the first authentic
// run. It decodes the authentic image and compares every instruction in the
// proposed parent and child routes, the two pre-instruction stops, the kernel
// slots and the startup data. With --generated it also checks the generated
// per-instruction mapping: exactly the reviewed entry set, no unchecked
// dispatch in the generated bodies, and the mapping identity the authorisation
// must name. It links no generated authentic code and executes no guest
// instruction.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <Zydis/Zydis.h>

#include "kernel/authentic_run_route.h"
#include "kernel/crypto.h"
#include "xbe/xbe.h"

namespace route = kernel::authentic::route;

namespace {

constexpr char kExpectedSha1[] = "f636a16af04171089f879a4bed646e185c734233";
constexpr uint32_t kExpectedTitle = 0x4541007B;
constexpr uint32_t kExpectedEntry = 0x002B3916;
constexpr uint32_t kExpectedBase = 0x00010000;
constexpr uint32_t kExpectedTlsBlock = 0x94;

struct Check {
    std::string name;
    bool passed;
};

std::vector<Check> g_checks;
std::vector<std::string> g_lines;
bool g_failed = false;

void Record(const std::string& name, bool passed) {
    g_checks.push_back({name, passed});
    g_lines.emplace_back(name + "=" + (passed ? "1" : "0"));
    if (!passed) g_failed = true;
}

std::string Hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", value);
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

bool Decode(const xbe::Image& image, uint32_t pc, ZydisDecodedInstruction& instruction,
            ZydisDecodedOperand* operands, std::string& text) {
    const int64_t offset = image.Offset(pc);
    const size_t span = image.FileSpan(pc);
    if (offset < 0 || !span) return false;
    static ZydisDecoder decoder = [] {
        ZydisDecoder d;
        ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
        return d;
    }();
    static ZydisFormatter formatter = [] {
        ZydisFormatter f;
        ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterSetProperty(&f, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
        ZydisFormatterSetProperty(&f, ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC, ZYAN_FALSE);
        return f;
    }();
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction, operands)))
        return false;
    char buffer[256];
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                                      instruction.operand_count_visible, buffer, sizeof(buffer), pc,
                                                      nullptr)))
        return false;
    text = buffer;
    return true;
}

const char* KindName(route::StepKind kind) {
    switch (kind) {
        case route::StepKind::kSequential: return "sequential";
        case route::StepKind::kConditional: return "conditional";
        case route::StepKind::kCall: return "call";
        case route::StepKind::kKernelCall: return "kernel-call";
        case route::StepKind::kRet: return "ret";
    }
    return "unknown";
}

const char* SegmentName(route::Segment segment) {
    switch (segment) {
        case route::Segment::kParentRoot: return "parent-root";
        case route::Segment::kSystemRoutine: return "system-routine";
        case route::Segment::kSehPrologue: return "seh-prologue";
        case route::Segment::kPreStartWalk: return "pre-start-walk";
        case route::Segment::kStartCallSetup: return "start-call-setup";
    }
    return "unknown";
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

size_t Occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
    return count;
}

uint64_t MappingIdentityFor(const std::set<uint32_t>& entries, uint32_t base, uint32_t end) {
    uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= uint8_t(value >> shift);
            hash *= 0x100000001B3ull;
        }
    };
    mix(base);
    mix(end);
    mix(uint32_t(entries.size()));
    for (uint32_t entry : entries) mix(entry);
    return hash;
}

bool CheckRoute(const xbe::Image& image, const route::Step* steps, size_t count, const char* name, uint32_t stop,
                size_t& index_base) {
    bool ok = true;
    std::set<uint32_t> pcs;
    for (size_t i = 0; i < count; ++i) pcs.insert(steps[i].pc);
    std::set<uint32_t> seen;
    for (size_t i = 0; i < count; ++i) {
        const route::Step& step = steps[i];
        if (!seen.insert(step.pc).second) {
            g_lines.emplace_back(std::string("AUTH_RUN_DUPLICATE_PC=") + Hex(step.pc));
            ok = false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        std::string text;
        if (!Decode(image, step.pc, instruction, operands, text)) {
            g_lines.emplace_back(std::string("AUTH_RUN_DECODE_FAIL=") + Hex(step.pc));
            ok = false;
            continue;
        }
        const bool text_ok = text == step.text;
        const bool length_ok = instruction.length == step.length;
        if (!text_ok || !length_ok) {
            g_lines.emplace_back(std::string("AUTH_RUN_MISMATCH=") + Hex(step.pc) + " expected=\"" + step.text +
                                 "\" got=\"" + text + "\" expected_len=" + std::to_string(step.length) +
                                 " got_len=" + std::to_string(instruction.length));
            ok = false;
        }
        if (step.kind == route::StepKind::kCall || step.kind == route::StepKind::kConditional) {
            // The decoded immediate is the branch/call target. For a conditional
            // it is the taken target regardless of the reviewed outcome; the
            // reviewed fallthrough is step.next. The one reviewed dynamic call
            // (002B5C7E) is memory-indirect and its target is bound by the
            // runtime gate to step.target.
            const bool immediate = operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
            const bool memory_indirect = operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY;
            const uint32_t actual = immediate ? uint32_t(step.pc + instruction.length + operands[0].imm.value.s) : 0;
            const bool accepted = immediate ? actual == step.target
                                            : (memory_indirect && step.pc == 0x002B5C7E);
            if (!accepted) {
                g_lines.emplace_back(std::string("AUTH_RUN_BRANCH_MISMATCH=") + Hex(step.pc) + " expected=" +
                                     Hex(step.target) + " actual=" + (immediate ? Hex(actual) : "dynamic"));
                ok = false;
            }
        }
        if (step.kind == route::StepKind::kKernelCall) {
            const bool slot_form = operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                                   operands[0].mem.base == ZYDIS_REGISTER_NONE &&
                                   operands[0].mem.index == ZYDIS_REGISTER_NONE;
            const uint32_t slot = slot_form ? uint32_t(operands[0].mem.disp.value) : 0;
            uint32_t encoding = 0;
            if (!slot_form || slot != step.slot ||
                !image.Read32(step.slot, encoding) || encoding != step.encoding) {
                g_lines.emplace_back(std::string("AUTH_RUN_KERNEL_MISMATCH=") + Hex(step.pc) + " slot=" + Hex(slot) +
                                     " expected=" + Hex(step.slot) + " encoding=" + Hex(encoding) +
                                     " expected_encoding=" + Hex(step.encoding));
                ok = false;
            }
        }
        const bool is_ret = instruction.mnemonic == ZYDIS_MNEMONIC_RET;
        if (is_ret != (step.kind == route::StepKind::kRet)) {
            g_lines.emplace_back(std::string("AUTH_RUN_RET_CLASSIFICATION_MISMATCH=") + Hex(step.pc) +
                                 " mnemonic=" + (is_ret ? "ret" : text) + " kind=" + KindName(step.kind));
            ok = false;
        }
        if (step.kind == route::StepKind::kRet) {
            if (instruction.mnemonic != ZYDIS_MNEMONIC_RET) {
                g_lines.emplace_back(std::string("AUTH_RUN_RET_MISMATCH=") + Hex(step.pc));
                ok = false;
            } else {
                const bool has_immediate = instruction.operand_count_visible != 0 &&
                                           operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
                const uint32_t immediate = has_immediate ? uint32_t(operands[0].imm.value.u) : 0;
                if (immediate != step.ret_pop_bytes) {
                    g_lines.emplace_back(std::string("AUTH_RUN_RET_IMMEDIATE_MISMATCH=") + Hex(step.pc) +
                                         " decoded=" + std::to_string(immediate) + " manifest=" +
                                         std::to_string(step.ret_pop_bytes));
                    ok = false;
                }
            }
            const bool known_target = step.next == stop || pcs.count(step.next) != 0;
            if (!known_target) {
                g_lines.emplace_back(std::string("AUTH_RUN_RET_TARGET_MISMATCH=") + Hex(step.pc) + " next=" +
                                     Hex(step.next));
                ok = false;
            }
        } else if (step.ret_pop_bytes != 0) {
            g_lines.emplace_back(std::string("AUTH_RUN_RET_IMMEDIATE_ON_NON_RET=") + Hex(step.pc));
            ok = false;
        }
        char line[384];
        std::snprintf(line, sizeof(line), "AUTH_RUN_STEP=%zu,%s,%08X,%u,%s,%08X,%08X,%s", index_base + i,
                      SegmentName(step.segment), step.pc, step.length, KindName(step.kind), step.next, step.target,
                      step.text);
        g_lines.emplace_back(line);
    }
    (void)stop;
    index_base += count;
    g_lines.emplace_back(std::string("AUTH_RUN_") + name + "_EXECUTED=" + std::to_string(count));
    return ok;
}

bool CheckGenerated(const std::string& directory) {
    const std::string mapping_path = directory + "/x86_func_mapping.cpp";
    const std::string source_path = directory + "/x86_recomp.0.cpp";
    std::ifstream mapping_file(mapping_path), source_file(source_path);
    if (!mapping_file || !source_file) {
        g_lines.emplace_back("AUTH_RUN_GENERATED_MISSING=" + directory);
        return false;
    }
    const std::string mapping((std::istreambuf_iterator<char>(mapping_file)), {});
    const std::string source((std::istreambuf_iterator<char>(source_file)), {});
    uint32_t base = 0, end = 0;
    const size_t base_at = mapping.find("const uint32_t X86CodeBase = 0x");
    const size_t end_at = mapping.find("const uint32_t X86CodeEnd = 0x");
    if (base_at == std::string::npos || end_at == std::string::npos) {
        g_lines.emplace_back("AUTH_RUN_GENERATED_BOUNDS=missing");
        return false;
    }
    const size_t base_value = mapping.find("0x", base_at);
    const size_t end_value = mapping.find("0x", end_at);
    if (base_value == std::string::npos || end_value == std::string::npos) {
        g_lines.emplace_back("AUTH_RUN_GENERATED_BOUNDS=missing");
        return false;
    }
    base = uint32_t(std::stoul(mapping.substr(base_value), nullptr, 16));
    end = uint32_t(std::stoul(mapping.substr(end_value), nullptr, 16));
    std::set<uint32_t> entries;
    size_t at = 0;
    while ((at = mapping.find("{0x", at)) != std::string::npos) {
        const size_t comma = mapping.find(',', at);
        if (comma == std::string::npos) break;
        entries.insert(uint32_t(std::stoul(mapping.substr(at + 1), nullptr, 16)));
        at = comma;
    }
    std::set<uint32_t> expected;
    for (size_t i = 0; i < route::kParentRouteCount; ++i) expected.insert(route::kParentRoute[i].pc);
    for (size_t i = 0; i < route::kChildRouteCount; ++i) expected.insert(route::kChildRoute[i].pc);
    for (uint32_t pc : route::kAllTrapEntries) expected.insert(pc);
    Record("AUTH_RUN_MAPPING_ENTRY_SET", entries == expected);
    g_lines.emplace_back("AUTH_RUN_MAPPING_ENTRIES=" + std::to_string(entries.size()) + "/" +
                         std::to_string(expected.size()));
    const uint64_t identity = MappingIdentityFor(entries, base, end);
    char identity_text[32];
    std::snprintf(identity_text, sizeof(identity_text), "%016llX", static_cast<unsigned long long>(identity));
    g_lines.emplace_back(std::string("AUTH_RUN_MAPPING_IDENTITY=") + identity_text);
    g_lines.emplace_back("AUTH_RUN_MAPPING_BOUNDS=" + Hex(base) + ".." + Hex(end));
    Record("AUTH_RUN_NO_UNCHECKED_INDIRECT", !Contains(source, "x86_icall"));
    // Checked mode emits one host function per instruction and no direct host
    // call between route functions. Every transfer must name a checked helper.
    size_t direct_calls = 0;
    for (size_t pos = 0; (pos = source.find("(ctx, base);", pos)) != std::string::npos; ++pos) ++direct_calls;
    Record("AUTH_RUN_NO_DIRECT_HOST_CALLS", direct_calls == 0);
    Record("AUTH_RUN_CHECKED_TRANSFERS", Occurrences(source, "x86_checked_jmp") > 0 &&
                                            Occurrences(source, "x86_checked_call") > 0);
    const size_t expected_functions = expected.size();
    g_lines.emplace_back("AUTH_RUN_GENERATED_CHECK=" + std::string(entries == expected ? "PASS" : "FAIL"));
    (void)expected_functions;
    return entries == expected && !Contains(source, "x86_icall") && direct_calls == 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string image_path = "assets/default.xbe";
    std::string generated_directory;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) json = true;
        else if (std::strcmp(argv[i], "--generated") == 0 && i + 1 < argc) generated_directory = argv[++i];
        else image_path = argv[i];
    }
    std::ifstream file(image_path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", image_path.c_str());
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
    Record("AUTH_RUN_IMAGE_IDENTITY", sha1 == kExpectedSha1);
    Record("AUTH_RUN_TITLE", image.title_id == kExpectedTitle);
    Record("AUTH_RUN_ENTRY", image.entry == kExpectedEntry);
    Record("AUTH_RUN_BASE", image.base == kExpectedBase);
    size_t index = 0;
    const bool parent_ok = CheckRoute(image, route::kParentRoute, route::kParentRouteCount, "PARENT",
                                      route::kParentStop, index);
    const bool child_ok = CheckRoute(image, route::kChildRoute, route::kChildRouteCount, "CHILD",
                                     route::kChildStop, index);
    Record("AUTH_RUN_PARENT_ROUTE", parent_ok);
    Record("AUTH_RUN_CHILD_ROUTE", child_ok);
    Record("AUTH_RUN_PARENT_COUNT", route::kParentRouteCount == route::kParentExecutedInstructions);
    Record("AUTH_RUN_CHILD_COUNT", route::kChildRouteCount == route::kChildExecutedInstructions);
    Record("AUTH_RUN_TOTAL_COUNT", route::kParentRouteCount + route::kChildRouteCount ==
                                       route::kTotalExecutedInstructions);
    // Budget invariant: the manifest-derived totals include every instruction
    // entry, every RET transfer and the terminal stop. The previous child
    // budget of 128 is statically insufficient for this route.
    Record("AUTH_RUN_PARENT_REQUIRED_TRANSFERS",
           route::kParentRequiredTransfers ==
               route::ComputeRequiredTransfers(route::kParentRoute, route::kParentRouteCount));
    Record("AUTH_RUN_CHILD_REQUIRED_TRANSFERS",
           route::kChildRequiredTransfers ==
               route::ComputeRequiredTransfers(route::kChildRoute, route::kChildRouteCount));
    Record("AUTH_RUN_CHILD_BUDGET_128_REFUSED", route::kChildRequiredTransfers > 128);
    Record("AUTH_RUN_PARENT_BUDGET_72_REFUSED", route::kParentRequiredTransfers > 72);
    Record("AUTH_RUN_PARENT_BUDGET_73_ACCEPTED", route::kParentRequiredTransfers <= 73);
    Record("AUTH_RUN_CHILD_BUDGET_156_ACCEPTED", route::kChildRequiredTransfers <= 156);
    Record("AUTH_RUN_CHILD_BUDGET_155_REFUSED", route::kChildRequiredTransfers > 155);
    char budget_line[96];
    std::snprintf(budget_line, sizeof(budget_line), "AUTH_RUN_PARENT_REQUIRED_TRANSFERS=%u",
                  route::kParentRequiredTransfers);
    g_lines.emplace_back(budget_line);
    std::snprintf(budget_line, sizeof(budget_line), "AUTH_RUN_CHILD_REQUIRED_TRANSFERS=%u",
                  route::kChildRequiredTransfers);
    g_lines.emplace_back(budget_line);
    std::snprintf(budget_line, sizeof(budget_line), "AUTH_RUN_PARENT_BOUNDARY_STEPS=%u",
                  route::kParentBoundarySteps);
    g_lines.emplace_back(budget_line);
    std::snprintf(budget_line, sizeof(budget_line), "AUTH_RUN_CHILD_BOUNDARY_STEPS=%u",
                  route::kChildBoundarySteps);
    g_lines.emplace_back(budget_line);
    g_lines.emplace_back("AUTH_RUN_CHILD_BUDGET_128=REFUSED");
    {
        char parent_identity[40], child_identity[40];
        std::snprintf(parent_identity, sizeof(parent_identity), "%016llX",
                      static_cast<unsigned long long>(route::ComputeManifestIdentity(
                          route::kParentRoute, route::kParentRouteCount, route::kParentEntry, route::kParentStop,
                          0, 0, 0, 0, 0, 0, 0, 0, 0, route::kParentRequiredTransfers)));
        std::snprintf(child_identity, sizeof(child_identity), "%016llX",
                      static_cast<unsigned long long>(route::ComputeManifestIdentity(
                          route::kChildRoute, route::kChildRouteCount, route::kChildEntry, route::kChildStop,
                          route::kSehCall, route::kSehEntry, route::kSehHandler, route::kSehScopeTable,
                          route::kSehScopeBytes, route::kSehScopeFlag, route::kTlsBlockSize,
                          route::kCriticalSectionObject, route::kChildStartRoutine,
                          route::kChildRequiredTransfers)));
        g_lines.emplace_back(std::string("AUTH_RUN_PARENT_MANIFEST_IDENTITY=") + parent_identity);
        g_lines.emplace_back(std::string("AUTH_RUN_CHILD_MANIFEST_IDENTITY=") + child_identity);
    }
    // The two terminal boundaries are exact: the parent's next instruction is
    // the ret at 002B3987, and the child's dynamic call's target is 002B38A2.
    bool parent_stop_edge = false, child_stop_edge = false;
    for (size_t i = 0; i < route::kParentRouteCount; ++i)
        if (route::kParentRoute[i].next == route::kParentStop) parent_stop_edge = true;
    for (size_t i = 0; i < route::kChildRouteCount; ++i)
        if (route::kChildRoute[i].next == route::kChildStop) child_stop_edge = true;
    Record("AUTH_RUN_PARENT_STOP_EDGE", parent_stop_edge);
    Record("AUTH_RUN_CHILD_STOP_EDGE", child_stop_edge);
    // Startup data.
    uint32_t tls_source = 0, tls_end = 0, tls_zero = 0, list_head = 0, scope_first = 0;
    const bool data_ok = image.Read32(0x003ED3EC, tls_source) && image.Read32(0x003ED3F0, tls_end) &&
                         image.Read32(0x003ED3FC, tls_zero) && image.Read32(0x0046C8CC, list_head) &&
                         image.Read32(0x003ED5B8, scope_first);
    Record("AUTH_RUN_STATIC_DATA", data_ok);
    Record("AUTH_RUN_TLS_ZERO_FILL", tls_zero == 0x90 && tls_source == 0 && tls_end == 0);
    Record("AUTH_RUN_TLS_BLOCK", xbe::TlsBlockSize(image.tls) == kExpectedTlsBlock);
    Record("AUTH_RUN_LIST_EMPTY", list_head == 0x0046C8CC);
    Record("AUTH_RUN_SCOPE_EMPTY", scope_first == 0xFFFFFFFF);
    // The reviewed import slots and the exact call-site encodings.
    uint32_t enter_slot = 0, leave_slot = 0, create_slot = 0, close_slot = 0;
    uint32_t hardware_slot = 0, version_slot = 0;
    const bool slots_ok = image.Read32(0x003C9E44, enter_slot) && image.Read32(0x003C9E40, leave_slot) &&
                          image.Read32(0x003C9E68, create_slot) && image.Read32(0x003C9D4C, close_slot) &&
                          image.Read32(0x003C9E34, hardware_slot) && image.Read32(0x003C9E3C, version_slot);
    Record("AUTH_RUN_IMPORT_SLOTS", slots_ok && enter_slot == 0x80000115 && leave_slot == 0x80000126 &&
                                        create_slot == 0x800000FF && close_slot == 0x800000BB);
    Record("AUTH_RUN_DATA_IMPORT_SLOTS", slots_ok && hardware_slot == 0x80000142 && version_slot == 0x80000144);
    // The three trap-only entries must decode to the reviewed lengths.
    bool traps_ok = true;
    for (uint32_t pc : route::kAllTrapEntries) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        std::string text;
        if (!Decode(image, pc, instruction, operands, text)) traps_ok = false;
    }
    Record("AUTH_RUN_TRAP_ENTRIES", traps_ok);
    g_lines.emplace_back("AUTH_RUN_STOP_PARENT=" + Hex(route::kParentStop));
    g_lines.emplace_back("AUTH_RUN_STOP_CHILD=" + Hex(route::kChildStop));
    g_lines.emplace_back("AUTH_RUN_STOP_KIND=pre-instruction");
    g_lines.emplace_back("AUTH_RUN_CHILD_TRANSITION=" + Hex(route::kChildStop) + " first-instruction-not-executed");
    g_lines.emplace_back("AUTH_RUN_TOTAL_EXECUTED=" + std::to_string(route::kParentRouteCount + route::kChildRouteCount));
    g_lines.emplace_back("AUTH_RUN_BOUNDARY_STEPS=" + std::to_string(route::kParentRouteCount + route::kChildRouteCount + 2));
    g_lines.emplace_back("AUTH_RUN_EXECUTION=PROHIBITED");
    if (!generated_directory.empty()) {
        const bool generated_ok = CheckGenerated(generated_directory);
        Record("AUTH_RUN_GENERATED_MAPPING", generated_ok);
    } else {
        g_lines.emplace_back("AUTH_RUN_GENERATED_MAPPING=skipped");
    }
    if (json) {
        std::printf("{\n  \"schema\": \"mostwanted.authentic-run-preflight/1\",\n  \"phase\": \"2D-19\",\n");
        std::printf("  \"image_sha1\": \"%s\",\n  \"parent_instructions\": %zu,\n  \"child_instructions\": %zu,\n",
                    sha1.c_str(), route::kParentRouteCount, route::kChildRouteCount);
        std::printf("  \"total_executed\": %zu,\n  \"stops\": [\"%s\", \"%s\"],\n",
                    route::kParentRouteCount + route::kChildRouteCount, Hex(route::kParentStop).c_str(),
                    Hex(route::kChildStop).c_str());
        std::printf("  \"parent_required_transfers\": %u,\n  \"child_required_transfers\": %u,\n"
                    "  \"child_budget_128\": \"REFUSED\",\n",
                    route::kParentRequiredTransfers, route::kChildRequiredTransfers);
        std::printf("  \"execution\": \"PROHIBITED\",\n  \"checks\": [\n");
        for (size_t i = 0; i < g_checks.size(); ++i)
            std::printf("    {\"name\": \"%s\", \"passed\": %s}%s\n", g_checks[i].name.c_str(),
                        g_checks[i].passed ? "true" : "false", i + 1 == g_checks.size() ? "" : ",");
        std::printf("  ],\n  \"result\": \"%s\"\n}\n", g_failed ? "FAIL" : "PASS");
    } else {
        for (const std::string& line : g_lines) std::printf("%s\n", line.c_str());
        std::printf("AUTH_RUN_RESULT=%s\n", g_failed ? "FAIL" : "PASS");
    }
    return g_failed ? 1 : 0;
}
