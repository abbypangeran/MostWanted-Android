// Phase 2C-16 real-image preparation and preflight, included by
// execution_probe.cpp when EXECUTION_PROBE_MODE_REAL is defined.  It uses the
// same probe state, hooks and observers as the synthetic harness; only the
// image preparation, reviewed spans and preflight checks differ.

namespace {

constexpr uint32_t kRealImageBase = 0x00010000;
constexpr uint32_t kRealHeaderSize = 0x1000;
constexpr uint32_t kRealImportPage = 0x003C9000;
constexpr uint32_t kRealRdataPage = 0x003ED000;
constexpr uint32_t kRealCounterPage = 0x004ED000;
constexpr uint32_t kRealTargetPage = 0x004DF000;
constexpr uint32_t kRealStackRequest = 0x4000;
constexpr uint32_t kRealStackCommitted = 0x10000;  // AllocStack rounds the request to 64 KB
constexpr uint32_t kRealStackHeadroom = 0x100;
constexpr uint32_t kImportArgumentBytes = 10 * sizeof(uint32_t);

// the reviewed route's stack slots in the helper frame; verified zero before
// the run because the stack is freshly committed.
constexpr uint32_t kImportStackSlots = 4;  // +0x38 a5, +0x3C a6, +0x40 a7, +0x44 a4

// the two prepared image import slots are patched to their reviewed kernel
// thunks and restored before teardown.
struct RealImportPatch {
    uint32_t slot;
    uint32_t original;
    uint32_t target;
    uint32_t ordinal;
    uint32_t encoding;
    bool active;
};
RealImportPatch g_real_imports[] = {
    {kImportSlot, 0, 0, kImportOrdinal, kImportEncoding, false},
    {kCloseImportSlot, 0, 0, kCloseOrdinal, kCloseImportEncoding, false},
};

uint32_t g_real_worker_baseline = 0;

// test-only injection: leaves a wrong value so the readback below fails
uint32_t g_import_restore_fail_ordinal = 0;

bool RestoreImportSlots() {
    bool active = false;
    for (const RealImportPatch& patch : g_real_imports) active |= patch.active;
    if (!active) return true;
    bool wrote = ProtectRange(kRealImportPage, mem::kPage, mem::kRead | mem::kWrite);
    for (RealImportPatch& patch : g_real_imports) {
        if (!patch.active) continue;
        if (wrote) wrote = mem::Write(patch.slot, &patch.original, sizeof(patch.original)) == mem::AccessStatus::kOk;
        if (wrote && g_import_restore_fail_ordinal == patch.ordinal) {
            const uint32_t wrong = patch.original ^ 0xFFFFFFFFu;
            mem::Write(patch.slot, &wrong, sizeof(wrong));
        }
    }
    const bool read_only = ProtectRange(kRealImportPage, mem::kPage, mem::kRead);
    const bool protected_read_only = mem::CheckAccess(kRealImportPage, 1, mem::kRead) == mem::AccessStatus::kOk &&
                                     mem::CheckAccess(kRealImportPage, 1, mem::kWrite) != mem::AccessStatus::kOk;
    bool ok = wrote && read_only && protected_read_only;
    for (RealImportPatch& patch : g_real_imports) {
        if (!patch.active) continue;
        patch.active = false;
        uint32_t restored = 0;
        const bool readback =
            mem::Read(patch.slot, &restored, sizeof(restored)) == mem::AccessStatus::kOk && restored == patch.original;
        const bool slot_ok = readback && protected_read_only;
        std::printf("IMPORT slot=%08X restored=%08X ok=%d\n", patch.slot, restored, int(slot_ok));
        ok = ok && slot_ok;
    }
    std::printf("IMPORT restored=%d read_only=%d\n", int(ok), int(protected_read_only));
    return ok;
}

struct RealFacts {
    uint32_t base = 0, image_size = 0, header_size = 0, sections = 0;
    uint32_t g10108 = 0, g10118 = 0, g10130 = 0, g10184 = 0;
    uint32_t g3ec = 0, g3f0 = 0, g3f4 = 0, g3fc = 0, g4edc74 = 0;
    uint32_t p = 0, p_value = 0;
    uint32_t entry_ea = 0;
    bool entry_taken = false;
    bool p_write_required = false;
    uint32_t idiv_dividend = 0, idiv_quotient = 0, idiv_remainder = 0;
    uint32_t counter_after = 0;
    uint32_t q = 0, q_before = 0, q_after = 0;
    uint32_t stack_base = 0, stack_top = 0, esp = 0;
    uint32_t import_authentic = 0, import_target = 0;
    uint32_t import_esp = 0, import_arg0 = 0, import_return = 0;
    std::array<uint32_t, 10> import_args{};
    uint32_t close_import_authentic = 0, close_import_target = 0;
    uint32_t close_esp = 0, close_arg_slot = 0, close_return = 0;
    uint32_t baseline_system = 0, baseline_contiguous = 0, baseline_virtual = 0;
    std::vector<Span> spans;
    std::vector<uint32_t> snapshot_pages;
};

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
    return count;
}

bool ValidateRealIdentity(const std::string& path, xbe::Image& image, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path.c_str(), bytes)) {
        error = "real XBE is missing: " + path;
        return false;
    }
    if (!image.Parse(std::move(bytes), error)) {
        error = "real XBE parse failed: " + error;
        return false;
    }
    if (Sha1Hex(image.file) != kRealSha1) {
        error = "real XBE SHA-1 mismatch";
        return false;
    }
    if (image.title_id != kRealTitle) {
        error = "real XBE title ID mismatch";
        return false;
    }
    if (image.entry != kRealEntry) {
        error = "real XBE entry mismatch";
        return false;
    }
    if (image.base != kRealImageBase) {
        error = "real XBE base mismatch";
        return false;
    }
    return true;
}

bool ValidateRealRouteText(const xbe::Image& image, std::string& error) {
    static_assert(kExpectedRouteTextCount == kRouteCount);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC, ZYAN_FALSE);

    uint64_t route_hash = 0xcbf29ce484222325ULL;
    size_t route_bytes = 0;
    for (size_t i = 0; i < kRouteCount; ++i) {
        const RouteStep& step = kRoute[i];
        const int64_t offset = image.Offset(step.pc);
        const size_t span = image.FileSpan(step.pc);
        if (offset < 0 || span == 0) {
            error = "route PC is not file-backed";
            return false;
        }
        if (uint64_t(offset) + step.length > image.file.size()) {
            error = "route instruction bytes are truncated";
            return false;
        }
        for (uint32_t j = 0; j < step.length; ++j) {
            route_hash ^= image.file[size_t(offset) + j];
            route_hash *= 0x100000001b3ULL;
        }
        route_bytes += step.length;
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction, operands))) {
            error = "route instruction did not decode";
            return false;
        }
        char text[256];
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, text,
                                        sizeof(text), step.pc, nullptr);
        if (std::strcmp(text, kExpectedRouteText[i]) != 0) {
            error = std::string("route text mismatch at ") + std::to_string(step.pc);
            return false;
        }
        if (instruction.length != step.length) {
            error = std::string("route length mismatch at ") + std::to_string(step.pc);
            return false;
        }
        if (step.kind == StepKind::kConditional) {
            if (operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                uint32_t(step.pc + instruction.length + operands[0].imm.value.s) != step.alternate) {
                error = std::string("route branch target mismatch at ") + std::to_string(step.pc);
                return false;
            }
        } else if (step.kind == StepKind::kCall) {
            if (operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                uint32_t(step.pc + instruction.length + operands[0].imm.value.s) != step.next) {
                error = std::string("route call target mismatch at ") + std::to_string(step.pc);
                return false;
            }
        } else if (step.kind == StepKind::kKernelCall) {
            const ImportStep* import = FindImportStep(step.pc);
            if (!import || operands[0].type != ZYDIS_OPERAND_TYPE_MEMORY ||
                operands[0].mem.base != ZYDIS_REGISTER_NONE ||
                operands[0].mem.index != ZYDIS_REGISTER_NONE || operands[0].size != 32 ||
                operands[0].mem.disp.value != int64_t(import->slot) || step.next != import->return_pc) {
                error = "route import call mismatch";
                return false;
            }
        }
    }

    if (route_bytes != kExpectedRouteBytes || route_hash != kExpectedRouteHash) {
        error = "route byte hash mismatch";
        return false;
    }

    const RouteStep* post = FindStep(kPostImportBranch);
    if (!post || post->kind != StepKind::kConditional || post->next != kDebugPath ||
        post->alternate != kHandleRead) {
        error = "post-import branch coverage mismatch";
        return false;
    }
    const int64_t debug_offset = image.Offset(kDebugPath);
    const size_t debug_span = image.FileSpan(kDebugPath);
    ZydisDecodedInstruction debug_instruction;
    ZydisDecodedOperand debug_operands[ZYDIS_MAX_OPERAND_COUNT];
    char debug_text[256] = {};
    if (debug_offset < 0 ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + debug_offset, debug_span,
                                             &debug_instruction, debug_operands)) ||
        debug_instruction.length != kDebugPathLength) {
        error = "debug arm does not decode as reviewed";
        return false;
    }
    ZydisFormatterFormatInstruction(&formatter, &debug_instruction, debug_operands,
                                    debug_instruction.operand_count_visible, debug_text, sizeof(debug_text),
                                    kDebugPath, nullptr);
    if (std::strcmp(debug_text, kDebugPathText) != 0) {
        error = "debug arm text mismatch";
        return false;
    }

    // the helper's RET contract: the instruction, its immediate and its return
    // target, with a pre-instruction stop at the destination.
    const int64_t ret_offset = image.Offset(kRetInstruction);
    const size_t ret_span = image.FileSpan(kRetInstruction);
    ZydisDecodedInstruction ret_instruction;
    ZydisDecodedOperand ret_operands[ZYDIS_MAX_OPERAND_COUNT];
    if (ret_offset < 0 ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + ret_offset, ret_span, &ret_instruction,
                                             ret_operands)) ||
        ret_instruction.mnemonic != ZYDIS_MNEMONIC_RET || ret_instruction.operand_count_visible != 1 ||
        ret_operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || ret_operands[0].imm.value.u != kRetPopBytes ||
        ret_instruction.length != 3) {
        error = "helper RET contract mismatch";
        return false;
    }
    // the six caller pushes at 0x002B395E..0x002B3967 are the 0x18 cleanup
    for (uint32_t pc : {0x002B395EU, 0x002B395FU, 0x002B3960U, 0x002B3961U, 0x002B3966U, 0x002B3967U}) {
        if (!FindStep(pc)) {
            error = "a reviewed caller argument push is missing";
            return false;
        }
    }
    const RouteStep* call = FindStep(kCallSite);
    if (!call || kCallSite + call->length != kRetTarget) {
        error = "the reviewed RET target is not the direct call's continuation";
        return false;
    }
    const RouteStep* ret = FindStep(kRetInstruction);
    const RouteStep* stop = FindStep(kStopBefore);
    if (!ret || ret->length != 3 || ret->next != kRetTarget || !stop || stop->kind != StepKind::kStop ||
        stop->pc != kStopBefore) {
        error = "the reviewed RET or caller stop boundary is missing or misplaced";
        return false;
    }
    const RouteStep* caller = FindStep(kCallerBranch);
    const RouteStep* off_path = FindStep(kCallerOffPath);
    const RouteStep* continuation = FindStep(kCallerContinuation);
    const RouteStep* close = FindStep(kCloseCall);
    const RouteStep* close_entry = FindStep(kCloseTarget);
    const RouteStep* close_import = FindStep(kCloseImportCall);
    const RouteStep* close_branch = FindStep(kCloseBranch);
    const RouteStep* close_ret = FindStep(kCloseRet);
    const RouteStep* close_error = FindStep(kCloseErrorPath);
    if (!caller || caller->kind != StepKind::kConditional || caller->next != kCallerOffPath ||
        caller->alternate != kCallerContinuation || !off_path || off_path->kind != StepKind::kForbidden ||
        !continuation || continuation->next != kCloseCall || !close || close->kind != StepKind::kCall ||
        close->next != kCloseTarget || !close_entry || close_entry->kind != StepKind::kSequential ||
        close_entry->next != kCloseImportCall || !close_import || close_import->kind != StepKind::kKernelCall ||
        close_import->next != kCloseImportReturn || !close_branch || close_branch->kind != StepKind::kConditional ||
        close_branch->next != kCloseSuccess || close_branch->alternate != kCloseErrorPath || !close_ret ||
        close_ret->length != 3 || close_ret->next != kCloseContinue || !close_error ||
        close_error->kind != StepKind::kForbidden) {
        error = "the caller handle check or NtClose wrapper route is missing or misplaced";
        return false;
    }
    auto check_direct_call = [&](uint32_t pc, uint32_t target, const char* what) {
        const int64_t offset = image.Offset(pc);
        const size_t span = image.FileSpan(pc);
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (offset < 0 ||
            !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction,
                                                 operands)) ||
            instruction.mnemonic != ZYDIS_MNEMONIC_CALL || instruction.length != 5 ||
            operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
            uint32_t(pc + instruction.length + operands[0].imm.value.s) != target) {
            error = std::string("the ") + what + " is not the reviewed direct call";
            return false;
        }
        return true;
    };
    if (!check_direct_call(kCloseCall, kCloseTarget, "close wrapper call") ||
        !check_direct_call(kOffPathCall, kOffPathTarget, "zero-handle arm call") ||
        !check_direct_call(kCloseErrorCall, kCloseErrorTarget, "wrapper error-path call"))
        return false;
    // the wrapper's NtClose import: the slot's encoding, not its execution,
    // pins the ordinal; the wrapper's ret 0x04 must be its only RET form.
    const int64_t close_offset = image.Offset(kCloseImportCall);
    const size_t close_span = image.FileSpan(kCloseImportCall);
    ZydisDecodedInstruction close_instruction;
    ZydisDecodedOperand close_operands[ZYDIS_MAX_OPERAND_COUNT];
    if (close_offset < 0 ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + close_offset, close_span,
                                             &close_instruction, close_operands)) ||
        close_instruction.mnemonic != ZYDIS_MNEMONIC_CALL || close_instruction.length != 6 ||
        close_operands[0].type != ZYDIS_OPERAND_TYPE_MEMORY ||
        close_operands[0].mem.base != ZYDIS_REGISTER_NONE ||
        close_operands[0].mem.index != ZYDIS_REGISTER_NONE ||
        close_operands[0].mem.disp.value != int64_t(kCloseImportSlot)) {
        error = "the close wrapper does not call the reviewed import slot";
        return false;
    }
    const int64_t close_ret_offset = image.Offset(kCloseRet);
    const size_t close_ret_span = image.FileSpan(kCloseRet);
    ZydisDecodedInstruction close_ret_instruction;
    ZydisDecodedOperand close_ret_operands[ZYDIS_MAX_OPERAND_COUNT];
    if (close_ret_offset < 0 ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + close_ret_offset, close_ret_span,
                                             &close_ret_instruction, close_ret_operands)) ||
        close_ret_instruction.mnemonic != ZYDIS_MNEMONIC_RET || close_ret_instruction.length != 3 ||
        close_ret_instruction.operand_count_visible != 1 ||
        close_ret_operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || close_ret_operands[0].imm.value.u != 4) {
        error = "the close wrapper RET contract is not ret 0x04";
        return false;
    }
    uint32_t close_import_value = 0;
    if (!image.Read32(kCloseImportSlot, close_import_value) || close_import_value != kCloseImportEncoding) {
        error = "the close wrapper's import slot is not NtClose ordinal 187";
        return false;
    }
    uint32_t import = 0;
    if (!image.Read32(kImportSlot, import) || import != kImportEncoding) {
        error = "import slot is not the ordinal-255 encoding";
        return false;
    }
    return true;
}

bool ValidateHookCoverageText(const std::string& header, const std::string& source, std::string& error) {
    for (const RouteStep& step : kRoute) {
        char name[64];
        std::snprintf(name, sizeof(name), "ProbeStep_0x%08X(", step.pc);
        const size_t in_header = CountOccurrences(header, name);
        const size_t in_source = CountOccurrences(source, name);
        if (in_header != 1 || in_source != 1) {
            error = std::string("hook coverage mismatch at ") + name;
            return false;
        }
    }
    const size_t step_total = CountOccurrences(source, "ProbeStep_0x");
    const size_t stop_total = CountOccurrences(source, "ProbeStop_0x");
    const size_t header_steps = CountOccurrences(header, "ProbeStep_0x");
    const size_t header_stops = CountOccurrences(header, "ProbeStop_0x");
    if (stop_total != 0 || header_stops != 0) {
        error = "generated source contains a post-instruction stop hook";
        return false;
    }
    if (step_total != kRouteCount || header_steps != kRouteCount) {
        error = "generated hook counts are not exactly the reviewed set";
        return false;
    }
    const std::string import_255 = "x86_checked_indirect_call(ctx, base, 0x3C9E68u, 0x2B5CE6u, 0x2B5CE0u)";
    const std::string import_187 = "x86_checked_indirect_call(ctx, base, 0x3C9D4Cu, 0x2B2763u, 0x2B275Du)";
    if (source.find(import_255) == std::string::npos || source.find(import_187) == std::string::npos ||
        CountOccurrences(source, "x86_checked_indirect_call") != 2 ||
        CountOccurrences(source, "0x3C9E68u") != 1 || CountOccurrences(source, "0x3C9D4Cu") != 1) {
        error = "generated source does not contain exactly the two reviewed import calls";
        return false;
    }
    const std::string ret_18 = "x86_checked_ret(ctx, 0x2B5D12u, 24)";
    const std::string ret_04 = "x86_checked_ret(ctx, 0x2B2774u, 4)";
    const std::string ret_caller = "x86_checked_ret(ctx, 0x2B3987u, 0)";
    if (source.find(ret_18) == std::string::npos || source.find(ret_04) == std::string::npos ||
        source.find(ret_caller) == std::string::npos || CountOccurrences(source, "x86_checked_ret") != 3 ||
        source.find("// 002B5D12 ret 0x18") == std::string::npos ||
        source.find("// 002B2774 ret 0x04") == std::string::npos) {
        error = "generated source does not contain exactly the three reviewed RETs";
        return false;
    }
    if (source.find("// 002B5D15") != std::string::npos || source.find("// 002B2777") != std::string::npos ||
        source.find("// 002B3988") != std::string::npos) {
        error = "generated source contains code after a reviewed boundary";
        return false;
    }
    struct HookOrder {
        const char* hook;
        const char* instruction;
        const char* what;
    };
    const HookOrder orders[] = {
        {"ProbeStep_0x002B3987(ctx", "// 002B3987 ret", "caller RET stop"},
        {"ProbeStep_0x002B276C(ctx", "// 002B276C push eax", "wrapper error arm"},
        {"ProbeStep_0x002B3826(ctx", "// 002B3826 push [esp+0x04]", "error stub"},
        {"ProbeStep_0x002B2759(ctx", "// 002B2759 push [esp+0x04]", "NtClose wrapper"},
    };
    for (const HookOrder& order : orders) {
        const size_t hook = source.find(order.hook);
        const size_t instruction = source.find(order.instruction);
        if (hook == std::string::npos || instruction == std::string::npos || hook > instruction) {
            error = std::string("the ") + order.what + " hook is missing or misplaced";
            return false;
        }
    }
    for (const char* instruction : {"// 002B396F mov edi, eax", "// 002B3973 jnz 0x002B397F",
                                    "// 002B397F push edi", "// 002B3980 call 0x002B2759",
                                    "// 002B3985 pop edi", "// 002B3986 pop esi", "// 002B2759 push [esp+0x04]",
                                    "// 002B275D call [0x003C9D4C]", "// 002B2765 jl 0x002B276C",
                                    "// 002B2774 ret 0x04"}) {
        if (source.find(instruction) == std::string::npos) {
            error = std::string("the reviewed instruction text is missing: ") + instruction;
            return false;
        }
    }
    for (const char* forbidden : {"x86_icall", "x86_kernel", "x86_resume", "ProbeStop_"}) {
        if (source.find(forbidden) != std::string::npos) {
            error = std::string("generated source contains ") + forbidden;
            return false;
        }
    }
    if (source.find("X86_FUNC(__imp__sub_002B5CEC)") == std::string::npos ||
        source.find("ProbeStep_0x002B5CEC(ctx") == std::string::npos) {
        error = "debug boundary function is missing";
        return false;
    }
    if (source.find("0x3C9DC0") != std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x2B37F8") != std::string::npos) {
        error = "the wrapper error path's callees were compiled";
        return false;
    }
    if (CountOccurrences(source, "x86_checked_call(ctx, base, 0x2B5CA7u, 0x2B396Fu, 0x2B396Au)") != 1 ||
        CountOccurrences(source, "x86_checked_call(ctx, base, 0x2B55EEu, 0x2B397Fu, 0x2B397Au)") != 1 ||
        CountOccurrences(source, "x86_checked_call(ctx, base, 0x2B2759u, 0x2B3985u, 0x2B3980u)") != 1 ||
        CountOccurrences(source, "x86_checked_call(ctx, base, 0x2B3826u, 0x2B2772u, 0x2B276Du)") != 1 ||
        CountOccurrences(source, "x86_checked_call") != 4) {
        error = "generated source does not contain exactly the reviewed direct calls";
        return false;
    }
    const size_t functions = CountOccurrences(source, "X86_FUNC(__imp__sub_");
    if (functions != 6) {
        error = "generated source has an unexpected function set";
        return false;
    }
    for (const char* function : {"X86_FUNC(__imp__sub_002B3916)", "X86_FUNC(__imp__sub_002B5CA7)",
                                   "X86_FUNC(__imp__sub_002B2759)", "X86_FUNC(__imp__sub_002B3826)"}) {
        if (source.find(function) == std::string::npos) {
            error = "generated source is missing a reviewed function";
            return false;
        }
    }
    return true;
}

bool ValidateHookCoverage(const std::string& header_path, const std::string& source_path, std::string& error) {
    std::vector<uint8_t> header_bytes, source_bytes;
    if (!ReadFile(header_path.c_str(), header_bytes)) {
        error = "generated hook header is missing";
        return false;
    }
    if (!ReadFile(source_path.c_str(), source_bytes)) {
        error = "generated hook source is missing";
        return false;
    }
    const std::string header(header_bytes.begin(), header_bytes.end());
    const std::string source(source_bytes.begin(), source_bytes.end());
    return ValidateHookCoverageText(header, source, error);
}

bool ValidateReviewedSpans(const std::vector<Span>& spans, std::string& error) {
    for (size_t i = 0; i < spans.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (spans[i].begin < spans[j].end && spans[j].begin < spans[i].end) {
                error = "reviewed spans overlap";
                return false;
            }
    for (const Span& span : spans) {
        if (span.begin >= span.end) {
            error = "reviewed span is empty or inverted";
            return false;
        }
        if (InImportPage(span.begin, span.end)) {
            error = "reviewed span overlaps the ordinal-255 import page";
            return false;
        }
        if (mem::CheckAccess(span.begin, span.end - span.begin, span.write ? mem::kReadWrite : mem::kRead) !=
            mem::AccessStatus::kOk) {
            error = "reviewed span does not have its required permission";
            return false;
        }
    }
    return true;
}

bool AddSpan(std::vector<Span>& spans, uint32_t begin, uint32_t end, bool write) {
    if (begin >= end) return false;
    for (size_t i = 0; i < spans.size();) {
        Span& span = spans[i];
        if (end <= span.begin || begin >= span.end) {
            ++i;
            continue;
        }
        begin = std::min(begin, span.begin);
        end = std::max(end, span.end);
        write = write || span.write;
        spans.erase(spans.begin() + long(i));
    }
    spans.push_back({begin, end, write});
    return true;
}

void AddSnapshotPage(std::vector<uint32_t>& pages, uint32_t page) {
    page &= ~(mem::kPage - 1);
    if (std::find(pages.begin(), pages.end(), page) == pages.end()) pages.push_back(page);
}

bool ValidatePointerSpan(const xbe::Image* image, uint32_t address, uint32_t size, bool write, const char* name,
                         std::string& error) {
    if (uint64_t(address) + size > (uint64_t(1) << 32)) {
        error = std::string(name) + " overflows the guest address space";
        return false;
    }
    if (InImportPage(address, uint64_t(address) + size)) {
        error = std::string(name) + " points at the ordinal-255 import page";
        return false;
    }
    if (mem::CheckAccess(address, size, write ? mem::kReadWrite : mem::kRead) != mem::AccessStatus::kOk) {
        error = std::string(name) + " target span is not committed or lacks permission";
        return false;
    }
    if (image) {
        bool inside = address >= image->base && uint64_t(address) + size <= uint64_t(image->base) + image->header_size;
        if (!inside) {
            const xbe::Section* section = image->SectionAt(address);
            inside = section && uint64_t(address) + size <= uint64_t(section->va) + section->vsize;
        }
        if (!inside) {
            error = std::string(name) + " target span is outside the prepared image";
            return false;
        }
    }
    return true;
}

bool ValidateStack(uint32_t base, uint32_t top, uint32_t esp, std::string& error) {
    if (!base || top <= base || top - base < kRealStackCommitted) {
        error = "stack allocation is too small";
        return false;
    }
    if ((esp & 15) != 0) {
        error = "initial ESP is not 16-byte aligned";
        return false;
    }
    if (esp > top || esp < base + kRealStackHeadroom) {
        error = "initial ESP is outside the reviewed stack headroom";
        return false;
    }
    if (mem::CheckAccess(base, top - base, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = "stack span is not writable";
        return false;
    }
    return true;
}

bool ValidateStackSpan(const RealFacts& facts, uint32_t address, uint32_t size, const char* name, std::string& error) {
    if (address < facts.stack_base || uint64_t(address) + size > facts.stack_top) {
        error = std::string(name) + " is outside the reviewed guest stack";
        return false;
    }
    if (mem::CheckAccess(address, size, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = std::string(name) + " is not writable";
        return false;
    }
    return true;
}

bool ValidateImportArguments(const RealFacts& facts, uint32_t esp, std::string& error) {
    if (esp != facts.import_esp) {
        error = "ordinal-255 argument address does not match the reviewed stack";
        return false;
    }
    if (!ValidateStackSpan(facts, esp, kImportArgumentBytes, "import argument block", error)) return false;
    if (!ValidateStackSpan(facts, facts.import_arg0, 4, "import handle output", error)) return false;
    return true;
}

bool ValidatePagePermissions(const RealFacts& facts, std::string& error) {
    const RouteStep* first = FindStep(kEntry);
    const RouteStep* helper = FindStep(kCallTarget);
    if (!first || !helper || mem::CheckAccess(kEntry, first->length, mem::kExecute) != mem::AccessStatus::kOk ||
        mem::CheckAccess(kCallTarget, helper->length, mem::kExecute) != mem::AccessStatus::kOk) {
        error = "reviewed code pages are not executable";
        return false;
    }
    for (uint32_t address : {kEntryValue, kEntryPointer, kHelperValue, kGlobalReadA, kGlobalReadB, kGlobalLength,
                             kGlobalStore}) {
        if (mem::CheckAccess(address, 4, mem::kRead) != mem::AccessStatus::kOk) {
            error = "a reviewed global is not readable";
            return false;
        }
    }
    if (mem::CheckAccess(kCounterAddress, 4, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = "the reviewed counter is not writable";
        return false;
    }
    if (mem::CheckAccess(facts.p, 4, facts.p_write_required ? mem::kReadWrite : mem::kRead) !=
        mem::AccessStatus::kOk) {
        error = "the entry pointer does not have the required permission";
        return false;
    }
    if (mem::CheckAccess(facts.q, 4, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = "the helper store pointer is not writable";
        return false;
    }
    if (mem::CheckAccess(kRealImportPage, 4, mem::kRead) != mem::AccessStatus::kOk ||
        mem::CheckAccess(kCloseImportSlot, 4, mem::kRead) != mem::AccessStatus::kOk) {
        error = "the reviewed import page is not readable";
        return false;
    }
    if (mem::CheckAccess(kRealImportPage, 1, mem::kWrite) == mem::AccessStatus::kOk) {
        error = "the reviewed import page is writable";
        return false;
    }
    if (mem::CheckAccess(kImportSlot, 4, mem::kExecute) == mem::AccessStatus::kOk) {
        error = "the reviewed import page is executable";
        return false;
    }
    if (mem::CheckAccess(kImportReturn, 1, mem::kExecute) != mem::AccessStatus::kOk) {
        error = "the import call return PC is not executable";
        return false;
    }
    if (kImportReturn < X86CodeBase || kImportReturn >= X86CodeEnd) {
        error = "the import call return PC is outside the generated code";
        return false;
    }
    if (mem::CheckAccess(kCloseImportReturn, 1, mem::kExecute) != mem::AccessStatus::kOk) {
        error = "the close call return PC is not executable";
        return false;
    }
    if (kCloseImportReturn < X86CodeBase || kCloseImportReturn >= X86CodeEnd) {
        error = "the close call return PC is outside the generated code";
        return false;
    }
    if (!ValidateStackSpan(facts, facts.import_esp, kImportArgumentBytes, "import argument block", error))
        return false;
    if (!ValidateStackSpan(facts, facts.import_arg0, 4, "import handle output", error)) return false;
    if (!ValidateStackSpan(facts, facts.close_esp, 8, "NtClose call frame", error)) return false;
    if (mem::CheckAccess(kEntry & ~(mem::kPage - 1), 1, mem::kWrite) == mem::AccessStatus::kOk) {
        error = "the reviewed code page is writable";
        return false;
    }
    if (mem::CheckAccess(kGlobalReadA & ~(mem::kPage - 1), 1, mem::kWrite) == mem::AccessStatus::kOk) {
        error = "the reviewed read-only data page is writable";
        return false;
    }
    if (mem::CheckAccess(kCounterAddress & ~(mem::kPage - 1), 1, mem::kWrite) != mem::AccessStatus::kOk) {
        error = "the reviewed writable data page is not writable";
        return false;
    }
    return true;
}

bool ReadRealU32(uint32_t address, const char* name, uint32_t& value, std::string& error) {
    if (mem::CheckAccess(address, 4, mem::kRead) != mem::AccessStatus::kOk) {
        error = std::string(name) + " is not readable";
        return false;
    }
    if (mem::Read(address, &value, sizeof(value)) != mem::AccessStatus::kOk) {
        error = std::string(name) + " read failed";
        return false;
    }
    return true;
}

bool RoundSection(const xbe::Image& image, const char* name, uint32_t& begin, uint32_t& end) {
    for (const xbe::Section& section : image.sections) {
        if (section.name != name || !(section.flags & xbe::kPreload)) continue;
        begin = section.va & ~(mem::kPage - 1);
        end = (section.va + section.vsize + mem::kPage - 1) & ~(mem::kPage - 1);
        return true;
    }
    return false;
}

bool PrepareRealImage(const std::string& path, xbe::Image& image, Options& options, RealFacts& facts,
                      std::string& error) {
    if (!ValidateRealIdentity(path, image, error)) return false;
    if (!ValidateRealRouteText(image, error)) return false;
    const std::string sha1 = Sha1Hex(image.file);
    if (!xbe::PrepareImage(image, kRealEntry, sha1, error)) return false;
    kernel::g_base = mem::Base();
    if (!dispatch::Init(error)) {
        error = "dispatch init failed: " + error;
        return false;
    }
    if (X86FuncMappingCount != 6 || X86ContinuationCount != 0) {
        error = "generated real mapping does not contain the six reviewed functions and boundary stubs";
        return false;
    }
    bool entry = false, helper = false, debug_boundary = false, close_wrapper = false, close_stub = false,
         off_path_boundary = false;
    for (size_t i = 0; i < X86FuncMappingCount; ++i) {
        if (X86FuncMappings[i].guest == kEntry) entry = true;
        else if (X86FuncMappings[i].guest == kCallTarget) helper = true;
        else if (X86FuncMappings[i].guest == kDebugPath) debug_boundary = true;
        else if (X86FuncMappings[i].guest == kCloseTarget) close_wrapper = true;
        else if (X86FuncMappings[i].guest == kCloseErrorTarget) close_stub = true;
        else if (X86FuncMappings[i].guest == kOffPathTarget) off_path_boundary = true;
        else {
            error = "generated real mapping contains an unexpected function";
            return false;
        }
    }
    if (!entry || !helper || !debug_boundary || !close_wrapper || !close_stub || !off_path_boundary) {
        error = "generated real mapping is missing a reviewed function or boundary stub";
        return false;
    }
    if (!ValidateHookCoverage(std::string(EXECUTION_PROBE_REAL_GENERATED_DIR) + "/x86_recomp_shared.h",
                              std::string(EXECUTION_PROBE_REAL_GENERATED_DIR) + "/x86_recomp.0.cpp", error))
        return false;

    // The whole prepared image starts read-only; the reviewed code and data
    // sections then get exactly the permissions the route needs.
    if (!ProtectRange(image.base, image.image_size, mem::kRead)) {
        error = "could not protect the prepared image read-only";
        return false;
    }
    uint32_t text_begin = 0, text_end = 0, data_begin = 0, data_end = 0;
    if (!RoundSection(image, ".text", text_begin, text_end) ||
        !ProtectRange(text_begin, text_end - text_begin, mem::kRead | mem::kExecute)) {
        error = "could not protect the reviewed code section executable";
        return false;
    }
    if (!RoundSection(image, ".data", data_begin, data_end) ||
        !ProtectRange(data_begin, data_end - data_begin, mem::kReadWrite)) {
        error = "could not protect the reviewed data section writable";
        return false;
    }
    if (!ProtectRange(kRealImportPage, mem::kPage, mem::kRead | mem::kWrite)) {
        error = "could not open the reviewed import page for its patches";
        return false;
    }
    for (RealImportPatch& patch : g_real_imports) {
        uint32_t authentic = 0;
        if (mem::Read(patch.slot, &authentic, sizeof(authentic)) != mem::AccessStatus::kOk ||
            authentic != patch.encoding) {
            error = patch.ordinal == kImportOrdinal ? "import slot is not the authentic ordinal-255 encoding"
                                                    : "close import slot is not the authentic ordinal-187 encoding";
            return false;
        }
        const uint32_t target = dispatch::KernelThunkAddress(patch.ordinal);
        if (!target || mem::Write(patch.slot, &target, sizeof(target)) != mem::AccessStatus::kOk) {
            error = "could not patch a reviewed import slot to its kernel thunk";
            return false;
        }
        patch.original = authentic;
        patch.target = target;
        patch.active = true;
    }
    if (!ProtectRange(kRealImportPage, mem::kPage, mem::kRead)) {
        error = "could not protect the reviewed import page read-only";
        return false;
    }
    facts.import_authentic = g_real_imports[0].original;
    facts.import_target = g_real_imports[0].target;
    facts.close_import_authentic = g_real_imports[1].original;
    facts.close_import_target = g_real_imports[1].target;
    g_real_worker_baseline = kernel::ThreadWorkerLaunchCountForTest();
    if (!PrepareKernelBridge(true, true, error)) return false;

    facts.base = image.base;
    facts.image_size = image.image_size;
    facts.header_size = image.header_size;
    facts.sections = image.sections.size();
    if (!ReadRealU32(kEntryValue, "[0x10108]", facts.g10108, error) ||
        !ReadRealU32(kEntryPointer, "[0x10118]", facts.g10118, error) ||
        !ReadRealU32(kHelperValue, "[0x10130]", facts.g10130, error) ||
        !ReadRealU32(kGlobalReadB, "[0x3ED3EC]", facts.g3ec, error) ||
        !ReadRealU32(kGlobalLength, "[0x3ED3F0]", facts.g3f0, error) ||
        !ReadRealU32(kGlobalStore, "[0x3ED3F4]", facts.g3f4, error) ||
        !ReadRealU32(kGlobalReadA, "[0x3ED3FC]", facts.g3fc, error) ||
        !ReadRealU32(kCounterAddress, "[0x4EDC74]", facts.g4edc74, error))
        return false;

    facts.p = facts.g10118;
    if (!ValidatePointerSpan(&image, facts.p, 4, false, "entry pointer", error)) return false;
    if (!ReadRealU32(facts.p, "[entry pointer]", facts.p_value, error)) return false;
    facts.entry_ea = uint32_t(facts.g10108 - facts.p + 0x10000);
    facts.entry_taken = facts.entry_ea >= facts.p_value;
    facts.p_write_required = !facts.entry_taken;
    if (facts.p_write_required && !ValidatePointerSpan(&image, facts.p, 4, true, "entry pointer write", error))
        return false;

    facts.q = facts.g3f4;
    if (!ValidatePointerSpan(&image, facts.q, 4, true, "helper store pointer", error)) return false;
    if (!ReadRealU32(facts.q, "[helper store pointer]", facts.q_before, error)) return false;
    if (mem::CheckAccess(kCounterAddress, 4, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = "[0x4EDC74] is not writable";
        return false;
    }

    const uint32_t eax = facts.g3fc - facts.g3ec;
    const uint32_t lea = eax + facts.g3f0 + 0x0F;
    const uint32_t masked = lea & 0xFFFFFFF0u;
    const uint32_t added = masked + 4;
    const int64_t dividend = int32_t(added);
    const int64_t quotient = dividend / -4;
    const int64_t remainder = dividend % -4;
    if (quotient < INT32_MIN || quotient > INT32_MAX) {
        error = "reviewed IDIV quotient does not fit EAX";
        return false;
    }
    facts.idiv_dividend = added;
    facts.counter_after = added;
    facts.idiv_quotient = uint32_t(int32_t(quotient));
    facts.idiv_remainder = uint32_t(int32_t(remainder));
    facts.q_after = facts.idiv_quotient;

    const uint32_t stack_top = mem::AllocStack(kRealStackRequest);
    if (!stack_top) {
        error = "could not allocate the reviewed guest stack";
        return false;
    }
    facts.stack_top = stack_top;
    facts.stack_base = stack_top - kRealStackCommitted;
    facts.esp = stack_top - 16;
    if (!ValidateStack(facts.stack_base, facts.stack_top, facts.esp, error)) return false;
    if (!ValidateStackSpan(facts, facts.esp, 4, "root sentinel slot", error)) return false;

    // The reviewed route pushes the ten authentic arguments; the only slots it
    // does not write itself must still be the zero-filled stack.
    facts.import_esp = facts.esp - kImportStackBytes;
    facts.import_arg0 = facts.import_esp + kImportArg0Offset;
    facts.import_return = kImportReturn;
    facts.import_args = {facts.import_arg0,
                         0,
                         facts.g10130,
                         facts.counter_after,
                         0,
                         kStartRoutine,
                         0,
                         0,
                         0,
                         kSystemRoutine};
    if (!ValidateImportArguments(facts, facts.import_esp, error)) return false;
    if (facts.esp < facts.import_esp + kImportArgumentBytes) {
        error = "root sentinel slot overlaps the reviewed import block";
        return false;
    }
    facts.close_esp = facts.import_esp + 0x38;
    facts.close_arg_slot = facts.close_esp + 4;
    facts.close_return = kCloseImportReturn;
    if (!ValidateStackSpan(facts, facts.close_esp, 8, "NtClose call frame", error)) return false;
    g_expected_kernel_args = facts.import_args;
    g_have_expected_kernel_args = true;
    std::array<uint32_t, kImportStackSlots> initial_slots{};
    if (mem::Read(facts.import_esp + 0x38, initial_slots.data(), sizeof(initial_slots)) != mem::AccessStatus::kOk)
        return false;
    for (uint32_t value : initial_slots) {
        if (value != 0) {
            error = "reviewed import stack slots are not the zero-filled stack";
            return false;
        }
    }

    facts.spans.clear();
    AddSpan(facts.spans, kRealImageBase, kRealImageBase + kRealHeaderSize, false);
    AddSpan(facts.spans, kRealRdataPage, kRealRdataPage + mem::kPage, false);
    AddSpan(facts.spans, kRealCounterPage, kRealCounterPage + mem::kPage, true);
    AddSpan(facts.spans, kRealTargetPage, kRealTargetPage + mem::kPage, true);
    AddSpan(facts.spans, facts.p & ~(mem::kPage - 1), (facts.p & ~(mem::kPage - 1)) + mem::kPage, false);
    AddSpan(facts.spans, facts.q & ~(mem::kPage - 1), (facts.q & ~(mem::kPage - 1)) + mem::kPage, true);
    AddSpan(facts.spans, facts.stack_base, facts.stack_top, true);
    if (!ValidateReviewedSpans(facts.spans, error)) return false;

    facts.snapshot_pages.clear();
    AddSnapshotPage(facts.snapshot_pages, kRealImageBase);
    AddSnapshotPage(facts.snapshot_pages, kRealRdataPage);
    AddSnapshotPage(facts.snapshot_pages, kRealCounterPage);
    AddSnapshotPage(facts.snapshot_pages, kRealTargetPage);
    AddSnapshotPage(facts.snapshot_pages, facts.p);
    AddSnapshotPage(facts.snapshot_pages, facts.q);
    for (uint32_t page = facts.stack_base; page < facts.stack_top; page += mem::kPage)
        AddSnapshotPage(facts.snapshot_pages, page);

    if (!ValidatePagePermissions(facts, error)) return false;

    const mem::Stats baseline = mem::Query();
    facts.baseline_system = baseline.system_used;
    facts.baseline_contiguous = baseline.contiguous_used;
    facts.baseline_virtual = baseline.virtual_committed;

    options = Options{};
    options.start_pc = kEntry;
    options.initial_esp = facts.esp;
    options.allow_kernel_call = true;
    options.require_kernel_success = true;
    options.allowed_spans = facts.spans;
    options.snapshot_pages = facts.snapshot_pages;
    options.image_base = image.base;
    options.image_size = image.image_size;
    options.free_image = true;
    options.stack_top = facts.stack_top;
    options.free_stack = true;
    return true;
}

void PrintRealFacts(const RealFacts& facts) {
    std::printf("PREFLIGHT identity: base=%08X image=%X header=%X sections=%u\n", facts.base, facts.image_size,
                facts.header_size, facts.sections);
    std::printf("PREFLIGHT route: %zu/%zu instructions matched, stop=%08X, import=%08X:%08X\n", kRouteCount,
                kRouteCount, kStopBefore, kImportSlot, kImportEncoding);
    std::printf("PREFLIGHT mapping: functions=6 (3 reviewed + 3 boundary stubs) continuations=0 hooks=%zu "
                "stop_hooks=0\n",
                kRouteCount);
    std::printf("PREFLIGHT globals: 10108=%08X 10118=%08X 10130=%08X 3ED3EC=%08X 3ED3F0=%08X 3ED3F4=%08X "
                "3ED3FC=%08X 4EDC74=%08X\n",
                facts.g10108, facts.g10118, facts.g10130, facts.g3ec, facts.g3f0, facts.g3f4, facts.g3fc,
                facts.g4edc74);
    std::printf("PREFLIGHT entry: ea=%08X cmp=%08X branch=%s pointer=%08X pointer_value=%08X pointer_write=%d\n",
                facts.entry_ea, facts.p_value, facts.entry_taken ? "taken" : "not-taken", facts.p, facts.p_value,
                int(facts.p_write_required));
    std::printf("PREFLIGHT idiv: dividend=%08X quotient=%08X remainder=%08X store=%08X before=%08X after=%08X\n",
                facts.idiv_dividend, facts.idiv_quotient, facts.idiv_remainder, facts.q, facts.q_before,
                facts.q_after);
    std::printf("PREFLIGHT helper: arg=00000000 branch=not-taken fall-through-value=%08X\n", facts.g10130);
    std::printf("PREFLIGHT expected: counter=%08X result=%08X entry_branch=%s helper_branch=not-taken\n",
                facts.counter_after, facts.q_after, facts.entry_taken ? "taken" : "not-taken");
    std::printf("PREFLIGHT stack: base=%08X top=%08X esp=%08X requested=%X committed=%X\n", facts.stack_base,
                facts.stack_top, facts.esp, kRealStackRequest, kRealStackCommitted);
    std::printf("PREFLIGHT import: slot=%08X authentic=%08X probe=%08X esp=%08X handle=%08X return=%08X\n",
                kImportSlot, facts.import_authentic, facts.import_target, facts.import_esp, facts.import_arg0,
                facts.import_return);
    std::printf("PREFLIGHT close: slot=%08X authentic=%08X probe=%08X esp=%08X arg=%08X return=%08X\n",
                kCloseImportSlot, facts.close_import_authentic, facts.close_import_target, facts.close_esp,
                facts.close_arg_slot, facts.close_return);
    std::printf("PREFLIGHT args: handle=%08X ext=%08X stack=%08X tls=%08X id=%08X start=%08X context=%08X "
                "suspended=%08X debug=%08X system=%08X\n",
                facts.import_args[0], facts.import_args[1], facts.import_args[2], facts.import_args[3],
                facts.import_args[4], facts.import_args[5], facts.import_args[6], facts.import_args[7],
                facts.import_args[8], facts.import_args[9]);
    std::printf("PREFLIGHT pages: entry=RX helper=RX header=R rdata=R data=RW import=R\n");
    std::printf("PREFLIGHT continuation: status=0 jnl=%08X taken debug=%08X forbidden ret=%08X target=%08X pop=%X "
                "caller=%08X jnz=%08X taken off_path=%08X forbidden close=%08X wrapper=%08X stop=%08X "
                "pre-instruction\n",
                kPostImportBranch, kDebugPath, kRetInstruction, kRetTarget, kRetPopBytes, kCallerBranch,
                kCallerContinuation, kCallerOffPath, kCloseCall, kCloseTarget, kStopBefore);
    std::printf("PREFLIGHT close path: entry=%08X import=%08X status=%08X jl=%08X not-taken success=%08X ret=%08X "
                "continue=%08X forbidden=%08X\n",
                kCloseTarget, kCloseImportCall, kCloseStatusTest, kCloseBranch, kCloseSuccess, kCloseRet,
                kCloseContinue, kCloseErrorPath);
    std::printf("PREFLIGHT root: sentinel=%08X slot=%08X host_esp=%08X pre_ret=%08X post_ret=%08X stop=%08X "
                "optional-ret\n",
                kRootReturnSentinel, facts.esp, facts.esp + 4, facts.esp, facts.esp + 4, kStopBefore);
    std::printf("PREFLIGHT limits: instructions=80 direct_calls=2 kernel_calls=2 memory=256 imports=2 callbacks=0 "
                "workers=0\n");
    std::printf("PREFLIGHT spans:");
    for (const Span& span : facts.spans)
        std::printf(" %08X-%08X:%s", span.begin, span.end, span.write ? "RW" : "R");
    std::printf("\nPREFLIGHT PASS\n");
}

std::string TempRealCopy(const char* tag) {
    char path[256];
    std::snprintf(path, sizeof(path), "/tmp/mostwanted-probe-%d-%s.xbe", int(getpid()), tag);
    return path;
}

bool CopyAndPatch(const std::string& source, const std::string& destination, int64_t offset, uint8_t mask) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(source.c_str(), bytes)) return false;
    if (offset < 0 || size_t(offset) >= bytes.size()) return false;
    bytes[size_t(offset)] ^= mask;
    std::ofstream out(destination, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(out);
}

bool VerifyRealPostconditions(const RealFacts& facts, std::string& error) {
    const StepRecord* entry_branch = nullptr;
    const StepRecord* helper_branch = nullptr;
    const StepRecord* helper_value = nullptr;
    const StepRecord* idiv_result = nullptr;
    const StepRecord* kernel_status = nullptr;
    const StepRecord* post_branch = nullptr;
    const StepRecord* caller_branch = nullptr;
    const StepRecord* close_status = nullptr;
    const StepRecord* close_branch = nullptr;
    for (const StepRecord& step : g_probe.trace) {
        if (step.pc == 0x002B392A) entry_branch = &step;
        if (step.pc == 0x002B5CAF) helper_branch = &step;
        if (step.pc == 0x002B5CD9) helper_value = &step;
        if (step.pc == 0x002B3956) idiv_result = &step;
        if (step.pc == kImportReturn) kernel_status = &step;
        if (step.pc == kPostImportBranch) post_branch = &step;
        if (step.pc == kCallerBranch) caller_branch = &step;
        if (step.pc == kCloseStatusTest) close_status = &step;
        if (step.pc == kCloseBranch) close_branch = &step;
    }
    if (g_probe.executed != (g_probe.options.allow_root_return ? 72u : 71u)) {
        error = "expected " + std::to_string(g_probe.options.allow_root_return ? 72 : 71) + " instructions, got " +
                std::to_string(g_probe.executed);
        return false;
    }
    if (g_probe.direct_calls != 2) {
        error = "expected two direct calls, got " + std::to_string(g_probe.direct_calls);
        return false;
    }
    if (g_probe.kernel_calls != 2) {
        error = "expected two kernel calls, got " + std::to_string(g_probe.kernel_calls);
        return false;
    }
    if (!facts.entry_taken || !entry_branch || !entry_branch->branch_valid || !entry_branch->branch_taken) {
        error = "entry JNB was not taken";
        return false;
    }
    if (!helper_branch || !helper_branch->branch_valid || helper_branch->branch_taken) {
        error = "helper JNZ was not statically not-taken";
        return false;
    }
    if (!helper_value || helper_value->eax != facts.g10130) {
        error = "helper fall-through register mismatch";
        return false;
    }
    if (!idiv_result || idiv_result->eax != facts.q_after) {
        error = "IDIV register mismatch";
        return false;
    }
    if (!kernel_status || kernel_status->eax != kernel::kSuccess) {
        error = "kernel status was not success at the post-import branch";
        return false;
    }
    if (!post_branch || !post_branch->branch_valid || !post_branch->branch_taken) {
        error = "post-import JNL was not taken";
        return false;
    }
    if (!caller_branch || !caller_branch->branch_valid || !caller_branch->branch_taken) {
        error = "caller JNZ was not taken";
        return false;
    }
    if (!close_status || close_status->eax != kernel::kSuccess) {
        error = "NtClose status was not success at the wrapper branch";
        return false;
    }
    if (!close_branch || !close_branch->branch_valid || close_branch->branch_taken) {
        error = "NtClose JL was not statically not-taken";
        return false;
    }
    const MemoryRecord* import = nullptr;
    const MemoryRecord* close_import = nullptr;
    unsigned import_records = 0;
    for (const MemoryRecord& record : g_probe.memory) {
        if (!InImportPage(record.address, uint64_t(record.address) + record.size)) continue;
        ++import_records;
        if (record.pc == kImportCall && record.address == kImportSlot) import = &record;
        if (record.pc == kCloseImportCall && record.address == kCloseImportSlot) close_import = &record;
    }
    if (ImportReads() != 2 || g_probe.import_reads != 2 || import_records != 2 ||
        g_probe.import_reads_255 != 1 || g_probe.import_reads_187 != 1) {
        error = "reviewed import slot read counts are not one per slot";
        return false;
    }
    if (!import || import->size != 4 || import->write) {
        error = "the ordinal-255 operand read is not the reviewed call";
        return false;
    }
    if (!close_import || close_import->size != 4 || close_import->write) {
        error = "the ordinal-187 operand read is not the reviewed call";
        return false;
    }
    if (kernel::KernelWorkerCountForTest() != 0 ||
        kernel::ThreadWorkerLaunchCountForTest() != g_probe.worker_launch_baseline) {
        error = "a worker was activated";
        return false;
    }
    if (!g_probe.have_stop || g_probe.stop.pc != kStopBefore) {
        error = "mandatory stop was not at the caller RET boundary";
        return false;
    }
    if (g_probe.stop.esp != facts.esp) {
        error = "the caller pops did not restore the initial stack";
        return false;
    }
    if (g_probe.stop.eax != 1 || g_probe.stop.ecx != 0 || g_probe.stop.ebp != 0 || g_probe.stop.esi != 0 ||
        g_probe.stop.edi != 0) {
        error = "post-close register state mismatch";
        return false;
    }
    if (g_probe.options.allow_root_return) {
        if (!g_probe.stop_root_frame) {
            error = "the root RET had no checked call frame";
            return false;
        }
        if (!g_probe.root_completed || g_probe.root_eax != 1) {
            error = "root completion did not return EAX=1";
            return false;
        }
        if (g_probe.root_pre_esp != facts.esp + 4 || g_probe.root_esp != facts.esp + 4) {
            error = "root return did not restore the reviewed host ESP";
            return false;
        }
    } else if (!g_probe.stop_checked_call_clear) {
        error = "the checked call frames were not completed by the RETs";
        return false;
    }
    const uint32_t last = g_probe.options.allow_root_return ? kStopBefore : kLastInstruction;
    if (g_probe.trace.empty() || g_probe.trace.back().pc != last) {
        error = g_probe.options.allow_root_return ? "the root RET was not the last executed instruction"
                                                  : "the caller pops were not the last executed instructions";
        return false;
    }
    for (const StepRecord& step : g_probe.trace) {
        if (!g_probe.options.allow_root_return && step.pc == kStopBefore) {
            error = "the caller RET was executed";
            return false;
        }
    }
    bool sentinel_read = false;
    for (const MemoryRecord& record : g_probe.memory) {
        if (record.address != facts.esp) continue;
        if (!g_probe.options.allow_root_return || record.pc != kStopBefore || record.write || record.size != 4) {
            error = "the unreviewed root sentinel slot was accessed by the prefix";
            return false;
        }
        sentinel_read = true;
    }
    if (g_probe.options.allow_root_return && !sentinel_read) {
        error = "the root RET did not read the sentinel slot";
        return false;
    }
    uint32_t counter = 0, result = 0;
    if (mem::Read(kCounterAddress, &counter, sizeof(counter)) != mem::AccessStatus::kOk ||
        counter != facts.counter_after) {
        error = "counter postcondition failed at 004EDC74";
        return false;
    }
    if (mem::Read(facts.q, &result, sizeof(result)) != mem::AccessStatus::kOk || result != facts.q_after) {
        error = "result postcondition failed at the reviewed store";
        return false;
    }
    const uint32_t handle = g_kernel255_handle;
    uint32_t published = 0;
    if (!handle || g_kernel187_arg != handle ||
        mem::Read(facts.import_args[0], &published, sizeof(published)) != mem::AccessStatus::kOk ||
        published != handle) {
        error = "the reviewed close did not carry the published thread handle";
        return false;
    }
    if (!g_prepared_thread || g_prepared_thread->state.load() != kernel::ThreadState::kPrepared) {
        error = "no inactive thread was prepared and retained";
        return false;
    }
    if (g_prepared_thread->published_handle != handle || g_prepared_thread->start_routine != facts.import_args[5] ||
        g_prepared_thread->start_context != facts.import_args[6] ||
        g_prepared_thread->system_routine != facts.import_args[9] || g_prepared_thread->suspend.load() != 0) {
        error = "the prepared thread does not carry the authentic routines";
        return false;
    }
    if (kernel::FromHandle(handle)) {
        error = "the handle table entry survived the reviewed close";
        return false;
    }
    if (g_kernel255_calls != 1 || g_kernel255_esp != facts.import_esp - 4 ||
        g_kernel255_return != facts.import_return || g_kernel255_args != facts.import_args) {
        error = "ordinal-255 call frame or arguments do not match the preflight";
        return false;
    }
    if (g_kernel187_calls != 1 || g_kernel187_esp != facts.close_esp ||
        g_kernel187_return != facts.close_return || g_kernel187_arg != handle ||
        g_kernel187_status != kernel::kSuccess || g_kernel255_status != kernel::kSuccess) {
        error = "ordinal-187 call frame or result does not match the reviewed wrapper";
        return false;
    }
    if (!kernel::Threads().empty()) {
        error = "a guest thread became active";
        return false;
    }
    return true;
}

void FakeRealTrace(const RealFacts& facts, bool entry_taken, bool helper_taken, bool root = false) {
    g_probe = ProbeState{};
    g_probe.options.allow_root_return = root;
    g_probe.options.root_sentinel_at_entry = root;
    g_probe.executed = root ? 72 : 71;
    g_probe.direct_calls = 2;
    g_probe.kernel_calls = 2;
    g_probe.import_reads = 2;
    g_probe.import_reads_255 = 1;
    g_probe.import_reads_187 = 1;
    g_probe.worker_launch_baseline = kernel::ThreadWorkerLaunchCountForTest();
    g_probe.memory.push_back({kImportCall, kImportSlot, 4, false});
    g_probe.memory.push_back({kCloseImportCall, kCloseImportSlot, 4, false});
    g_kernel255_calls = 1;
    g_kernel255_status = kernel::kSuccess;
    g_kernel187_calls = 1;
    g_kernel187_status = kernel::kSuccess;
    StepRecord entry{};
    entry.pc = 0x002B392A;
    entry.branch_valid = true;
    entry.branch_taken = entry_taken;
    g_probe.trace.push_back(entry);
    StepRecord helper{};
    helper.pc = 0x002B5CAF;
    helper.branch_valid = true;
    helper.branch_taken = helper_taken;
    g_probe.trace.push_back(helper);
    StepRecord idiv{};
    idiv.pc = 0x002B3956;
    idiv.eax = facts.q_after;
    g_probe.trace.push_back(idiv);
    StepRecord helper_value{};
    helper_value.pc = 0x002B5CD9;
    helper_value.eax = facts.g10130;
    g_probe.trace.push_back(helper_value);
    StepRecord status{};
    status.pc = kImportReturn;
    status.eax = kernel::kSuccess;
    g_probe.trace.push_back(status);
    StepRecord post_branch{};
    post_branch.pc = kPostImportBranch;
    post_branch.branch_valid = true;
    post_branch.branch_taken = true;
    g_probe.trace.push_back(post_branch);
    StepRecord ret{};
    ret.pc = kRetInstruction;
    g_probe.trace.push_back(ret);
    StepRecord caller{};
    caller.pc = kCallerBranch;
    caller.branch_valid = true;
    caller.branch_taken = true;
    g_probe.trace.push_back(caller);
    StepRecord close_status{};
    close_status.pc = kCloseStatusTest;
    close_status.eax = kernel::kSuccess;
    g_probe.trace.push_back(close_status);
    StepRecord close_branch{};
    close_branch.pc = kCloseBranch;
    close_branch.branch_valid = true;
    close_branch.branch_taken = false;
    g_probe.trace.push_back(close_branch);
    if (root) {
        StepRecord ret_root{};
        ret_root.pc = kStopBefore;
        g_probe.trace.push_back(ret_root);
        g_probe.memory.push_back({kStopBefore, facts.esp, 4, false});
    } else {
        StepRecord last{};
        last.pc = kLastInstruction;
        g_probe.trace.push_back(last);
    }
    StepRecord stop{};
    stop.pc = kStopBefore;
    g_probe.stop = stop;
    g_probe.stop.eax = 1;
    g_probe.stop.ecx = 0;
    g_probe.stop.ebp = 0;
    g_probe.stop.esi = 0;
    g_probe.stop.edi = 0;
    g_probe.stop.esp = facts.esp;
    g_probe.stop_checked_call_clear = !root;
    g_probe.stop_root_frame = root;
    g_probe.root_completed = root;
    g_probe.root_pre_esp = facts.esp + 4;
    g_probe.root_esp = facts.esp + 4;
    g_probe.root_eax = 1;
    g_probe.have_stop = true;
}

// restores the authentic import slot before releasing the image; a failed
// restore must fail the process, so it is propagated, not printed and dropped
bool TeardownReal(const Options& options) {
    const bool restored = RestoreImportSlots();
    const bool freed = Teardown(options);
    return restored && freed;
}

bool CancelRealPreparedThread(const RealFacts& facts, std::string& error) {
    if (!g_prepared_thread) {
        error = "no prepared thread to cancel";
        return false;
    }
    const kernel::ThreadRef thread = g_prepared_thread;
    g_prepared_thread.reset();
    uint32_t handle = 0;
    const bool handle_read =
        mem::Read(facts.import_args[0], &handle, sizeof(handle)) == mem::AccessStatus::kOk;
    const uint32_t stack_top = thread->stack_top;
    const bool handle_closed = handle_read && handle && !kernel::FromHandle(handle);
    const bool prepared = handle_closed && thread->state.load() == kernel::ThreadState::kPrepared &&
                          thread->published_handle == handle && kernel::Threads().empty() &&
                          kernel::ThreadWorkerLaunchCountForTest() == g_real_worker_baseline;
    const bool cancelled = kernel::CancelPreparedThread(thread);
    uint32_t cleared = 1;
    const bool cleared_ok =
        mem::Read(facts.import_args[0], &cleared, sizeof(cleared)) == mem::AccessStatus::kOk && cleared == 0;
    const bool stack_freed = !mem::IsCommitted(stack_top - 1);
    const mem::Stats stats = mem::Query();
    const bool resources = stats.system_used == facts.baseline_system &&
                           stats.contiguous_used == facts.baseline_contiguous &&
                           stats.virtual_committed == facts.baseline_virtual;
    const bool ok = prepared && cancelled && cleared_ok && stack_freed && !kernel::FromHandle(handle) &&
                    thread->state.load() == kernel::ThreadState::kCancelled && thread->published_handle == 0 &&
                    kernel::Threads().empty() && resources;
    std::printf("CANCEL handle_closed=%d prepared=%d cancelled=%d output_cleared=%d stack_freed=%d resources=%d "
                "workers=%zu\n",
                int(handle_closed), int(prepared), int(cancelled), int(cleared_ok), int(stack_freed), int(resources),
                kernel::KernelWorkerCountForTest());
    if (!ok) error = "prepared thread cancellation did not roll back all resources";
    return ok;
}

int RealChild(const std::string& scenario) {
    if (!mem::Init()) {
        std::printf("SETUP fail memory init\n");
        return 4;
    }
    std::string error;
    xbe::Image image;
    Options options;
    RealFacts facts;
    bool ok = false;
    int exit_code = 5;
    uint32_t execute_counter = 0, execute_result = 0;

    if (scenario == "execute" || scenario == "execute-root-return") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) {
            std::printf("SETUP fail %s\n", error.c_str());
            exit_code = 4;
        } else {
            if (scenario == "execute-root-return") {
                options.allow_root_return = true;
                options.root_sentinel_at_entry = true;
            }
            const Report report = RunProbe(options);
            if (report.failure) {
                PrintFailure(report.error);
                exit_code = 3;
            } else {
                std::string post_error;
                if (!VerifyRealPostconditions(facts, post_error)) {
                    std::printf("RESULT fail reason=\"%s\"\n", post_error.c_str());
                    exit_code = 3;
                } else if (!CancelRealPreparedThread(facts, post_error)) {
                    std::printf("RESULT fail reason=\"%s\"\n", post_error.c_str());
                    exit_code = 3;
                } else {
                    mem::Read(kCounterAddress, &execute_counter, sizeof(execute_counter));
                    mem::Read(facts.q, &execute_result, sizeof(execute_result));
                    exit_code = 0;
                }
            }
        }
    } else if (scenario == "preflight") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) PrintRealFacts(facts);
        else std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 0 : 3;
    } else if (scenario == "identity-missing") {
        ok = ValidateRealIdentity("/tmp/mostwanted-probe-does-not-exist.xbe", image, error);
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "identity-mismatch") {
        const std::string path = TempRealCopy("mismatch");
        const bool copied = CopyAndPatch(EXECUTION_PROBE_REAL_XBE, path, 0x100, 0xFF);
        ok = copied && ValidateRealIdentity(path, image, error);
        std::remove(path.c_str());
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "route-mismatch") {
        if (ValidateRealIdentity(EXECUTION_PROBE_REAL_XBE, image, error)) {
            const int64_t offset = image.Offset(kEntry);
            if (offset >= 0 && size_t(offset) < image.file.size()) {
                image.file[size_t(offset)] ^= 0x01;
                ok = ValidateRealRouteText(image, error);
            } else {
                error = "entry is not file-backed";
            }
        }
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "incomplete-hooks" || scenario == "unexpected-target" || scenario == "extra-hook" ||
               scenario == "post-import-instruction") {
        std::vector<uint8_t> header_bytes, source_bytes;
        const std::string header_path = std::string(EXECUTION_PROBE_REAL_GENERATED_DIR) + "/x86_recomp_shared.h";
        const std::string source_path = std::string(EXECUTION_PROBE_REAL_GENERATED_DIR) + "/x86_recomp.0.cpp";
        if (ReadFile(header_path.c_str(), header_bytes) && ReadFile(source_path.c_str(), source_bytes)) {
            std::string header(header_bytes.begin(), header_bytes.end());
            std::string source(source_bytes.begin(), source_bytes.end());
            if (scenario == "incomplete-hooks") {
                const std::string needle = "ProbeStep_0x002B3940(ctx, base);";
                const size_t at = source.find(needle);
                if (at != std::string::npos) source.erase(at, needle.size());
            } else if (scenario == "unexpected-target") {
                const size_t at = source.find("x86_checked_call(ctx, base, 0x2B5CA7u");
                if (at != std::string::npos) {
                    const size_t arg = source.find("0x2B5CA7u", at);
                    if (arg != std::string::npos) source.replace(arg, 10, "0x2B5CA8u");
                }
            } else if (scenario == "extra-hook") {
                source += "\nProbeStep_0x002B3916(ctx, base);\n";
            } else {
                source += "\nProbeStop_0x002B5D11(ctx, base);\n";
            }
            ok = ValidateHookCoverageText(header, source, error);
        } else {
            error = "generated hook files are missing: " + header_path + " / " + source_path;
        }
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "overlapping-spans") {
        const std::vector<Span> spans = {{0x00010000, 0x00011000, true}, {0x00010800, 0x00011800, true}};
        ok = ValidateReviewedSpans(spans, error);
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "pointer-uncommitted") {
        ok = ValidatePointerSpan(nullptr, 0x60000000, 4, false, "pointer", error);
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "invalid-stack") {
        ok = ValidateStack(0x0FF000, 0x100000, 0x100008, error);
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "postcondition-count" || scenario == "postcondition-branch" ||
               scenario == "postcondition-caller-branch" || scenario == "postcondition-close-branch" ||
               scenario == "postcondition-close-status" || scenario == "postcondition-post-branch" ||
               scenario == "postcondition-import" || scenario == "postcondition-counter" ||
               scenario == "postcondition-result" || scenario == "postcondition-ret-cleanup" ||
               scenario == "postcondition-root-completed" || scenario == "postcondition-root-esp" ||
               scenario == "postcondition-root-frame" || scenario == "postcondition-root-ret" ||
               scenario == "postcondition-root-sentinel") {
        const bool root_case = scenario == "postcondition-root-completed" || scenario == "postcondition-root-esp" ||
                               scenario == "postcondition-root-frame" || scenario == "postcondition-root-ret" ||
                               scenario == "postcondition-root-sentinel";
        const bool needs_image = scenario == "postcondition-counter" || scenario == "postcondition-result";
        if (needs_image) ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        else {
            facts = RealFacts{};
            facts.entry_taken = true;
            ok = true;
        }
        if (ok) {
            facts.entry_taken = true;
            FakeRealTrace(facts, true, false, root_case);
            if (scenario == "postcondition-count") g_probe.executed = 70;
            else if (scenario == "postcondition-branch") g_probe.trace[0].branch_taken = false;
            else if (scenario == "postcondition-caller-branch") {
                for (StepRecord& step : g_probe.trace)
                    if (step.pc == kCallerBranch) step.branch_taken = false;
            }
            else if (scenario == "postcondition-close-branch") {
                for (StepRecord& step : g_probe.trace)
                    if (step.pc == kCloseBranch) step.branch_taken = true;
            }
            else if (scenario == "postcondition-close-status") {
                for (StepRecord& step : g_probe.trace)
                    if (step.pc == kCloseStatusTest) step.eax = kernel::kInvalidHandle;
            }
            else if (scenario == "postcondition-post-branch") {
                for (StepRecord& step : g_probe.trace)
                    if (step.pc == kPostImportBranch) step.branch_taken = false;
            }
            else if (scenario == "postcondition-ret-cleanup") g_probe.stop.esp += 4;
            else if (scenario == "postcondition-root-completed") g_probe.root_completed = false;
            else if (scenario == "postcondition-root-esp") g_probe.root_esp += 4;
            else if (scenario == "postcondition-root-frame") g_probe.stop_root_frame = false;
            else if (scenario == "postcondition-root-ret") g_probe.trace.back().pc = kLastInstruction;
            else if (scenario == "postcondition-root-sentinel") {
                for (auto it = g_probe.memory.begin(); it != g_probe.memory.end();) {
                    if (it->address == facts.esp)
                        it = g_probe.memory.erase(it);
                    else
                        ++it;
                }
            }
            else if (scenario == "postcondition-import")
                g_probe.memory.push_back({kImportCall, kImportSlot, 4, false});
            else if (scenario == "postcondition-result") Write32(kCounterAddress, facts.counter_after);
            std::string post_error;
            ok = VerifyRealPostconditions(facts, post_error);
            if (!ok) std::printf("RESULT fail reason=\"%s\"\n", post_error.c_str());
            exit_code = ok ? 5 : 3;
        } else {
            std::printf("SETUP fail %s\n", error.c_str());
            exit_code = 4;
        }
    } else if (scenario == "import-slot-mismatch") {
        const std::string path = TempRealCopy("import");
        bool copied = ValidateRealIdentity(EXECUTION_PROBE_REAL_XBE, image, error);
        const int64_t offset = copied ? image.Offset(kImportSlot) : -1;
        copied = copied && offset >= 0 && CopyAndPatch(EXECUTION_PROBE_REAL_XBE, path, offset, 0x01);
        xbe::Image patched;
        std::vector<uint8_t> patched_bytes;
        std::string patched_error;
        const bool parsed = copied && ReadFile(path.c_str(), patched_bytes) &&
                            patched.Parse(std::move(patched_bytes), patched_error);
        const bool accepted = parsed && ValidateRealRouteText(patched, patched_error);
        std::remove(path.c_str());
        if (accepted || !parsed) {
            // the negative test itself failed: report an internal failure, not a rejection
            error = accepted ? "patched import slot was accepted" : "could not prepare the import-slot copy";
            std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
            exit_code = 5;
        } else {
            error = patched_error.empty() ? "import slot copy was rejected without a reason" : patched_error;
            std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
            exit_code = 3;
        }
    } else if (scenario == "restore-failure" || scenario == "restore-close-failure") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) g_import_restore_fail_ordinal = scenario == "restore-failure" ? kImportOrdinal : kCloseOrdinal;
        else std::printf("SETUP fail %s\n", error.c_str());
        exit_code = ok ? 0 : 4;
    } else if (scenario == "close-import-slot-mismatch") {
        const std::string path = TempRealCopy("close-import");
        bool copied = ValidateRealIdentity(EXECUTION_PROBE_REAL_XBE, image, error);
        const int64_t offset = copied ? image.Offset(kCloseImportSlot) : -1;
        copied = copied && offset >= 0 && CopyAndPatch(EXECUTION_PROBE_REAL_XBE, path, offset, 0x01);
        xbe::Image patched;
        std::vector<uint8_t> patched_bytes;
        std::string patched_error;
        const bool parsed = copied && ReadFile(path.c_str(), patched_bytes) &&
                            patched.Parse(std::move(patched_bytes), patched_error);
        const bool accepted = parsed && ValidateRealRouteText(patched, patched_error);
        std::remove(path.c_str());
        if (accepted || !parsed) {
            error = accepted ? "patched close import slot was accepted" : "could not prepare the close import copy";
            std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
            exit_code = 5;
        } else {
            error = patched_error.empty() ? "close import slot copy was rejected without a reason" : patched_error;
            std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
            exit_code = 3;
        }
    } else if (scenario == "import-page-writable") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) {
            ok = ProtectRange(kRealImportPage, mem::kPage, mem::kRead | mem::kWrite) &&
                 ValidatePagePermissions(facts, error);
            ProtectRange(kRealImportPage, mem::kPage, mem::kRead);
        }
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "argument-span") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) {
            RealFacts bad = facts;
            bad.import_arg0 = facts.stack_top;
            ok = ValidateImportArguments(bad, bad.import_esp, error);
        }
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else if (scenario == "execute-permission") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) {
            ok = ValidatePagePermissions(facts, error);
            if (ok) {
                const uint32_t page = kEntry & ~(mem::kPage - 1);
                if (ProtectRange(page, mem::kPage, mem::kRead)) ok = ValidatePagePermissions(facts, error);
                else error = "could not remove execute permission";
                ProtectRange(page, mem::kPage, mem::kRead | mem::kExecute);
            }
        }
        if (!ok) std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        exit_code = ok ? 5 : 3;
    } else {
        std::printf("SETUP fail unknown real scenario %s\n", scenario.c_str());
        return 4;
    }

    const bool root_execution = scenario == "execute-root-return";
    const bool execute_success = (scenario == "execute" || root_execution) && exit_code == 0;
    bool teardown_ok = false;
    if (scenario == "preflight" || scenario == "execute-permission" || scenario == "execute" ||
        scenario == "execute-root-return" || scenario == "postcondition-counter" ||
        scenario == "postcondition-result" || scenario == "import-page-writable" || scenario == "argument-span" ||
        scenario == "restore-failure" || scenario == "restore-close-failure") {
        teardown_ok = TeardownReal(options);
    } else {
        teardown_ok = TeardownReal(Options{});
    }
    if (!teardown_ok) {
        std::printf("RESULT fail reason=\"teardown or import restoration failed\"\n");
        exit_code = 5;
    } else if (execute_success) {
        std::printf("REAL_EXEC ok instructions=%u direct_calls=%u kernel_calls=%u import_reads=%u workers=%zu "
                    "last=%08X stop=%08X counter=%08X result=%08X root=%d root_esp=%08X\n",
                    g_probe.executed, g_probe.direct_calls, g_probe.kernel_calls, ImportReads(),
                    kernel::KernelWorkerCountForTest(), root_execution ? kStopBefore : kLastInstruction, kStopBefore,
                    execute_counter, execute_result, int(root_execution), root_execution ? g_probe.root_esp : 0);
    }
    alarm(0);
    return exit_code;
}

constexpr int kSupervisionFd = 3;

bool SupervisionTokenValid() {
    const char* expected = std::getenv("MOSTWANTED_PROBE_SUPERVISION");
    if (!expected || !*expected) return false;
    char token[64] = {};
    const ssize_t count = read(kSupervisionFd, token, sizeof(token) - 1);
    if (count <= 0) return false;
    token[count] = '\0';
    return std::strcmp(token, expected) == 0;
}

std::string SupervisionToken() {
    const uint64_t nonce = uint64_t(Clock::now().time_since_epoch().count()) ^ (uint64_t(getpid()) << 32);
    char token[32];
    std::snprintf(token, sizeof(token), "%016llX", static_cast<unsigned long long>(nonce));
    return token;
}

ChildResult SpawnSupervisedChild(const std::string& self, const std::string& scenario,
                                 std::chrono::milliseconds timeout) {
    int out[2], control[2];
    if (pipe(out) != 0) return {};
    if (pipe(control) != 0) {
        close(out[0]);
        close(out[1]);
        return {};
    }
    const std::string token = SupervisionToken();
    setenv("MOSTWANTED_PROBE_SUPERVISION", token.c_str(), 1);
    const pid_t pid = fork();
    if (pid == 0) {
        close(out[0]);
        close(control[1]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        if (control[0] != kSupervisionFd) {
            dup2(control[0], kSupervisionFd);
            close(control[0]);
        }
        close(out[1]);
        execl(self.c_str(), self.c_str(), "--child", scenario.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    unsetenv("MOSTWANTED_PROBE_SUPERVISION");
    close(out[1]);
    close(control[0]);
    ChildResult result;
    if (pid < 0) {
        close(out[0]);
        close(control[1]);
        return result;
    }
    (void)write(control[1], token.data(), token.size());
    close(control[1]);
    const auto start = Clock::now();
    const auto deadline = start + timeout;
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (Clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(out[0], buffer, sizeof(buffer));
        if (count <= 0) break;
        if (result.output.size() < 65536) result.output.append(buffer, size_t(count));
    }
    close(out[0]);
    result.status = status;
    return result;
}

int RealParent(const std::string& mode, bool execute_real, bool root_return) {
    const std::string self = SelfPath();
    if (self.empty()) {
        std::printf("FAIL: cannot locate the real probe executable\n");
        return 1;
    }
    if (mode == "preflight") {
        const ChildResult result = SpawnChild(self, "preflight", std::chrono::seconds(15));
        if (result.timed_out) {
            std::printf("PREFLIGHT FAIL: timeout\n");
            return 1;
        }
        std::printf("%s", result.output.c_str());
        return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 ? 0 : 1;
    }
    if (mode == "run-once") {
        if (!execute_real) {
            std::printf("REFUSED: pass --execute-real only after explicit approval\n");
            return 2;
        }
        const ChildResult result = SpawnSupervisedChild(self, root_return ? "execute-root-return" : "execute",
                                                       std::chrono::seconds(15));
        std::printf("%s", result.output.c_str());
        if (result.timed_out) {
            std::printf("REAL EXECUTION FAIL: timeout\n");
            return 1;
        }
        return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 ? 0 : 1;
    }

    struct RealCase {
        const char* scenario;
        bool expect_ok;
        const char* expected;
    };
    const RealCase cases[] = {
        {"preflight", true, "PREFLIGHT PASS"},
        {"identity-missing", false, "real XBE is missing"},
        {"identity-mismatch", false, "SHA-1 mismatch"},
        {"route-mismatch", false, "route text mismatch"},
        {"incomplete-hooks", false, "hook coverage"},
        {"unexpected-target", false, "call"},
        {"extra-hook", false, "hook coverage"},
        {"post-import-instruction", false, "post-instruction stop hook"},
        {"overlapping-spans", false, "overlap"},
        {"pointer-uncommitted", false, "not committed"},
        {"invalid-stack", false, "stack"},
        {"execute-permission", false, "not executable"},
        {"import-slot-mismatch", false, "import slot is not the ordinal-255 encoding"},
        {"close-import-slot-mismatch", false, "NtClose ordinal 187"},
        {"import-page-writable", false, "import page is writable"},
        {"argument-span", false, "outside the reviewed guest stack"},
        {"postcondition-count", false, "expected 71 instructions"},
        {"postcondition-branch", false, "entry JNB"},
        {"postcondition-caller-branch", false, "caller JNZ"},
        {"postcondition-close-branch", false, "NtClose JL"},
        {"postcondition-close-status", false, "NtClose status"},
        {"postcondition-post-branch", false, "post-import JNL"},
        {"postcondition-ret-cleanup", false, "did not restore the initial stack"},
        {"postcondition-import", false, "import slot"},
        {"postcondition-counter", false, "counter postcondition"},
        {"postcondition-result", false, "result postcondition"},
        {"postcondition-root-completed", false, "root completion did not return EAX=1"},
        {"postcondition-root-esp", false, "root return did not restore the reviewed host ESP"},
        {"postcondition-root-frame", false, "the root RET had no checked call frame"},
        {"postcondition-root-ret", false, "the root RET was not the last executed instruction"},
        {"postcondition-root-sentinel", false, "the root RET did not read the sentinel slot"},
    };
    int failed = 0;
    for (const RealCase& test : cases) {
        const ChildResult result = SpawnChild(self, test.scenario, std::chrono::seconds(15));
        const int exit_code = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : -1;
        const int expected_exit = test.expect_ok ? 0 : 3;
        const bool condition = !result.timed_out && exit_code == expected_exit &&
                               result.output.find(test.expected) != std::string::npos;
        if (!test.expect_ok)
            std::printf("%s: real child %s rejects with: %s\n", condition ? "PASS" : "FAIL", test.scenario,
                        test.expected);
        else
            std::printf("%s: real child %s completes preflight\n", condition ? "PASS" : "FAIL", test.scenario);
        if (!condition) ++failed;
    }

    const ChildResult unauthorised = SpawnChild(self, "execute", std::chrono::seconds(5));
    const int unauthorised_exit = WIFEXITED(unauthorised.status) ? WEXITSTATUS(unauthorised.status) : -1;
    const bool unauthorised_ok = !unauthorised.timed_out && unauthorised_exit == 2 &&
                                 unauthorised.output.find("REFUSED") != std::string::npos;
    std::printf("%s: direct --child execute is refused without the supervised parent\n",
                unauthorised_ok ? "PASS" : "FAIL");
    if (!unauthorised_ok) ++failed;

    const ChildResult unauthorised_root = SpawnChild(self, "execute-root-return", std::chrono::seconds(5));
    const int unauthorised_root_exit =
        WIFEXITED(unauthorised_root.status) ? WEXITSTATUS(unauthorised_root.status) : -1;
    const bool unauthorised_root_ok = !unauthorised_root.timed_out && unauthorised_root_exit == 2 &&
                                      unauthorised_root.output.find("REFUSED") != std::string::npos;
    std::printf("%s: direct --child execute-root-return is refused without the supervised parent\n",
                unauthorised_root_ok ? "PASS" : "FAIL");
    if (!unauthorised_root_ok) ++failed;

    const ChildResult supervised = SpawnSupervisedChild(self, "supervised-check", std::chrono::seconds(5));
    const int supervised_exit = WIFEXITED(supervised.status) ? WEXITSTATUS(supervised.status) : -1;
    const bool supervised_ok = !supervised.timed_out && supervised_exit == 0 &&
                               supervised.output.find("RESULT ok supervision=1") != std::string::npos;
    std::printf("%s: supervised token/fd pathway is wired without execution\n", supervised_ok ? "PASS" : "FAIL");
    if (!supervised_ok) ++failed;

    // an injected restore failure must fail the process and suppress REAL_EXEC ok
    for (const char* scenario : {"restore-failure", "restore-close-failure"}) {
        const ChildResult restore = SpawnChild(self, scenario, std::chrono::seconds(15));
        const int restore_exit = WIFEXITED(restore.status) ? WEXITSTATUS(restore.status) : -1;
        const bool restore_ok = !restore.timed_out && restore_exit == 5 &&
                                restore.output.find("IMPORT restored=") != std::string::npos &&
                                restore.output.find("ok=0") != std::string::npos &&
                                restore.output.find("RESULT fail reason=\"teardown or import restoration failed\"") !=
                                    std::string::npos &&
                                restore.output.find("REAL_EXEC ok") == std::string::npos;
        std::printf("%s: failed %s restoration exits nonzero without REAL_EXEC ok\n", restore_ok ? "PASS" : "FAIL",
                    scenario);
        if (!restore_ok) ++failed;
    }

    const int missing_flag = RealParent("run-once", false, false);
    std::printf("%s: --run-once without --execute-real is refused\n", missing_flag == 2 ? "PASS" : "FAIL");
    if (missing_flag != 2) ++failed;

    const int root_missing_flag = RealParent("run-once", false, true);
    std::printf("%s: --run-once --root-return without --execute-real is refused\n",
                root_missing_flag == 2 ? "PASS" : "FAIL");
    if (root_missing_flag != 2) ++failed;

    std::printf("real probe preflight: %s\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) {
        const std::string scenario = argv[2];
        if (scenario == "execute" || scenario == "execute-root-return" || scenario == "supervised-check") {
            if (!SupervisionTokenValid()) {
                std::printf("REFUSED: real execution requires the supervised parent\n");
                return 2;
            }
            if (scenario == "execute" || scenario == "execute-root-return") {
                alarm(12);
            } else {
                std::printf("RESULT ok supervision=1\n");
                return 0;
            }
        }
        return RealChild(scenario);
    }
    bool execute_real = false;
    bool root_return = false;
    const char* mode = "self-test";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--execute-real") == 0) execute_real = true;
        else if (std::strcmp(argv[i], "--root-return") == 0) root_return = true;
        else if (std::strcmp(argv[i], "--preflight") == 0) mode = "preflight";
        else if (std::strcmp(argv[i], "--self-test") == 0) mode = "self-test";
        else if (std::strcmp(argv[i], "--run-once") == 0) mode = "run-once";
        else {
            std::printf("usage: %s [--preflight|--self-test|--run-once --execute-real [--root-return]]\n", argv[0]);
            return 2;
        }
    }
    if (root_return && (std::strcmp(mode, "run-once") != 0 || !execute_real)) {
        std::printf("REFUSED: --root-return requires --run-once --execute-real\n");
        return 2;
    }
    return RealParent(mode, execute_real, root_return);
}
