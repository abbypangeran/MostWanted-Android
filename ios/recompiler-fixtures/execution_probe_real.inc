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

// the prepared image's import slot is patched exactly once, to the reviewed
// ordinal-255 thunk, and restored before teardown.
uint32_t g_real_import_slot = 0;
uint32_t g_real_import_original = 0;

uint32_t g_real_worker_baseline = 0;

// test-only injection: leaves a wrong value so the readback below fails
bool g_import_restore_fail_for_test = false;

bool RestoreImportSlot() {
    if (!g_real_import_slot) return true;
    const uint32_t slot = g_real_import_slot;
    const uint32_t original = g_real_import_original;
    g_real_import_slot = 0;  // restored once, even on failure
    bool wrote = ProtectRange(kRealImportPage, mem::kPage, mem::kRead | mem::kWrite);
    if (wrote) wrote = mem::Write(slot, &original, sizeof(original)) == mem::AccessStatus::kOk;
    if (wrote && g_import_restore_fail_for_test) {
        const uint32_t wrong = original ^ 0xFFFFFFFFu;
        mem::Write(slot, &wrong, sizeof(wrong));
    }
    const bool read_only = ProtectRange(kRealImportPage, mem::kPage, mem::kRead);
    uint32_t restored = 0;
    const bool readback =
        mem::Read(slot, &restored, sizeof(restored)) == mem::AccessStatus::kOk && restored == original;
    const bool protected_read_only = mem::CheckAccess(kRealImportPage, 1, mem::kRead) == mem::AccessStatus::kOk &&
                                     mem::CheckAccess(kRealImportPage, 1, mem::kWrite) != mem::AccessStatus::kOk;
    const bool ok = wrote && read_only && readback && protected_read_only;
    std::printf("IMPORT restored=%08X ok=%d read_only=%d\n", restored, int(ok), int(protected_read_only));
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
            if (operands[0].type != ZYDIS_OPERAND_TYPE_MEMORY || operands[0].mem.base != ZYDIS_REGISTER_NONE ||
                operands[0].mem.index != ZYDIS_REGISTER_NONE || operands[0].size != 32 ||
                operands[0].mem.disp.value != int64_t(kImportSlot) || step.pc != kImportCall ||
                step.next != kImportReturn) {
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

    // the helper's RET contract: never executed, but its immediate and return
    // target are reviewed so the stop boundary is meaningful.
    const int64_t ret_offset = image.Offset(kRetInstruction);
    const size_t ret_span = image.FileSpan(kRetInstruction);
    ZydisDecodedInstruction ret_instruction;
    ZydisDecodedOperand ret_operands[ZYDIS_MAX_OPERAND_COUNT];
    if (ret_offset < 0 ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + ret_offset, ret_span, &ret_instruction,
                                             ret_operands)) ||
        ret_instruction.mnemonic != ZYDIS_MNEMONIC_RET || ret_instruction.operand_count_visible != 1 ||
        ret_operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || ret_operands[0].imm.value.u != kRetPopBytes) {
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

    if (FindStep(kStopBefore)) {
        error = "stop PC is part of the reviewed route";
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
    if (step_total != kRouteCount || stop_total != 1 || header_steps != kRouteCount || header_stops != 1) {
        error = "generated hook counts are not exactly the reviewed set";
        return false;
    }
    const size_t step_call = source.find("ProbeStep_0x002B5D11(ctx");
    const size_t stop_call = source.find("ProbeStop_0x002B5D11(ctx");
    const std::string import_call =
        "x86_checked_indirect_call(ctx, base, 0x3C9E68u, 0x2B5CE6u, 0x2B5CE0u)";
    const size_t import_at = source.find(import_call);
    if (import_at == std::string::npos || CountOccurrences(source, "x86_checked_indirect_call") != 1 ||
        CountOccurrences(source, "0x3C9E68u") != 1) {
        error = "generated source does not contain exactly the reviewed import call";
        return false;
    }
    if (stop_call < import_at) {
        error = "stop hook is not after the checked import call";
        return false;
    }
    // the function must end at pop ebp: no instruction, label or RET beyond it
    if (source.find("// 002B5D12") != std::string::npos || source.find("loc_002B5D12") != std::string::npos ||
        source.find("ret 0x18") != std::string::npos) {
        error = "generated source contains the unreviewed RET";
        return false;
    }
    for (const char* forbidden : {"x86_icall", "x86_kernel", "x86_resume"}) {
        if (source.find(forbidden) != std::string::npos) {
            error = std::string("generated source contains ") + forbidden;
            return false;
        }
    }
    if (step_call == std::string::npos || stop_call == std::string::npos || step_call > stop_call) {
        error = "stop hook is not after the last reviewed instruction";
        return false;
    }
    if (source.find("X86_FUNC(__imp__sub_002B5CEC)") == std::string::npos ||
        source.find("ProbeStep_0x002B5CEC(ctx") == std::string::npos) {
        error = "debug boundary function is missing";
        return false;
    }
    if (source.find("X86_FUNC(__imp__sub_002B3826)") != std::string::npos ||
        source.find("x86_checked_call(ctx, base, 0x2B3826") != std::string::npos) {
        error = "the debug arm's callee was compiled";
        return false;
    }
    const size_t direct_calls = CountOccurrences(source, "x86_checked_call(ctx, base, 0x2B5CA7");
    const size_t functions = CountOccurrences(source, "X86_FUNC(__imp__sub_");
    if (direct_calls != 1 || functions != 3) {
        error = "generated source has an unexpected call or function set";
        return false;
    }
    if (source.find("X86_FUNC(__imp__sub_002B3916)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_002B5CA7)") == std::string::npos) {
        error = "generated source does not contain exactly the reviewed functions";
        return false;
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
    if (mem::CheckAccess(kRealImportPage, 4, mem::kRead) != mem::AccessStatus::kOk) {
        error = "the ordinal-255 import page is not readable";
        return false;
    }
    if (mem::CheckAccess(kRealImportPage, 1, mem::kWrite) == mem::AccessStatus::kOk) {
        error = "the ordinal-255 import page is writable";
        return false;
    }
    if (mem::CheckAccess(kImportSlot, 4, mem::kExecute) == mem::AccessStatus::kOk) {
        error = "the ordinal-255 import page is executable";
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
    if (!ValidateStackSpan(facts, facts.import_esp, kImportArgumentBytes, "import argument block", error))
        return false;
    if (!ValidateStackSpan(facts, facts.import_arg0, 4, "import handle output", error)) return false;
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
    if (X86FuncMappingCount != 3 || X86ContinuationCount != 0) {
        error = "generated real mapping does not contain the reviewed functions and the debug boundary";
        return false;
    }
    bool entry = false, helper = false, debug_boundary = false;
    for (size_t i = 0; i < X86FuncMappingCount; ++i) {
        if (X86FuncMappings[i].guest == kEntry) entry = true;
        else if (X86FuncMappings[i].guest == kCallTarget) helper = true;
        else if (X86FuncMappings[i].guest == kDebugPath) debug_boundary = true;
        else {
            error = "generated real mapping contains an unexpected function";
            return false;
        }
    }
    if (!entry || !helper || !debug_boundary) {
        error = "generated real mapping is missing a reviewed function or the debug boundary";
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
        error = "could not open the ordinal-255 import page for its reviewed patch";
        return false;
    }
    uint32_t authentic_import = 0;
    if (mem::Read(kImportSlot, &authentic_import, sizeof(authentic_import)) != mem::AccessStatus::kOk ||
        authentic_import != kImportEncoding) {
        error = "import slot is not the authentic ordinal-255 encoding";
        return false;
    }
    const uint32_t import_target = dispatch::KernelThunkAddress(kImportOrdinal);
    if (!import_target || mem::Write(kImportSlot, &import_target, sizeof(import_target)) != mem::AccessStatus::kOk) {
        error = "could not patch the import slot to the reviewed ordinal-255 thunk";
        return false;
    }
    if (!ProtectRange(kRealImportPage, mem::kPage, mem::kRead)) {
        error = "could not protect the ordinal-255 import page read-only";
        return false;
    }
    g_real_import_slot = kImportSlot;
    g_real_import_original = authentic_import;
    facts.import_authentic = authentic_import;
    facts.import_target = import_target;
    g_real_worker_baseline = kernel::ThreadWorkerLaunchCountForTest();
    if (!PrepareKernelBridge(true, error)) return false;

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
    std::printf("PREFLIGHT route: 57/57 instructions matched, stop=%08X, import=%08X:%08X\n", kStopBefore,
                kImportSlot, kImportEncoding);
    std::printf("PREFLIGHT mapping: functions=3 (2 reviewed + debug boundary) continuations=0 hooks=57 "
                "stop_hooks=1\n");
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
    std::printf("PREFLIGHT args: handle=%08X ext=%08X stack=%08X tls=%08X id=%08X start=%08X context=%08X "
                "suspended=%08X debug=%08X system=%08X\n",
                facts.import_args[0], facts.import_args[1], facts.import_args[2], facts.import_args[3],
                facts.import_args[4], facts.import_args[5], facts.import_args[6], facts.import_args[7],
                facts.import_args[8], facts.import_args[9]);
    std::printf("PREFLIGHT pages: entry=RX helper=RX header=R rdata=R data=RW import=R\n");
    std::printf("PREFLIGHT continuation: status=0 jnl=%08X taken debug=%08X forbidden ret=%08X target=%08X pop=%X\n",
                kPostImportBranch, kDebugPath, kRetInstruction, kRetTarget, kRetPopBytes);
    std::printf("PREFLIGHT limits: instructions=64 direct_calls=1 kernel_calls=1 memory=256 imports=1 callbacks=0 "
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
    for (const StepRecord& step : g_probe.trace) {
        if (step.pc == 0x002B392A) entry_branch = &step;
        if (step.pc == 0x002B5CAF) helper_branch = &step;
        if (step.pc == 0x002B5CD9) helper_value = &step;
        if (step.pc == 0x002B3956) idiv_result = &step;
        if (step.pc == kImportReturn) kernel_status = &step;
        if (step.pc == kPostImportBranch) post_branch = &step;
    }
    if (g_probe.executed != 55) {
        error = "expected 55 instructions, got " + std::to_string(g_probe.executed);
        return false;
    }
    if (g_probe.direct_calls != 1) {
        error = "expected one direct call, got " + std::to_string(g_probe.direct_calls);
        return false;
    }
    if (g_probe.kernel_calls != 1) {
        error = "expected one kernel call, got " + std::to_string(g_probe.kernel_calls);
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
    const MemoryRecord* import = nullptr;
    unsigned import_records = 0;
    for (const MemoryRecord& record : g_probe.memory) {
        if (!InImportPage(record.address, uint64_t(record.address) + record.size)) continue;
        import = &record;
        ++import_records;
    }
    if (ImportReads() != 1 || g_probe.import_reads != 1 || import_records != 1) {
        error = "ordinal-255 import slot read count is not one";
        return false;
    }
    if (import->pc != kImportCall || import->address != kImportSlot || import->size != 4 || import->write) {
        error = "the import operand read is not the reviewed call";
        return false;
    }
    if (kernel::KernelWorkerCountForTest() != 0 ||
        kernel::ThreadWorkerLaunchCountForTest() != g_probe.worker_launch_baseline) {
        error = "a worker was activated";
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
    if (!g_probe.have_stop || g_probe.stop.pc != kLastInstruction) {
        error = "mandatory stop was not after 002B5D11";
        return false;
    }
    if (g_probe.stop.esp != facts.import_esp + 0x2C) {
        error = "post-import ESP does not match pop ebp";
        return false;
    }
    if (g_probe.stop.ecx != 0 || g_probe.stop.ebp != 0) {
        error = "post-import register state mismatch";
        return false;
    }
    uint32_t handle = 0;
    if (mem::Read(facts.import_args[0], &handle, sizeof(handle)) != mem::AccessStatus::kOk || !handle ||
        g_probe.stop.eax != handle) {
        error = "the helper did not return the published thread handle";
        return false;
    }
    unsigned handle_reads = 0;
    for (const MemoryRecord& record : g_probe.memory)
        if (!record.write && record.pc == kHandleRead && record.address == facts.import_args[0] && record.size == 4)
            ++handle_reads;
    if (handle_reads != 1) {
        error = "the handle output was not read exactly once";
        return false;
    }
    uint32_t ret_target = 0;
    if (mem::Read(g_probe.stop.esp, &ret_target, sizeof(ret_target)) != mem::AccessStatus::kOk ||
        ret_target != kRetTarget) {
        error = "the reviewed RET target is not on the stack at the stop";
        return false;
    }
    if (g_kernel255_calls != 1 || g_kernel255_esp != facts.import_esp - 4 ||
        g_kernel255_return != facts.import_return) {
        error = "ordinal-255 call frame mismatch";
        return false;
    }
    if (g_kernel255_args != facts.import_args) {
        error = "ordinal-255 arguments do not match the authentic preflight values";
        return false;
    }
    if (!g_prepared_thread || g_prepared_thread->state.load() != kernel::ThreadState::kPrepared) {
        error = "no inactive thread was prepared";
        return false;
    }
    if (g_prepared_thread->start_routine != facts.import_args[5] ||
        g_prepared_thread->start_context != facts.import_args[6] ||
        g_prepared_thread->system_routine != facts.import_args[9] || g_prepared_thread->suspend.load() != 0) {
        error = "the prepared thread does not carry the authentic routines";
        return false;
    }
    if (kernel::FromHandle(handle).get() != g_prepared_thread.get()) {
        error = "the returned handle does not name the prepared thread";
        return false;
    }
    if (!kernel::Threads().empty()) {
        error = "a guest thread became active";
        return false;
    }
    return true;
}

void FakeRealTrace(const RealFacts& facts, bool entry_taken, bool helper_taken) {
    g_probe = ProbeState{};
    g_probe.executed = 55;
    g_probe.direct_calls = 1;
    g_probe.kernel_calls = 1;
    g_probe.import_reads = 1;
    g_probe.worker_launch_baseline = kernel::ThreadWorkerLaunchCountForTest();
    g_probe.memory.push_back({kImportCall, kImportSlot, 4, false});
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
    StepRecord last{};
    last.pc = kLastInstruction;
    g_probe.trace.push_back(last);
    g_probe.stop = last;
    g_probe.stop.eax = kernel::kSuccess;
    g_probe.stop.ecx = 0;
    g_probe.stop.ebp = 0;
    g_probe.stop.esp = facts.import_esp + 0x2C;
    g_probe.have_stop = true;
}

// restores the authentic import slot before releasing the image; a failed
// restore must fail the process, so it is propagated, not printed and dropped
bool TeardownReal(const Options& options) {
    const bool restored = RestoreImportSlot();
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
    const bool prepared = handle_read && handle && kernel::FromHandle(handle).get() == thread.get() &&
                          kernel::Threads().empty() &&
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
                    kernel::Threads().empty() && resources;
    std::printf("CANCEL prepared=%d cancelled=%d output_cleared=%d stack_freed=%d resources=%d workers=%zu\n",
                int(prepared), int(cancelled), int(cleared_ok), int(stack_freed), int(resources),
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

    if (scenario == "execute") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (!ok) {
            std::printf("SETUP fail %s\n", error.c_str());
            exit_code = 4;
        } else {
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
                source += "\nloc_002B5D12:;\n";
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
               scenario == "postcondition-post-branch" || scenario == "postcondition-import" ||
               scenario == "postcondition-counter" || scenario == "postcondition-result") {
        const bool needs_image = scenario == "postcondition-counter" || scenario == "postcondition-result";
        if (needs_image) ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        else {
            facts = RealFacts{};
            facts.entry_taken = true;
            ok = true;
        }
        if (ok) {
            facts.entry_taken = true;
            FakeRealTrace(facts, true, false);
            if (scenario == "postcondition-count") g_probe.executed = 54;
            else if (scenario == "postcondition-branch") g_probe.trace[0].branch_taken = false;
            else if (scenario == "postcondition-post-branch") {
                for (StepRecord& step : g_probe.trace)
                    if (step.pc == kPostImportBranch) step.branch_taken = false;
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
    } else if (scenario == "restore-failure") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) g_import_restore_fail_for_test = true;
        else std::printf("SETUP fail %s\n", error.c_str());
        exit_code = ok ? 0 : 4;
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

    const bool execute_success = scenario == "execute" && exit_code == 0;
    bool teardown_ok = false;
    if (scenario == "preflight" || scenario == "execute-permission" || scenario == "execute" ||
        scenario == "postcondition-counter" || scenario == "postcondition-result" ||
        scenario == "import-page-writable" || scenario == "argument-span" || scenario == "restore-failure") {
        teardown_ok = TeardownReal(options);
    } else {
        teardown_ok = TeardownReal(Options{});
    }
    if (!teardown_ok) {
        std::printf("RESULT fail reason=\"teardown or import restoration failed\"\n");
        exit_code = 5;
    } else if (execute_success) {
        std::printf("REAL_EXEC ok instructions=%u direct_calls=%u kernel_calls=%u import_reads=%u workers=%zu "
                    "last=%08X stop=%08X counter=%08X result=%08X\n",
                    g_probe.executed, g_probe.direct_calls, g_probe.kernel_calls, ImportReads(),
                    kernel::KernelWorkerCountForTest(), kLastInstruction, kStopBefore, execute_counter,
                    execute_result);
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

int RealParent(const std::string& mode, bool execute_real) {
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
        const ChildResult result = SpawnSupervisedChild(self, "execute", std::chrono::seconds(15));
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
        {"post-import-instruction", false, "unreviewed RET"},
        {"overlapping-spans", false, "overlap"},
        {"pointer-uncommitted", false, "not committed"},
        {"invalid-stack", false, "stack"},
        {"execute-permission", false, "not executable"},
        {"import-slot-mismatch", false, "import slot is not the ordinal-255 encoding"},
        {"import-page-writable", false, "import page is writable"},
        {"argument-span", false, "outside the reviewed guest stack"},
        {"postcondition-count", false, "expected 55 instructions"},
        {"postcondition-branch", false, "entry JNB"},
        {"postcondition-post-branch", false, "post-import JNL"},
        {"postcondition-import", false, "import slot"},
        {"postcondition-counter", false, "counter postcondition"},
        {"postcondition-result", false, "result postcondition"},
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

    const ChildResult supervised = SpawnSupervisedChild(self, "supervised-check", std::chrono::seconds(5));
    const int supervised_exit = WIFEXITED(supervised.status) ? WEXITSTATUS(supervised.status) : -1;
    const bool supervised_ok = !supervised.timed_out && supervised_exit == 0 &&
                               supervised.output.find("RESULT ok supervision=1") != std::string::npos;
    std::printf("%s: supervised token/fd pathway is wired without execution\n", supervised_ok ? "PASS" : "FAIL");
    if (!supervised_ok) ++failed;

    // an injected restore failure must fail the process and suppress REAL_EXEC ok
    const ChildResult restore = SpawnChild(self, "restore-failure", std::chrono::seconds(15));
    const int restore_exit = WIFEXITED(restore.status) ? WEXITSTATUS(restore.status) : -1;
    const bool restore_ok = !restore.timed_out && restore_exit == 5 &&
                            restore.output.find("IMPORT restored=") != std::string::npos &&
                            restore.output.find("ok=0") != std::string::npos &&
                            restore.output.find("RESULT fail reason=\"teardown or import restoration failed\"") !=
                                std::string::npos &&
                            restore.output.find("REAL_EXEC ok") == std::string::npos;
    std::printf("%s: failed import restoration exits nonzero without REAL_EXEC ok\n", restore_ok ? "PASS" : "FAIL");
    if (!restore_ok) ++failed;

    const int missing_flag = RealParent("run-once", false);
    std::printf("%s: --run-once without --execute-real is refused\n", missing_flag == 2 ? "PASS" : "FAIL");
    if (missing_flag != 2) ++failed;

    std::printf("real probe preflight: %s\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) {
        const std::string scenario = argv[2];
        if (scenario == "execute" || scenario == "supervised-check") {
            if (!SupervisionTokenValid()) {
                std::printf("REFUSED: real execution requires the supervised parent\n");
                return 2;
            }
            if (scenario == "execute") {
                alarm(12);
            } else {
                std::printf("RESULT ok supervision=1\n");
                return 0;
            }
        }
        return RealChild(scenario);
    }
    bool execute_real = false;
    const char* mode = "self-test";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--execute-real") == 0) execute_real = true;
        else if (std::strcmp(argv[i], "--preflight") == 0) mode = "preflight";
        else if (std::strcmp(argv[i], "--self-test") == 0) mode = "self-test";
        else if (std::strcmp(argv[i], "--run-once") == 0) mode = "run-once";
        else {
            std::printf("usage: %s [--preflight|--self-test|--run-once --execute-real]\n", argv[0]);
            return 2;
        }
    }
    return RealParent(mode, execute_real);
}
