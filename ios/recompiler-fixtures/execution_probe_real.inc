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

struct RealFacts {
    uint32_t base = 0, image_size = 0, header_size = 0, sections = 0;
    uint32_t g10108 = 0, g10118 = 0, g10130 = 0, g10184 = 0;
    uint32_t g3ec = 0, g3f0 = 0, g3f4 = 0, g3fc = 0, g4edc74 = 0;
    uint32_t p = 0, p_value = 0;
    uint32_t entry_ea = 0;
    bool entry_taken = false;
    bool p_write_required = false;
    uint32_t idiv_dividend = 0, idiv_quotient = 0, idiv_remainder = 0;
    uint32_t q = 0, q_before = 0, q_after = 0;
    uint32_t stack_base = 0, stack_top = 0, esp = 0;
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
        }
    }

    if (route_bytes != kExpectedRouteBytes || route_hash != kExpectedRouteHash) {
        error = "route byte hash mismatch";
        return false;
    }

    const int64_t stop_offset = image.Offset(kStopBefore);
    const size_t stop_span = image.FileSpan(kStopBefore);
    ZydisDecodedInstruction stop_instruction;
    ZydisDecodedOperand stop_operands[ZYDIS_MAX_OPERAND_COUNT];
    if (stop_offset < 0 ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + stop_offset, stop_span, &stop_instruction,
                                             stop_operands)) ||
        stop_instruction.mnemonic != ZYDIS_MNEMONIC_CALL || stop_operands[0].type != ZYDIS_OPERAND_TYPE_MEMORY ||
        stop_operands[0].mem.disp.value != int64_t(kRealImportSlot)) {
        error = "stop instruction is not the expected import call";
        return false;
    }
    uint32_t import = 0;
    if (!image.Read32(kRealImportSlot, import) || import != kImportEncoding) {
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
    const size_t step_call = source.find("ProbeStep_0x002B5CDF(ctx");
    const size_t stop_call = source.find("ProbeStop_0x002B5CDF(ctx");
    if (step_call == std::string::npos || stop_call == std::string::npos || step_call > stop_call) {
        error = "stop hook is not after the last reviewed instruction";
        return false;
    }
    const size_t stop_pc = source.find("2B5CE0");
    if (stop_pc != std::string::npos && stop_call > stop_pc) {
        error = "stop hook is not before the forbidden import call";
        return false;
    }
    if (source.find("3C9E68") != std::string::npos) {
        error = "generated source contains the forbidden import slot";
        return false;
    }
    for (const char* forbidden : {"x86_checked_indirect_call", "x86_icall", "x86_kernel", "x86_resume"}) {
        if (source.find(forbidden) != std::string::npos) {
            error = std::string("generated source contains ") + forbidden;
            return false;
        }
    }
    const size_t direct_calls = CountOccurrences(source, "x86_checked_call(ctx, base, 0x2B5CA7");
    const size_t functions = CountOccurrences(source, "X86_FUNC(__imp__sub_");
    if (direct_calls != 1 || functions != 2) {
        error = "generated source has an unexpected call or function set";
        return false;
    }
    if (source.find("X86_FUNC(__imp__sub_002B3916)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_002B5CA7)") == std::string::npos) {
        error = "generated source does not contain exactly the two reviewed functions";
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
    if (mem::CheckAccess(kRealImportPage, 4, mem::kRead) == mem::AccessStatus::kOk) {
        error = "the ordinal-255 import page is readable";
        return false;
    }
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
    if (X86FuncMappingCount != 2 || X86ContinuationCount != 0) {
        error = "generated real mapping does not contain exactly two functions and no continuations";
        return false;
    }
    bool entry = false, helper = false;
    for (size_t i = 0; i < X86FuncMappingCount; ++i) {
        if (X86FuncMappings[i].guest == kEntry) entry = true;
        else if (X86FuncMappings[i].guest == kCallTarget) helper = true;
        else {
            error = "generated real mapping contains an unexpected function";
            return false;
        }
    }
    if (!entry || !helper) {
        error = "generated real mapping is missing a reviewed function";
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
    if (!ProtectRange(kRealImportPage, mem::kPage, mem::kNoAccess)) {
        error = "could not protect the ordinal-255 import page";
        return false;
    }

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

    options = Options{};
    options.start_pc = kEntry;
    options.initial_esp = facts.esp;
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
    std::printf("PREFLIGHT route: 50/50 instructions matched, stop=%08X, import=%08X:%08X\n", kStopBefore,
                kRealImportSlot, kImportEncoding);
    std::printf("PREFLIGHT mapping: functions=2 continuations=0 hooks=50 stop_hooks=1\n");
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
    std::printf("PREFLIGHT stack: base=%08X top=%08X esp=%08X requested=%X committed=%X\n", facts.stack_base,
                facts.stack_top, facts.esp, kRealStackRequest, kRealStackCommitted);
    std::printf("PREFLIGHT pages: entry=RX helper=RX header=R rdata=R data=RW import=NOACCESS\n");
    std::printf("PREFLIGHT limits: instructions=64 direct_calls=1 memory=256 imports=0 callbacks=0 workers=0\n");
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

    if (scenario == "execute") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) {
            const Report report = RunProbe(options);
            if (report.failure) {
                PrintFailure(report.error);
                exit_code = 3;
            } else {
                std::printf("REAL_EXEC ok instructions=%u direct_calls=%u import_reads=%u workers=%zu stop=%08X\n",
                            g_probe.executed, g_probe.direct_calls, ImportReads(),
                            kernel::KernelWorkerCountForTest(), kStopBefore);
                exit_code = 0;
            }
        } else {
            std::printf("SETUP fail %s\n", error.c_str());
            exit_code = 4;
        }
    } else if (scenario == "preflight") {
        ok = PrepareRealImage(EXECUTION_PROBE_REAL_XBE, image, options, facts, error);
        if (ok) PrintRealFacts(facts);
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
               scenario == "pre-import-stop") {
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
                const size_t at = source.find("ProbeStop_0x002B5CDF(ctx, base);");
                if (at != std::string::npos) source.insert(at, "3C9E68 ");
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

    if (scenario == "preflight" || scenario == "execute-permission" || scenario == "execute") {
        if (!Teardown(options)) exit_code = 5;
    } else {
        if (!Teardown(Options{})) exit_code = 5;
    }
    return exit_code;
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
        const ChildResult result = SpawnChild(self, "execute", std::chrono::seconds(15));
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
        {"overlapping-spans", false, "overlap"},
        {"pointer-uncommitted", false, "not committed"},
        {"invalid-stack", false, "stack"},
        {"execute-permission", false, "not executable"},
        {"pre-import-stop", false, "forbidden import slot"},
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
    std::printf("real probe preflight: %s\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return RealChild(argv[2]);
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
