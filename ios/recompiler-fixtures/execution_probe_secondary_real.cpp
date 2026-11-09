// Phase 2C-33 real-XBE secondary-thread SEH/TLS probe.
//
// Separate executable and generated mapping from the Phase 2C-30 root probe.
// The preflight prepares the authentic image and a fresh inactive thread, then
// validates identity, route, mapping, permissions, thread context, callback
// ticket and expected SEH/TLS facts.  It never executes a guest instruction.
// The supervised execute path is implemented but requires the parent's token
// and is not run in this phase.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <Zydis/Zydis.h>

#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "execution_probe_secondary_route.h"
#include "kernel/crypto.h"
#include "kernel/guest_callback.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/worker_lifecycle.h"
#include "xbe/image_loader.h"
#include "xbe/xbe.h"
#include "x86_recomp_shared.h"

namespace kernel {
uint8_t* g_base = nullptr;
}

namespace {

using namespace execution_probe_secondary;
using Clock = std::chrono::steady_clock;

constexpr const char* kRealSha1 = "f636a16af04171089f879a4bed646e185c734233";
constexpr uint32_t kRealEntry = 0x002B3916;
constexpr uint32_t kRealTitle = 0x4541007B;
constexpr uint32_t kRealImageBase = 0x00010000;
constexpr uint32_t kOutputPage = 0x60000000;  // probe-owned, outside the image
constexpr uint32_t kRealHandleOut = kOutputPage;
constexpr uint32_t kRealIdOut = kOutputPage + 8;
constexpr int kSupervisionFd = 3;
constexpr uint32_t kMaximumPrintedDiffs = 32;

int failures = 0;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

struct ProbeFailure {
    std::string reason;
    uint32_t pc = 0;
    uint32_t address = 0;
    bool has_address = false;
};

struct ProbeNormalStop {};

struct MemoryRecord {
    uint32_t pc;
    uint32_t address;
    uint32_t size;
    bool write;
};

struct StepRecord {
    uint32_t pc;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t cf, zf, sf, of, pf, af;
    bool branch_valid = false;
    bool branch_taken = false;
};

struct RealFacts {
    uint32_t base = 0, image_size = 0, header_size = 0, sections = 0;
    uint32_t tls_source = 0, tls_end = 0, tls_zero = 0, tls_copy = 0;
    uint32_t stack_base = 0, stack_top = 0, initial_esp = 0, entry_esp = 0;
    uint32_t stop_esp = 0, stop_ebp = 0, registration = 0;
    uint32_t kpcr = 0, ethread = 0, tls = 0, fs0 = 0;
    uint32_t handle = 0, id = 0;
    uint32_t worker_baseline = 0;
    std::vector<uint32_t> snapshot_pages;
};

struct ProbeState {
    bool running = false;
    bool in_step = false;
    bool first_step = true;
    bool have_previous = false;
    uint32_t previous_pc = 0;
    uint32_t executed = 0;
    uint32_t direct_calls = 0;
    uint32_t kernel_calls = 0;
    uint32_t import_reads = 0;
    uint32_t worker_baseline = 0;
    uint32_t instruction_budget = kMaximumInstructions;
    uint32_t stack_base = 0;
    uint32_t stack_top = 0;
    uint32_t kpcr = 0;
    uint32_t ethread = 0;
    uint32_t tls = 0;
    Clock::time_point deadline{};
    bool deadline_set = false;
    bool corrupt_helper_return = false;
    std::vector<StepRecord> trace;
    std::vector<MemoryRecord> memory;
    StepRecord stop{};
    bool have_stop = false;
    bool stop_frame_clear = false;
    std::vector<uint32_t> snapshot_pages;
};

ProbeState g_probe;

const RouteStep* FindStep(uint32_t pc) {
    for (const RouteStep& step : kRoute)
        if (step.pc == pc) return &step;
    return nullptr;
}

[[noreturn]] void Fail(const char* reason, uint32_t pc, uint32_t address = 0, bool has_address = false) {
    ProbeFailure failure;
    failure.reason = reason;
    failure.pc = pc;
    failure.address = address;
    failure.has_address = has_address;
    throw failure;
}

struct InStep {
    InStep() { g_probe.in_step = true; }
    ~InStep() { g_probe.in_step = false; }
};

void RecordStep(uint32_t pc, X86Context& ctx, const RouteStep& step) {
    StepRecord record{};
    record.pc = pc;
    record.eax = ctx.eax;
    record.ecx = ctx.ecx;
    record.edx = ctx.edx;
    record.ebx = ctx.ebx;
    record.esp = ctx.esp;
    record.ebp = ctx.ebp;
    record.esi = ctx.esi;
    record.edi = ctx.edi;
    record.cf = ctx.cf;
    record.zf = ctx.zf;
    record.sf = ctx.sf;
    record.of = ctx.of;
    record.pf = ctx.pf;
    record.af = ctx.af;
    if (step.kind == StepKind::kConditional) {
        record.branch_valid = true;
        record.branch_taken = ctx.zf != 0;
    }
    g_probe.trace.push_back(record);
}

void SecondaryStep(uint32_t pc, X86Context& ctx) {
    if (!g_probe.running) Fail("instruction step outside a probe run", pc);
    if (g_probe.in_step) Fail("re-entry during a step", pc);
    InStep guard;

    if (pc == kStartRoutine) Fail("start routine entry is forbidden", pc);
    if (pc == kStackHelper) Fail("stack helper entry is forbidden", pc);
    if (pc == kNextHelper) Fail("next helper entry is forbidden", pc);
    if (pc == kSehHandler) Fail("exception handler entry is forbidden", pc);

    const RouteStep* step = FindStep(pc);
    if (!step) Fail("instruction PC is not in the reviewed route", pc);
    if (g_probe.deadline_set && Clock::now() > g_probe.deadline) Fail("instruction deadline exceeded", pc);
    if (mem::CheckAccess(pc, step->length, mem::kExecute) != mem::AccessStatus::kOk)
        Fail("guest code page is not executable", pc);

    if (g_probe.first_step) {
        if (pc != kEntry) Fail("first instruction is not the reviewed entry", pc);
        g_probe.first_step = false;
    } else {
        const RouteStep* previous = FindStep(g_probe.previous_pc);
        const bool next_edge = previous && pc == previous->next;
        const bool alternate_edge = previous && previous->kind == StepKind::kConditional &&
                                    pc == previous->alternate;
        if (!next_edge && !alternate_edge) Fail("unexpected control transfer", pc);
        if (previous && previous->kind == StepKind::kConditional && !g_probe.trace.empty()) {
            const bool taken = alternate_edge;
            if (g_probe.trace.back().branch_taken != taken) Fail("branch outcome mismatch", pc);
        }
    }

    if (step->kind == StepKind::kCall) {
        if (g_probe.direct_calls >= kMaximumDirectCalls) Fail("direct call budget exhausted", pc);
        const dispatch::ResolvedTarget target = dispatch::ResolveTarget(step->next);
        if (target.kind != dispatch::TargetKind::kGeneratedFunction || !target.function)
            Fail("direct call target is not a reviewed generated function", pc);
        if (mem::CheckAccess(step->next, 1, mem::kExecute) != mem::AccessStatus::kOk)
            Fail("direct call target page is not executable", pc);
        ++g_probe.direct_calls;
    }

    if (step->kind == StepKind::kRet) {
        if (ctx.checked_call == nullptr) Fail("checked RET has no active call frame", pc);
        if (ctx.checked_call->return_address != kSehReturn)
            Fail("checked RET frame does not expect the reviewed return", pc);
    }

    if (pc == kSehHelper && g_probe.corrupt_helper_return) {
        const uint32_t wrong = 0xDEADBEEF;
        if (mem::Write(ctx.esp, &wrong, sizeof(wrong)) != mem::AccessStatus::kOk)
            Fail("could not corrupt the helper return address", pc, ctx.esp, true);
    }

    if (step->kind == StepKind::kStop) {
        if (g_probe.direct_calls != kMaximumDirectCalls) Fail("stop reached without the reviewed call", pc);
        if (!kernel::Threads().empty()) Fail("a guest thread became active", pc);
        if (kernel::ThreadWorkerLaunchCountForTest() != g_probe.worker_baseline)
            Fail("a kernel worker was activated", pc);
        g_probe.stop.pc = pc;
        g_probe.stop.eax = ctx.eax;
        g_probe.stop.ecx = ctx.ecx;
        g_probe.stop.edx = ctx.edx;
        g_probe.stop.ebx = ctx.ebx;
        g_probe.stop.esp = ctx.esp;
        g_probe.stop.ebp = ctx.ebp;
        g_probe.stop.esi = ctx.esi;
        g_probe.stop.edi = ctx.edi;
        g_probe.stop.cf = ctx.cf;
        g_probe.stop.zf = ctx.zf;
        g_probe.stop.sf = ctx.sf;
        g_probe.stop.of = ctx.of;
        g_probe.stop.pf = ctx.pf;
        g_probe.stop.af = ctx.af;
        g_probe.stop_frame_clear = ctx.checked_call == nullptr;
        g_probe.have_stop = true;
        throw ProbeNormalStop{};
    }

    if (g_probe.executed >= g_probe.instruction_budget) Fail("instruction budget exhausted", pc);
    RecordStep(pc, ctx, *step);
    ++g_probe.executed;
    g_probe.previous_pc = pc;
    g_probe.have_previous = true;
}

}  // namespace

void SecondaryStep_0x002B5C0F(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C0F, ctx); }
void SecondaryStep_0x002B5C11(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C11, ctx); }
void SecondaryStep_0x002B5C16(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C16, ctx); }
void SecondaryStep_0x00319804(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319804, ctx); }
void SecondaryStep_0x00319809(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319809, ctx); }
void SecondaryStep_0x0031980F(X86Context& ctx, uint8_t*) { SecondaryStep(0x0031980F, ctx); }
void SecondaryStep_0x00319810(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319810, ctx); }
void SecondaryStep_0x00319814(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319814, ctx); }
void SecondaryStep_0x00319818(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319818, ctx); }
void SecondaryStep_0x0031981C(X86Context& ctx, uint8_t*) { SecondaryStep(0x0031981C, ctx); }
void SecondaryStep_0x0031981E(X86Context& ctx, uint8_t*) { SecondaryStep(0x0031981E, ctx); }
void SecondaryStep_0x0031981F(X86Context& ctx, uint8_t*) { SecondaryStep(0x0031981F, ctx); }
void SecondaryStep_0x00319820(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319820, ctx); }
void SecondaryStep_0x00319821(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319821, ctx); }
void SecondaryStep_0x00319824(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319824, ctx); }
void SecondaryStep_0x00319827(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319827, ctx); }
void SecondaryStep_0x00319828(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319828, ctx); }
void SecondaryStep_0x0031982B(X86Context& ctx, uint8_t*) { SecondaryStep(0x0031982B, ctx); }
void SecondaryStep_0x00319832(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319832, ctx); }
void SecondaryStep_0x00319835(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319835, ctx); }
void SecondaryStep_0x00319838(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319838, ctx); }
void SecondaryStep_0x0031983E(X86Context& ctx, uint8_t*) { SecondaryStep(0x0031983E, ctx); }
void SecondaryStep_0x002B5C1B(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C1B, ctx); }
void SecondaryStep_0x002B5C1F(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C1F, ctx); }
void SecondaryStep_0x002B5C25(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C25, ctx); }
void SecondaryStep_0x002B5C28(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C28, ctx); }
void SecondaryStep_0x002B5C2B(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C2B, ctx); }
void SecondaryStep_0x002B5C2E(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C2E, ctx); }
void SecondaryStep_0x002B5C31(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C31, ctx); }
void SecondaryStep_0x002B5C34(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C34, ctx); }
void SecondaryStep_0x002B5C3A(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C3A, ctx); }
void SecondaryStep_0x002B5C40(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C40, ctx); }
void SecondaryStep_0x002B5C42(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C42, ctx); }
void SecondaryStep_0x002B5C45(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C45, ctx); }
void SecondaryStep_0x002B5C47(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C47, ctx); }
void SecondaryStep_0x002B5C49(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C49, ctx); }
void SecondaryStep_0x002B5C4B(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C4B, ctx); }
void SecondaryStep_0x002B5C4E(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C4E, ctx); }
void SecondaryStep_0x002B5C50(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C50, ctx); }
void SecondaryStep_0x002B5C52(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C52, ctx); }
void SecondaryStep_0x002B5C55(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C55, ctx); }
void SecondaryStep_0x002B5C57(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C57, ctx); }
void SecondaryStep_0x002B5C5D(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C5D, ctx); }
void SecondaryStep_0x002B5C5F(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C5F, ctx); }
void SecondaryStep_0x002B5C61(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C61, ctx); }
void SecondaryStep_0x002B5C63(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C63, ctx); }
void SecondaryStep_0x002B5C66(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C66, ctx); }
void SecondaryStep_0x002B5C68(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C68, ctx); }
void SecondaryStep_0x002B5C6B(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C6B, ctx); }
void SecondaryStep_0x002B5C6D(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C6D, ctx); }
void SecondaryStep_0x002B5C6F(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C6F, ctx); }
void SecondaryStep_0x002B5C72(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C72, ctx); }
void SecondaryStep_0x002B5C74(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C74, ctx); }
void SecondaryStep_0x002B5C76(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C76, ctx); }
void SecondaryStep_0x002B38A2(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B38A2, ctx); }
void SecondaryStep_0x002B58B8(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B58B8, ctx); }
void SecondaryStep_0x002B597D(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B597D, ctx); }

void x86_memory_fault(X86Context&, const X86MemoryFault& fault) {
    ProbeFailure failure;
    failure.reason = std::string("guest memory fault: ") + mem::StatusName(fault.status);
    failure.pc = fault.instruction;
    failure.address = fault.address;
    failure.has_address = true;
    throw failure;
}

void x86_checked_trap(X86Context&, uint32_t va, const char* what) {
    ProbeFailure failure;
    failure.reason = std::string("checked recompiler blocked instruction: ") + (what ? what : "");
    failure.pc = va;
    throw failure;
}

// Reviewed real-memory policy.  The route may touch its own stack, the KPCR
// exception-list slot, the ETHREAD TLS pointer, the three rdata globals and the
// TLS block.  No import page and no unreviewed region is admitted.
void execution_probe_memory_access(uint32_t instruction, uint32_t address, std::size_t size, bool write) {
    if (!g_probe.running) Fail("memory access outside a probe run", instruction, address, true);
    if (size == 0) Fail("zero-length memory access", instruction, address, true);
    if (g_probe.memory.size() >= kMaximumMemoryOperations)
        Fail("memory-operation budget exhausted", instruction, address, true);
    const uint64_t end = uint64_t(address) + size;
    if (end > (uint64_t(1) << 32)) Fail("memory access overflows the guest address space", instruction, address, true);

    auto admit = [&] { g_probe.memory.push_back({instruction, address, uint32_t(size), write}); };

    if (!write && instruction == 0x00319809 && address == g_probe.kpcr && size == 4) return admit();
    if (write && instruction == 0x00319838 && address == g_probe.kpcr && size == 4) return admit();
    if (!write && instruction == 0x002B5C1F && address == g_probe.kpcr + 0x28 && size == 4) return admit();
    if (!write && instruction == 0x002B5C28 && address == g_probe.ethread + 0x28 && size == 4) return admit();
    if (!write && size == 4 &&
        ((instruction == 0x002B5C34 && address == kTlsGlobalEnd) ||
         (instruction == 0x002B5C3A && address == kTlsGlobalSource) ||
         (instruction == 0x002B5C57 && address == kTlsGlobalZeroFill)))
        return admit();
    if (write && address >= g_probe.tls && end <= uint64_t(g_probe.tls) + kTlsBlockSize) {
        if (instruction == 0x002B5C31 || instruction == 0x002B5C6B || instruction == 0x002B5C72) return admit();
        Fail("unreviewed TLS-block write", instruction, address, true);
    }
    if (address >= 0x003C9000 && address < 0x003CA000)
        Fail("import page access is forbidden", instruction, address, true);
    if (address >= g_probe.stack_base && end <= g_probe.tls) return admit();
    Fail("memory access outside the reviewed spans", instruction, address, true);
}

namespace {

std::string Sha1Hex(const std::vector<uint8_t>& bytes) {
    alignas(8) uint8_t context[128];
    uint8_t digest[20];
    crypto::ShaInit(context);
    crypto::ShaUpdate(context, bytes.data(), bytes.size());
    crypto::ShaFinal(context, digest);
    std::string result;
    result.reserve(40);
    for (uint8_t byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}

bool ReadFile(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), {});
    return bool(file) || file.eof();
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
    return count;
}

bool ValidateIdentity(const std::string& path, xbe::Image& image, std::string& error) {
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

bool ValidateRouteText(const xbe::Image& image, std::string& error) {
    static_assert(kExpectedRouteTextCount == kRouteCount);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC, ZYAN_FALSE);

    uint64_t hash = 0xcbf29ce484222325ULL;
    size_t route_bytes = 0;
    for (size_t i = 0; i < kRouteCount; ++i) {
        const RouteStep& step = kRoute[i];
        const int64_t offset = image.Offset(step.pc);
        const size_t span = image.FileSpan(step.pc);
        if (offset < 0 || span == 0) {
            error = "route PC is not file-backed";
            return false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction,
                                                 operands))) {
            error = "route instruction did not decode";
            return false;
        }
        char text[256];
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, text,
                                        sizeof(text), step.pc, nullptr);
        if (std::strcmp(text, kExpectedRouteText[i]) != 0 ||
            instruction.length != step.length) {
            error = std::string("route text/length mismatch at ") + std::to_string(step.pc);
            return false;
        }
        if (step.kind == StepKind::kCall || step.kind == StepKind::kConditional) {
            const uint32_t expected = step.kind == StepKind::kCall ? step.next : step.alternate;
            if (operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                uint32_t(step.pc + instruction.length + operands[0].imm.value.s) != expected) {
                error = std::string("route control-flow target mismatch at ") + std::to_string(step.pc);
                return false;
            }
        }
        if (step.kind == StepKind::kRet &&
            (instruction.mnemonic != ZYDIS_MNEMONIC_RET || instruction.operand_count_visible != 0)) {
            error = "route RET is not a plain return";
            return false;
        }
        for (uint32_t j = 0; j < step.length; ++j) {
            hash ^= image.file[size_t(offset) + j];
            hash *= 0x100000001b3ULL;
        }
        route_bytes += step.length;
    }
    if (route_bytes != kExpectedRouteBytes || hash != kExpectedRouteHash) {
        error = "route byte hash mismatch";
        return false;
    }
    for (size_t i = 0; i < kTrapCount; ++i) {
        const uint32_t pc = kTrapPcs[i];
        const int64_t offset = image.Offset(pc);
        const size_t span = image.FileSpan(pc);
        if (offset < 0 || span == 0) {
            error = "trap boundary is not file-backed";
            return false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction,
                                                 operands))) {
            error = "trap boundary did not decode";
            return false;
        }
        char text[256];
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, text,
                                        sizeof(text), pc, nullptr);
        if (std::strcmp(text, kExpectedTrapText[i]) != 0 ||
            instruction.length != (pc == kNextHelper ? 1 : 5)) {
            error = std::string("trap boundary text/length mismatch at ") + std::to_string(pc);
            return false;
        }
        if (pc == kStartRoutine &&
            (instruction.mnemonic != ZYDIS_MNEMONIC_CALL || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
             uint32_t(pc + instruction.length + operands[0].imm.value.s) != kStackHelper)) {
            error = "start routine entry is not the reviewed call";
            return false;
        }
    }
    return true;
}

bool ValidateMappingTextFromStrings(const std::string& header, const std::string& source,
                                   const std::string& mapping, std::string& error) {
    if (mapping.find("const uint32_t X86EntryPoint = 0x2B3916u;") == std::string::npos ||
        mapping.find("const std::size_t X86FuncMappingCount = 5;") == std::string::npos ||
        mapping.find("const std::size_t X86ContinuationCount = 0;") == std::string::npos) {
        error = "generated real secondary mapping entry or function count is unexpected";
        return false;
    }
    for (const RouteStep& step : kRoute) {
        char name[64];
        std::snprintf(name, sizeof(name), "SecondaryStep_0x%08X(", step.pc);
        if (CountOccurrences(header, name) != 1 || CountOccurrences(source, name) != 1) {
            error = std::string("real hook coverage mismatch at ") + name;
            return false;
        }
    }
    for (uint32_t pc : kTrapPcs) {
        char name[64];
        std::snprintf(name, sizeof(name), "SecondaryStep_0x%08X(", pc);
        if (CountOccurrences(header, name) != 1 || CountOccurrences(source, name) != 1) {
            error = std::string("real trap hook coverage mismatch at ") + name;
            return false;
        }
    }
    const size_t expected_hooks = kRouteCount + kTrapCount;
    if (CountOccurrences(source, "SecondaryStep_0x") != expected_hooks ||
        CountOccurrences(header, "SecondaryStep_0x") != expected_hooks) {
        error = "generated real hook counts are not exactly the reviewed set";
        return false;
    }
    if (CountOccurrences(source, "X86_FUNC(__imp__sub_") != 5 ||
        source.find("X86_FUNC(__imp__sub_002B5C0F)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_00319804)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_002B38A2)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_002B58B8)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_002B597D)") == std::string::npos ||
        source.find("sub_00319858") != std::string::npos) {
        error = "generated real source has an unexpected function set";
        return false;
    }
    const std::string call_seh = "x86_checked_call(ctx, base, 0x319804u, 0x2B5C1Bu, 0x2B5C16u)";
    const std::string call_start = "x86_checked_call(ctx, base, 0x2B58B8u, 0x2B38A7u, 0x2B38A2u)";
    const std::string call_next = "x86_checked_call(ctx, base, 0x2B597Du, 0x2B5C7Bu, 0x2B5C76u)";
    if (source.find(call_seh) == std::string::npos || source.find(call_start) == std::string::npos ||
        source.find(call_next) == std::string::npos ||
        CountOccurrences(source, "x86_checked_call") != 3) {
        error = "generated real source does not contain exactly the three reviewed calls";
        return false;
    }
    if (CountOccurrences(source, "x86_checked_ret") != 1 ||
        source.find("x86_checked_ret(ctx, 0x31983Eu, 0)") == std::string::npos) {
        error = "generated real source does not contain exactly the reviewed SEH RET";
        return false;
    }
    if (CountOccurrences(source, "x86_rep_movs_checked") != 2 ||
        CountOccurrences(source, "x86_rep_stos_checked") != 2) {
        error = "generated real source does not contain the reviewed REP forms";
        return false;
    }
    for (const char* forbidden : {"x86_icall", "x86_kernel", "ProbeStep_"}) {
        if (source.find(forbidden) != std::string::npos) {
            error = std::string("generated real source contains ") + forbidden;
            return false;
        }
    }
    if (CountOccurrences(source, "x86_resume") != 1 ||
        CountOccurrences(source, "catch (const X86Resume& r)") != 1) {
        error = "generated real source contains an unexpected X86Resume path";
        return false;
    }
    for (const char* after_boundary : {"// 002B5C7B", "// 0031983F", "// 002B38A7", "// 002B58BD",
                                       "// 002B597E"}) {
        if (source.find(after_boundary) != std::string::npos) {
            error = "generated real source decoded past a reviewed boundary";
            return false;
        }
    }
    return true;
}

bool ValidateMappingText(std::string& error) {
    std::vector<uint8_t> header_bytes, source_bytes, mapping_bytes;
    const std::string dir = EXECUTION_PROBE_SECONDARY_REAL_GENERATED_DIR;
    if (!ReadFile((dir + "/x86_recomp_shared.h").c_str(), header_bytes) ||
        !ReadFile((dir + "/x86_recomp.0.cpp").c_str(), source_bytes) ||
        !ReadFile((dir + "/x86_func_mapping.cpp").c_str(), mapping_bytes)) {
        error = "generated real secondary mapping files are missing";
        return false;
    }
    return ValidateMappingTextFromStrings(std::string(header_bytes.begin(), header_bytes.end()),
                                          std::string(source_bytes.begin(), source_bytes.end()),
                                          std::string(mapping_bytes.begin(), mapping_bytes.end()), error);
}

bool RoundSection(const xbe::Image& image, const char* name, uint32_t& begin, uint32_t& end) {
    for (const auto& section : image.sections) {
        if (section.name != name) continue;
        begin = section.va & ~(mem::kPage - 1);
        end = (section.va + section.vsize + mem::kPage - 1) & ~(mem::kPage - 1);
        return begin < end;
    }
    return false;
}

bool ProtectRange(uint32_t base, uint32_t size, uint8_t permissions) {
    return mem::Protect(base, size, permissions) == mem::AccessStatus::kOk;
}

bool Read32(uint32_t address, const char* name, uint32_t& value, std::string& error) {
    if (mem::Read(address, &value, sizeof(value)) != mem::AccessStatus::kOk) {
        error = std::string("could not read ") + name;
        return false;
    }
    return true;
}

bool ValidateContext(const kernel::ThreadRef& thread, const X86Context& ctx, std::string& error) {
    if (!thread || thread->state.load() != kernel::ThreadState::kPrepared ||
        thread->suspend.load() != 0 || !kernel::Threads().empty()) {
        error = "prepared thread is not a fresh inactive thread";
        return false;
    }
    if (thread->system_routine != kSystemRoutine || thread->start_routine != kStartRoutine ||
        thread->start_context != kStartContext) {
        error = "prepared-thread routine arguments are not authentic";
        return false;
    }
    const uint32_t expected_esp = (thread->tls & ~15u) - 16;
    if (ctx.esp != expected_esp || ctx.esp % 16 != 0) {
        error = "initial ESP does not match the production InitContext contract";
        return false;
    }
    if (ctx.fs_base != thread->kpcr) {
        error = "FS base is not the prepared KPCR";
        return false;
    }
    if (ctx.fcw != 0x27F || ctx.mxcsr != 0x1F80 || ctx.thread_handle != 0) {
        error = "x87/SSE or thread-handle context is not the production value";
        return false;
    }
    const uint32_t stack_base = thread->stack_top - thread->stack_size;
    if (!thread->stack_top || !thread->stack_size ||
        ctx.esp < stack_base + kCallbackFrameBytes + 0x40 || ctx.esp > thread->stack_top) {
        error = "initial ESP is outside the owned stack's callback region";
        return false;
    }
    if (thread->tls < stack_base || thread->tls + kTlsSize > thread->stack_top) {
        error = "TLS block is not inside the owned stack";
        return false;
    }
    if (kernel::R32(thread->kpcr + 0x00) != 0xFFFFFFFF) {
        error = "initial fs:[0] exception list is not the reviewed terminator";
        return false;
    }
    if (kernel::R32(thread->kpcr + 0x04) != thread->stack_top ||
        kernel::R32(thread->kpcr + 0x08) != stack_base ||
        kernel::R32(thread->kpcr + 0x28) != thread->ethread ||
        kernel::R32(thread->kpcr + 0x20) != thread->kpcr + 0x28) {
        error = "KPCR stack/ETHREAD fields are not the production values";
        return false;
    }
    if (kernel::R32(thread->ethread + 0x1C) != thread->stack_top ||
        kernel::R32(thread->ethread + 0x20) != stack_base ||
        kernel::R32(thread->ethread + 0x28) != thread->tls ||
        kernel::R32(thread->ethread + 0x12C) != thread->id) {
        error = "ETHREAD stack/TLS/id fields are not the production values";
        return false;
    }
    if (mem::CheckAccess(thread->kpcr, 4, mem::kReadWrite) != mem::AccessStatus::kOk) {
        error = "KPCR is not writable for the exception registration";
        return false;
    }
    if (mem::CheckAccess(stack_base, mem::kPage, mem::kReadWrite) != mem::AccessStatus::kOk ||
        !mem::IsCommitted(thread->stack_top - 1)) {
        error = "prepared stack is not committed";
        return false;
    }
    if (thread->ctx != &ctx) {
        error = "prepared thread does not point at the preflight context";
        return false;
    }
    return true;
}

bool ValidateTlsContract(const RealFacts& facts, std::string& error) {
    if (uint64_t(kTlsPointerBytes) + facts.tls_copy + facts.tls_zero > kTlsBlockSize) {
        error = "TLS copy and zero-fill exceed the prepared TLS block";
        return false;
    }
    if (facts.tls_copy != kAuthenticCopyBytes || facts.tls_zero != kAuthenticZeroFillBytes) {
        error = "authentic TLS globals are not copy 0 / zero-fill 0x90";
        return false;
    }
    return true;
}

bool PrepareReal(const std::string& path, RealFacts& facts, xbe::Image& image, kernel::ThreadRef& thread,
                 X86Context& context, kernel::GuestCallbackTicket& ticket, std::string& error) {
    if (!ValidateIdentity(path, image, error)) return false;
    if (!ValidateRouteText(image, error)) return false;
    if (!ValidateMappingText(error)) return false;
    const std::string sha1 = Sha1Hex(image.file);
    if (!xbe::PrepareImage(image, kRealEntry, sha1, error)) return false;
    facts.base = image.base;
    facts.image_size = image.image_size;
    kernel::g_base = mem::Base();
    const dispatch::Metadata metadata{X86CodeBase, X86CodeEnd, kSystemRoutine,
                                      std::span<const X86FuncMapping>(X86FuncMappings, X86FuncMappingCount),
                                      std::span<const uint32_t>(X86Continuations, X86ContinuationCount)};
    if (!dispatch::Init(metadata, error)) {
        error = "dispatch init failed: " + error;
        return false;
    }
    if (X86FuncMappingCount != 5 || X86ContinuationCount != 0) {
        error = "generated real secondary mapping does not contain five functions and no continuations";
        return false;
    }
    bool system = false, helper = false, start = false, stack_helper = false, next_helper = false;
    for (size_t i = 0; i < X86FuncMappingCount; ++i) {
        const uint32_t guest = X86FuncMappings[i].guest;
        if (guest == kSystemRoutine) system = true;
        else if (guest == kSehHelper) helper = true;
        else if (guest == kStartRoutine) start = true;
        else if (guest == kStackHelper) stack_helper = true;
        else if (guest == kNextHelper) next_helper = true;
        else {
            error = "generated real secondary mapping contains an unexpected function";
            return false;
        }
    }
    if (!system || !helper || !start || !stack_helper || !next_helper) {
        error = "generated real secondary mapping is missing a reviewed function";
        return false;
    }

    // The image starts read-only.  Only .text is executable; the secondary
    // route does not write any image section.
    if (!ProtectRange(image.base, image.image_size, mem::kRead)) {
        error = "could not protect the prepared image read-only";
        return false;
    }
    uint32_t text_begin = 0, text_end = 0;
    if (!RoundSection(image, ".text", text_begin, text_end) ||
        !ProtectRange(text_begin, text_end - text_begin, mem::kRead | mem::kExecute)) {
        error = "could not protect the reviewed code section executable";
        return false;
    }
    if (!ProtectRange(0x003C9000, mem::kPage, mem::kRead)) {
        error = "could not protect the import page read-only";
        return false;
    }
    if (!mem::Commit(kOutputPage, mem::kPage)) {
        error = "could not commit the probe output page";
        return false;
    }
    if (mem::Fill(kOutputPage, 0, mem::kPage) != mem::AccessStatus::kOk) {
        error = "could not clear the probe output page";
        return false;
    }

    const uint32_t status = kernel::PrepareSystemThreadEx(
        kRealHandleOut, kStackRequest, kTlsSize, kRealIdOut, kStartRoutine, kStartContext, false, kSystemRoutine, thread);
    if (status != kernel::kSuccess || !thread) {
        error = "fresh thread preparation failed";
        return false;
    }
    if (!kernel::InitializePreparedThreadContextForTest(thread, context)) {
        error = "prepared thread context initialization failed";
        return false;
    }
    if (!ValidateContext(thread, context, error)) return false;
    if (kernel::PrepareGuestCallback(kSystemRoutine, kernel::GuestCallbackClass::kSystemRoutine, ticket) !=
        kernel::GuestCallbackStatus::kReady) {
        error = "system-routine callback ticket was rejected";
        return false;
    }
    if (ticket.target != kSystemRoutine || ticket.callback_class != kernel::GuestCallbackClass::kSystemRoutine ||
        ticket.dispatch_generation == 0) {
        error = "system-routine callback ticket does not describe the reviewed target";
        return false;
    }
    if (!Read32(kTlsGlobalSource, "[0x3ED3EC]", facts.tls_source, error) ||
        !Read32(kTlsGlobalEnd, "[0x3ED3F0]", facts.tls_end, error) ||
        !Read32(kTlsGlobalZeroFill, "[0x3ED3FC]", facts.tls_zero, error)) {
        return false;
    }
    if (facts.tls_end < facts.tls_source) {
        error = "TLS source range is inverted";
        return false;
    }
    facts.tls_copy = facts.tls_end - facts.tls_source;
    if (!ValidateTlsContract(facts, error)) return false;

    uint32_t handle = 0, id = 0;
    if (mem::Read(kRealHandleOut, &handle, sizeof(handle)) != mem::AccessStatus::kOk ||
        mem::Read(kRealIdOut, &id, sizeof(id)) != mem::AccessStatus::kOk) {
        error = "could not read the prepared thread outputs";
        return false;
    }
    if (!handle || handle != thread->published_handle || id != thread->id ||
        kernel::FromHandle(handle).get() != thread.get()) {
        error = "prepared thread outputs are not published";
        return false;
    }

    facts.header_size = image.header_size;
    facts.sections = image.sections.size();
    facts.stack_base = thread->stack_top - thread->stack_size;
    facts.stack_top = thread->stack_top;
    facts.initial_esp = context.esp;
    facts.entry_esp = context.esp - kCallbackFrameBytes;
    facts.stop_esp = facts.entry_esp - 0x3C;
    facts.stop_ebp = facts.entry_esp - 0x04;
    facts.registration = facts.entry_esp - 0x14;
    facts.kpcr = thread->kpcr;
    facts.ethread = thread->ethread;
    facts.tls = thread->tls;
    facts.fs0 = kernel::R32(thread->kpcr);
    facts.handle = handle;
    facts.id = id;
    facts.worker_baseline = kernel::ThreadWorkerLaunchCountForTest();
    facts.snapshot_pages = {uint32_t((thread->stack_top - 1) & ~(mem::kPage - 1)),
                            thread->kpcr & ~(mem::kPage - 1),
                            kRdataPage,
                            kOutputPage,
                            image.base};
    return true;
}

void PrintFacts(const RealFacts& facts) {
    std::printf("PREFLIGHT identity: base=%08X image=%X header=%X sections=%u\n", facts.base, facts.image_size,
                facts.header_size, facts.sections);
    std::printf("PREFLIGHT route: %zu/%zu matched, bytes=%u hash=%016llX stop=%08X\n", kRouteCount, kRouteCount,
                kExpectedRouteBytes, static_cast<unsigned long long>(kExpectedRouteHash), kStopBefore);
    std::printf("PREFLIGHT mapping: functions=5 continuations=0 hooks=%zu checked_calls=3 checked_rets=1\n",
                kRouteCount + kTrapCount);
    std::printf("PREFLIGHT targets: system=%08X seh=%08X ret=%08X handler=%08X start=%08X next=%08X\n",
                kSystemRoutine, kSehHelper, kSehReturn, kSehHandler, kStartRoutine, kNextHelper);
    std::printf("PREFLIGHT stack: base=%08X top=%08X initial_esp=%08X entry_esp=%08X stop_esp=%08X stop_ebp=%08X\n",
                facts.stack_base, facts.stack_top, facts.initial_esp, facts.entry_esp, facts.stop_esp,
                facts.stop_ebp);
    std::printf("PREFLIGHT context: kpcr=%08X ethread=%08X tls=%08X fs0=%08X registration=%08X\n", facts.kpcr,
                facts.ethread, facts.tls, facts.fs0, facts.registration);
    std::printf("PREFLIGHT callback: system=%08X start=%08X context=%08X handle=%08X id=%08X\n", kSystemRoutine,
                kStartRoutine, kStartContext, facts.handle, facts.id);
    std::printf("PREFLIGHT tls: source=%08X end=%08X copy=%u zero=%u tls_bytes=%llu\n", facts.tls_source,
                facts.tls_end, facts.tls_copy, facts.tls_zero,
                static_cast<unsigned long long>(kTlsPointerBytes + facts.tls_copy + facts.tls_zero));
    std::printf("PREFLIGHT pages: output=%08X+%X:RW text=RX rdata=R data=R header=R import=R\n", kOutputPage,
                mem::kPage);
    std::printf("PREFLIGHT limits: instructions=%u direct_calls=%u kernel_calls=%u imports=%u memory=%u "
                "callbacks=0 workers=0\n",
                kMaximumInstructions, kMaximumDirectCalls, kMaximumKernelCalls, kMaximumImportReads,
                kMaximumMemoryOperations);
    std::printf("PREFLIGHT PASS\n");
}

bool CancelThread(const kernel::ThreadRef& thread, RealFacts& facts, std::string& error) {
    if (!thread) {
        error = "no prepared thread to cancel";
        return false;
    }
    const uint32_t handle = thread->published_handle;
    const uint32_t stack_top = thread->stack_top;
    uint32_t published = 0;
    const bool handle_read = mem::Read(kRealHandleOut, &published, sizeof(published)) == mem::AccessStatus::kOk;
    const bool prepared = thread->state.load() == kernel::ThreadState::kPrepared && handle && handle_read &&
                          published == handle && kernel::FromHandle(handle).get() == thread.get() &&
                          mem::IsCommitted(stack_top - 1);
    const bool cancelled = kernel::CancelPreparedThread(thread);
    uint32_t cleared = 1, id_cleared = 1;
    const bool output_cleared =
        mem::Read(kRealHandleOut, &cleared, sizeof(cleared)) == mem::AccessStatus::kOk && cleared == 0;
    const bool id_ok = mem::Read(kRealIdOut, &id_cleared, sizeof(id_cleared)) == mem::AccessStatus::kOk &&
                       id_cleared == 0;
    const bool stack_freed = !mem::IsCommitted(stack_top - 1);
    const bool tickets_clear = thread->system_routine_ticket.target == 0 && thread->start_routine_ticket.target == 0;
    const bool ok = prepared && cancelled && output_cleared && id_ok && stack_freed && tickets_clear &&
                    !kernel::FromHandle(handle) && thread->state.load() == kernel::ThreadState::kCancelled &&
                    thread->published_handle == 0 && thread->ctx == nullptr && kernel::Threads().empty();
    std::printf("CANCEL handle_closed=%d prepared=%d cancelled=%d output_cleared=%d id_cleared=%d stack_freed=%d "
                "tickets_clear=%d workers=%zu\n",
                int(!kernel::FromHandle(handle)), int(prepared), int(cancelled), int(output_cleared), int(id_ok),
                int(stack_freed), int(tickets_clear), kernel::KernelWorkerCountForTest());
    if (!ok) error = "prepared-thread cancellation or output clearing failed";
    (void)facts;
    return ok;
}

bool Teardown(uint32_t image_base, bool output_committed) {
    kernel::StopKernelWorkers();
    dispatch::Reset();
    if (image_base) {
        uint32_t base = image_base, size = 0;
        mem::FreeVirtual(base, size, mem::kMemRelease);
    }
    if (output_committed) mem::Decommit(kOutputPage, mem::kPage);
    const mem::Stats stats = mem::Query();
    const size_t workers = kernel::KernelWorkerCountForTest();
    mem::Shutdown();
    std::printf("TEARDOWN system_used=%u contiguous_used=%u virtual_committed=%u workers=%zu initialized=%d\n",
                stats.system_used, stats.contiguous_used, stats.virtual_committed, workers, int(mem::IsInitialized()));
    return stats.system_used == 0 && stats.contiguous_used == 0 && stats.virtual_committed == 0 && workers == 0 &&
           !mem::IsInitialized();
}

bool VerifySeh(const RealFacts& facts, std::string& error) {
    auto read = [](uint32_t address, const char* what, uint32_t& value, std::string& error) {
        if (mem::Read(address, &value, sizeof(value)) != mem::AccessStatus::kOk) {
            error = std::string("could not read ") + what;
            return false;
        }
        return true;
    };
    uint32_t exception_list = 0, next = 0, handler = 0, scope = 0, scope_flags = 0, saved_ebp = 0;
    uint32_t saved_esp = 0, saved_edi = 0, saved_esi = 0, saved_ebx = 0, local_eax = 0, local_edx = 0;
    uint32_t local_ebx = 0;
    const uint32_t S = facts.entry_esp;
    if (!read(facts.kpcr, "fs:[0]", exception_list, error) ||
        !read(S - 0x14, "SEH next", next, error) ||
        !read(S - 0x10, "SEH handler", handler, error) ||
        !read(S - 0x0C, "SEH scope table", scope, error) ||
        !read(S - 0x08, "SEH scope flags", scope_flags, error) ||
        !read(S - 0x04, "saved EBP", saved_ebp, error) ||
        !read(S - 0x1C, "saved ESP", saved_esp, error) ||
        !read(S - 0x38, "saved EDI", saved_edi, error) ||
        !read(S - 0x34, "saved ESI", saved_esi, error) ||
        !read(S - 0x30, "saved EBX", saved_ebx, error) ||
        !read(S - 0x24, "local EAX", local_eax, error) ||
        !read(S - 0x28, "local EDX", local_edx, error) ||
        !read(S - 0x2C, "local EBX", local_ebx, error))
        return false;
    if (exception_list != facts.registration) {
        error = "fs:[0] does not point at the new SEH registration";
        return false;
    }
    if (next != 0xFFFFFFFF || handler != kSehHandler) {
        error = "SEH registration record is not the reviewed pair";
        return false;
    }
    if (scope != kScopeTable || scope_flags != 0) {
        error = "SEH scope table or prolog flag is not the reviewed value";
        return false;
    }
    if (saved_ebp != 0 || saved_edi != 0 || saved_esi != 0 || saved_ebx != 0) {
        error = "SEH saved registers or EBP were corrupted";
        return false;
    }
    if (saved_esp != S - 0x38) {
        error = "SEH saved ESP is not the post-prolog stack";
        return false;
    }
    if (local_eax != facts.ethread || local_edx != facts.tls + 4 || local_ebx > kTlsBlockSize) {
        error = "SEH local frame does not hold the reviewed TLS values";
        return false;
    }
    std::printf("SEH registration=%08X next=%08X handler=%08X scope=%08X saved_esp=%08X\n", facts.registration,
                next, handler, scope, saved_esp);
    return true;
}

bool VerifyTls(const RealFacts& facts, std::string& error) {
    uint32_t pointer = 0;
    if (mem::Read(facts.tls, &pointer, sizeof(pointer)) != mem::AccessStatus::kOk) {
        error = "could not read the TLS pointer slot";
        return false;
    }
    if (pointer != facts.tls + kTlsPointerBytes) {
        error = "TLS pointer slot does not point at TLS+4";
        return false;
    }
    for (uint32_t i = 0; i < facts.tls_zero; ++i) {
        uint8_t value = 0;
        if (mem::Read(facts.tls + kTlsPointerBytes + i, &value, sizeof(value)) != mem::AccessStatus::kOk) {
            error = "could not read the TLS zero-fill destination";
            return false;
        }
        if (value != 0) {
            error = "TLS zero-fill mismatch";
            return false;
        }
    }
    std::printf("TLS pointer=%08X copy=%u zero_fill=%u\n", pointer, facts.tls_copy, facts.tls_zero);
    return true;
}

unsigned CountOps(uint32_t pc, uint32_t address, bool write) {
    unsigned count = 0;
    for (const MemoryRecord& record : g_probe.memory)
        if (record.pc == pc && record.address == address && record.write == write && record.size == 4) ++count;
    return count;
}

bool VerifyMemoryOperations(const RealFacts& facts, std::string& error) {
    if (CountOps(0x00319809, facts.kpcr, false) != 1) {
        error = "KPCR exception-list read count is not one";
        return false;
    }
    if (CountOps(0x00319838, facts.kpcr, true) != 1) {
        error = "KPCR exception-list write count is not one";
        return false;
    }
    if (CountOps(0x002B5C1F, facts.kpcr + 0x28, false) != 1) {
        error = "KPCR ETHREAD read count is not one";
        return false;
    }
    if (CountOps(0x002B5C28, facts.ethread + 0x28, false) != 1) {
        error = "ETHREAD TLS-pointer read count is not one";
        return false;
    }
    if (CountOps(0x002B5C34, kTlsGlobalEnd, false) != 1 ||
        CountOps(0x002B5C3A, kTlsGlobalSource, false) != 1 ||
        CountOps(0x002B5C57, kTlsGlobalZeroFill, false) != 1) {
        error = "TLS-global read counts are not one";
        return false;
    }
    if (CountOps(0x002B5C31, facts.tls, true) != 1) {
        error = "TLS pointer publication count is not one";
        return false;
    }
    unsigned copy_writes = 0;
    for (const MemoryRecord& record : g_probe.memory)
        if ((record.pc == 0x002B5C4E || record.pc == 0x002B5C55) && record.write) ++copy_writes;
    if (copy_writes != 0) {
        error = "the authentic copy length zero still wrote TLS data";
        return false;
    }
    unsigned fill = 0;
    for (uint32_t offset = 0; offset < facts.tls_zero; offset += 4)
        fill += CountOps(0x002B5C6B, facts.tls + kTlsPointerBytes + offset, true);
    if (fill != facts.tls_zero / 4) {
        error = "TLS zero-fill dword count is not 0x24";
        return false;
    }
    if (CountOps(0x002B5C72, facts.tls + kTlsPointerBytes + facts.tls_zero, true) != 0) {
        error = "the zero-remainder REP STOSB unexpectedly wrote";
        return false;
    }
    for (const MemoryRecord& record : g_probe.memory) {
        if (record.address >= 0x003C9000 && record.address < 0x003CA000) {
            error = "an import page access was recorded";
            return false;
        }
        const bool external =
            (record.address == facts.kpcr &&
             (record.pc == 0x00319809 || record.pc == 0x00319838)) ||
            (record.address == facts.kpcr + 0x28 && record.pc == 0x002B5C1F) ||
            (record.address == facts.ethread + 0x28 && record.pc == 0x002B5C28) ||
            (record.address == kTlsGlobalEnd && record.pc == 0x002B5C34) ||
            (record.address == kTlsGlobalSource && record.pc == 0x002B5C3A) ||
            (record.address == kTlsGlobalZeroFill && record.pc == 0x002B5C57);
        if (external) continue;
        if (record.address >= facts.tls &&
            record.address + record.size <= uint64_t(facts.tls) + kTlsBlockSize)
            continue;
        if (record.address >= facts.stack_base &&
            record.address + record.size <= uint64_t(facts.tls))
            continue;
        error = "a memory access escaped the stack/KPCR/ETHREAD/TLS spans";
        return false;
    }
    return true;
}

bool VerifyRealPostconditions(const RealFacts& facts, std::string& error) {
    if (!g_probe.have_stop || g_probe.stop.pc != kStopBefore) {
        error = "mandatory stop is not 002B5C76";
        return false;
    }
    if (g_probe.executed != kExpectedPrepareInstructions ||
        g_probe.trace.size() != kExpectedPrepareInstructions) {
        error = "expected 53 instructions";
        return false;
    }
    if (g_probe.trace.empty() || g_probe.trace.back().pc != 0x002B5C74) {
        error = "the last executed PC is not 002B5C74";
        return false;
    }
    if (g_probe.direct_calls != 1 || g_probe.kernel_calls != 0 || g_probe.import_reads != 0) {
        error = "call or import counts are not the reviewed set";
        return false;
    }
    if (!g_probe.stop_frame_clear) {
        error = "the checked call frame was not clear at the stop";
        return false;
    }
    if (g_probe.stop.esp != facts.stop_esp || g_probe.stop.ebp != facts.stop_ebp) {
        error = "SEH prolog stack or EBP contract mismatch";
        return false;
    }
    if (g_probe.stop.eax != 0 || g_probe.stop.ecx != 0 || g_probe.stop.edx != 0x90 ||
        g_probe.stop.ebx != 0 || g_probe.stop.esi != 0 || g_probe.stop.edi != facts.tls + kTlsBlockSize) {
        error = "post-TLS register state mismatch";
        return false;
    }
    if (g_probe.stop.zf != 1) {
        error = "the stop flags are not the reviewed result";
        return false;
    }
    uint32_t pushed = 0;
    if (mem::Read(g_probe.stop.esp, &pushed, sizeof(pushed)) != mem::AccessStatus::kOk || pushed != 1) {
        error = "the push 1 stack slot is not the reviewed value";
        return false;
    }
    const StepRecord* branch = nullptr;
    for (const StepRecord& step : g_probe.trace)
        if (step.pc == kTlsFillBranch) branch = &step;
    if (!branch || !branch->branch_valid || branch->branch_taken) {
        error = "TLS zero-fill branch is not the authentic not-taken path";
        return false;
    }
    if (!VerifySeh(facts, error)) return false;
    if (!VerifyTls(facts, error)) return false;
    if (!VerifyMemoryOperations(facts, error)) return false;
    if (!kernel::Threads().empty() || kernel::ThreadWorkerLaunchCountForTest() != facts.worker_baseline) {
        error = "a guest thread or worker became active";
        return false;
    }
    return true;
}

struct ChildResult {
    int status = -1;
    bool timed_out = false;
    std::string output;
    std::chrono::milliseconds elapsed{0};
};

std::string SupervisionToken() {
    const uint64_t nonce = uint64_t(Clock::now().time_since_epoch().count()) ^ (uint64_t(getpid()) << 32);
    char token[32];
    std::snprintf(token, sizeof(token), "%016llX", static_cast<unsigned long long>(nonce));
    return token;
}

bool SupervisionTokenValid() {
    const char* expected = std::getenv("MOSTWANTED_PROBE_SUPERVISION");
    if (!expected || !*expected) return false;
    char token[64] = {};
    const ssize_t count = read(kSupervisionFd, token, sizeof(token) - 1);
    if (count <= 0) return false;
    token[count] = '\0';
    return std::strcmp(token, expected) == 0;
}

ChildResult SpawnChild(const std::string& self, const std::string& scenario, std::chrono::milliseconds timeout) {
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

std::string SelfPath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size + 1);
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    char resolved[4096];
    if (!realpath(path.data(), resolved)) return {};
    return resolved;
#else
    return "/proc/self/exe";
#endif
}

struct SnapshotPage {
    uint32_t page;
    std::array<uint8_t, mem::kPage> bytes{};
    bool valid = false;
};

std::vector<SnapshotPage> Snapshot(const std::vector<uint32_t>& pages) {
    std::vector<SnapshotPage> snapshots;
    for (uint32_t page : pages) {
        SnapshotPage snapshot;
        snapshot.page = page;
        snapshot.valid = mem::Read(page, snapshot.bytes.data(), snapshot.bytes.size()) == mem::AccessStatus::kOk;
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

uint32_t WritePcFor(uint32_t address) {
    for (auto it = g_probe.memory.rbegin(); it != g_probe.memory.rend(); ++it)
        if (it->write && address >= it->address && address + 4 <= uint64_t(it->address) + it->size)
            return it->pc;
    return 0;
}

unsigned PrintMemoryDiff(const std::vector<SnapshotPage>& before) {
    unsigned changes = 0;
    unsigned printed = 0;
    for (const SnapshotPage& snapshot : before) {
        if (!snapshot.valid) continue;
        std::array<uint8_t, mem::kPage> after{};
        if (mem::Read(snapshot.page, after.data(), after.size()) != mem::AccessStatus::kOk) continue;
        for (uint32_t offset = 0; offset + 4 <= mem::kPage; offset += 4) {
            uint32_t old_value = 0, new_value = 0;
            std::memcpy(&old_value, snapshot.bytes.data() + offset, 4);
            std::memcpy(&new_value, after.data() + offset, 4);
            if (old_value == new_value) continue;
            const uint32_t address = snapshot.page + offset;
            if (printed < kMaximumPrintedDiffs) {
                std::printf("MEM pc=%08X address=%08X before=%08X after=%08X\n", WritePcFor(address), address,
                            old_value, new_value);
                ++printed;
            }
            ++changes;
        }
    }
    std::printf("guest_memory_changes=%u\n", changes);
    return changes;
}

void PrintTrace() {
    for (const StepRecord& step : g_probe.trace) {
        std::printf("STEP %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
                    "flags=%u%u%u%u%u%u",
                    step.pc, step.eax, step.ecx, step.edx, step.ebx, step.esp, step.ebp, step.esi, step.edi, step.cf,
                    step.zf, step.sf, step.of, step.pf, step.af);
        if (step.branch_valid) std::printf(" branch=%s", step.branch_taken ? "taken" : "not-taken");
        std::printf("\n");
    }
}

void PrintStop() {
    if (!g_probe.have_stop) return;
    const StepRecord& stop = g_probe.stop;
    std::printf("STOP %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
                "flags=%u%u%u%u%u%u frame=%s\n",
                stop.pc, stop.eax, stop.ecx, stop.edx, stop.ebx, stop.esp, stop.ebp, stop.esi, stop.edi, stop.cf,
                stop.zf, stop.sf, stop.of, stop.pf, stop.af, g_probe.stop_frame_clear ? "clear" : "active");
}

void PrintFailure(const ProbeFailure& failure) {
    std::printf("RESULT fail reason=\"%s\" pc=%08X", failure.reason.c_str(), failure.pc);
    if (failure.has_address) std::printf(" address=%08X", failure.address);
    std::printf(" instructions=%u direct_calls=%u kernel_calls=%u import_reads=%u\n", g_probe.executed,
                g_probe.direct_calls, g_probe.kernel_calls, g_probe.import_reads);
}

bool VerifySnapshotPages(const RealFacts& facts, const std::vector<SnapshotPage>& before, std::string& error) {
    for (const SnapshotPage& snapshot : before) {
        if (!snapshot.valid) {
            error = "a snapshot page was not readable";
            return false;
        }
        std::array<uint8_t, mem::kPage> after{};
        if (mem::Read(snapshot.page, after.data(), after.size()) != mem::AccessStatus::kOk) {
            error = "a snapshot page became unreadable";
            return false;
        }
        unsigned changes = 0;
        uint32_t changed_offset = 0, changed_value = 0;
        for (uint32_t offset = 0; offset + 4 <= mem::kPage; offset += 4) {
            uint32_t old_value = 0, new_value = 0;
            std::memcpy(&old_value, snapshot.bytes.data() + offset, 4);
            std::memcpy(&new_value, after.data() + offset, 4);
            if (old_value == new_value) continue;
            ++changes;
            changed_offset = offset;
            changed_value = new_value;
        }
        const bool is_stack = snapshot.page == uint32_t((facts.stack_top - 1) & ~(mem::kPage - 1));
        const bool is_kpcr = snapshot.page == (facts.kpcr & ~(mem::kPage - 1));
        if (is_stack) {
            if (!changes) {
                error = "the stack page did not record the SEH/TLS writes";
                return false;
            }
        } else if (is_kpcr) {
            if (changes != 1 || changed_offset != 0 || changed_value != facts.registration) {
                error = "the KPCR page changed outside fs:[0]";
                return false;
            }
        } else if (changes != 0) {
            error = "a read-only, output or header page changed";
            return false;
        }
    }
    return true;
}

std::string TempRealCopy(const char* tag) {
    char path[256];
    std::snprintf(path, sizeof(path), "/tmp/mostwanted-secondary-%d-%s.xbe", int(getpid()), tag);
    return path;
}

bool CopyAndPatch(const std::string& source, const std::string& destination, int64_t offset, uint8_t mask) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(source.c_str(), bytes)) return false;
    if (offset < 0 || size_t(offset) >= bytes.size()) return false;
    bytes[size_t(offset)] ^= mask;
    std::ofstream file(destination, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(file);
}

}  // namespace

ChildResult SpawnPlainChild(const std::string& self, const std::string& scenario,
                            std::chrono::milliseconds timeout) {
    int out[2];
    if (pipe(out) != 0) return {};
    const pid_t pid = fork();
    if (pid == 0) {
        close(out[0]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        close(out[1]);
        execl(self.c_str(), self.c_str(), "--child", scenario.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ChildResult result;
    if (pid < 0) {
        close(out[0]);
        close(out[1]);
        return result;
    }
    close(out[1]);
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

int RunPreflight(const std::string& scenario) {
    alarm(12);
    if (!mem::Init()) {
        std::printf("SETUP fail memory init\n");
        return 4;
    }
    if (!kernel::RestartKernelWorkers()) {
        std::printf("SETUP fail callback gate\n");
        mem::Shutdown();
        return 4;
    }

    RealFacts facts;
    xbe::Image image;
    kernel::ThreadRef thread;
    X86Context context{};
    kernel::GuestCallbackTicket ticket;
    std::string error;

    auto finish = [&](const char* failure, bool expected) {
        bool cancel_ok = true;
        if (thread) {
            thread->ctx = nullptr;
            std::string cancel_error;
            if (!CancelThread(thread, facts, cancel_error)) {
                cancel_ok = false;
                if (!failure) failure = "prepared-thread cancellation failed";
            }
        }
        if (failure) std::printf("RESULT fail reason=\"%s\"\n", failure);
        const bool teardown_ok = Teardown(facts.base, true);
        if (!teardown_ok || !cancel_ok) return 5;
        return failure ? (expected ? 3 : 5) : 0;
    };

    if (scenario == "identity-missing") {
        if (ValidateIdentity("/tmp/mostwanted-secondary-does-not-exist.xbe", image, error))
            return finish("missing real XBE was accepted", false);
        return finish("real XBE is missing", true);
    }
    if (scenario == "identity-mismatch") {
        const std::string path = TempRealCopy("identity");
        const bool copied = CopyAndPatch(EXECUTION_PROBE_SECONDARY_REAL_XBE, path, 0x100, 0xFF);
        const bool accepted = copied && ValidateIdentity(path, image, error);
        std::remove(path.c_str());
        if (accepted || !copied) return finish("patched identity was accepted", false);
        return finish("real XBE SHA-1 mismatch", true);
    }
    if (scenario == "route-mismatch" || scenario == "route-call-mismatch") {
        const std::string path = TempRealCopy(scenario.c_str());
        bool copied = ValidateIdentity(EXECUTION_PROBE_SECONDARY_REAL_XBE, image, error);
        const int64_t offset = copied ? image.Offset(scenario == "route-mismatch" ? kEntry : 0x002B5C17) : -1;
        copied = copied && CopyAndPatch(EXECUTION_PROBE_SECONDARY_REAL_XBE, path, offset, 0x01);
        xbe::Image patched;
        std::vector<uint8_t> bytes;
        std::string parse_error;
        const bool parsed = copied && ReadFile(path.c_str(), bytes) && patched.Parse(std::move(bytes), parse_error);
        const bool accepted = parsed && ValidateRouteText(patched, parse_error);
        std::remove(path.c_str());
        if (accepted || !parsed) return finish("patched route was accepted", false);
        return finish("route text/length mismatch", true);
    }
    if (scenario == "hook-mismatch") {
        std::vector<uint8_t> header_bytes, source_bytes, mapping_bytes;
        const std::string dir = EXECUTION_PROBE_SECONDARY_REAL_GENERATED_DIR;
        if (!ReadFile((dir + "/x86_recomp_shared.h").c_str(), header_bytes) ||
            !ReadFile((dir + "/x86_recomp.0.cpp").c_str(), source_bytes) ||
            !ReadFile((dir + "/x86_func_mapping.cpp").c_str(), mapping_bytes))
            return finish("generated mapping files are missing", false);
        std::string source(source_bytes.begin(), source_bytes.end());
        const std::string needle = "SecondaryStep_0x002B5C0F(ctx, base);";
        const size_t at = source.find(needle);
        if (at == std::string::npos) return finish("reviewed hook was not found", false);
        source.erase(at, needle.size());
        const bool accepted = ValidateMappingTextFromStrings(std::string(header_bytes.begin(), header_bytes.end()),
                                                              source,
                                                              std::string(mapping_bytes.begin(), mapping_bytes.end()),
                                                              error);
        if (accepted) return finish("hook removal was accepted", false);
        return finish("real hook coverage mismatch", true);
    }
    if (scenario == "bad-ticket") {
        if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error))
            return finish(error.c_str(), false);
        kernel::GuestCallbackTicket wrong;
        const kernel::GuestCallbackStatus status =
            kernel::PrepareGuestCallback(0x002B4000, kernel::GuestCallbackClass::kSystemRoutine, wrong);
        if (status != kernel::GuestCallbackStatus::kRejectedTarget)
            return finish("an unmapped system routine ticket was accepted", false);
        return finish("system-routine callback ticket was rejected", true);
    }
    if (scenario == "bad-fs" || scenario == "bad-kpcr" || scenario == "bad-ethread" ||
        scenario == "read-only-kpcr" || scenario == "uncommitted-stack") {
        if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error))
            return finish(error.c_str(), false);
        bool restore_kpcr = false;
        if (scenario == "bad-fs") {
            context.fs_base = 0x12345000;
        } else if (scenario == "bad-kpcr") {
            const uint32_t bad = 0x12345678;
            if (mem::Write(facts.kpcr, &bad, sizeof(bad)) != mem::AccessStatus::kOk)
                return finish("could not corrupt fs:[0]", false);
        } else if (scenario == "bad-ethread") {
            const uint32_t bad = 0x12345000;
            if (mem::Write(facts.ethread + 0x28, &bad, sizeof(bad)) != mem::AccessStatus::kOk)
                return finish("could not corrupt the ETHREAD TLS pointer", false);
        } else if (scenario == "read-only-kpcr") {
            if (mem::Protect(facts.kpcr & ~(mem::kPage - 1), mem::kPage, mem::kRead) != mem::AccessStatus::kOk)
                return finish("could not protect the KPCR page", false);
            restore_kpcr = true;
        } else {
            if (!mem::Decommit(facts.stack_base, mem::kPage))
                return finish("could not decommit the stack base page", false);
        }
        const bool valid = ValidateContext(thread, context, error);
        if (restore_kpcr)
            mem::Protect(facts.kpcr & ~(mem::kPage - 1), mem::kPage, mem::kRead | mem::kWrite);
        if (valid) return finish("corrupted context was accepted", false);
        return finish((std::string("context rejected: ") + error).c_str(), true);
    }
    if (scenario == "tls-globals" || scenario == "tls-overflow") {
        const std::string path = TempRealCopy(scenario.c_str());
        bool copied = ValidateIdentity(EXECUTION_PROBE_SECONDARY_REAL_XBE, image, error);
        const int64_t offset = copied ? image.Offset(kTlsGlobalZeroFill) : -1;
        copied = copied && CopyAndPatch(EXECUTION_PROBE_SECONDARY_REAL_XBE, path, offset,
                                        scenario == "tls-overflow" ? 0x01 : 0x80);
        xbe::Image patched;
        std::vector<uint8_t> bytes;
        std::string parse_error;
        const bool parsed = copied && ReadFile(path.c_str(), bytes) && patched.Parse(std::move(bytes), parse_error);
        facts = RealFacts{};
        const bool read = parsed && patched.Read32(kTlsGlobalSource, facts.tls_source) &&
                          patched.Read32(kTlsGlobalEnd, facts.tls_end) &&
                          patched.Read32(kTlsGlobalZeroFill, facts.tls_zero);
        std::remove(path.c_str());
        if (!read) return finish("patched TLS globals are not readable", false);
        facts.tls_copy = facts.tls_end - facts.tls_source;
        const bool valid = ValidateTlsContract(facts, error);
        if (valid) return finish("patched TLS globals were accepted", false);
        return finish(error.c_str(), true);
    }
    if (scenario == "tls-contents" || scenario == "seh-corrupt") {
        if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error))
            return finish(error.c_str(), false);
        if (scenario == "tls-contents") {
            const uint32_t pointer = facts.tls + kTlsPointerBytes;
            if (mem::Write(facts.tls, &pointer, sizeof(pointer)) != mem::AccessStatus::kOk ||
                mem::Fill(facts.tls + kTlsPointerBytes, 0, 0x90) != mem::AccessStatus::kOk)
                return finish("could not seed the TLS block", false);
            const uint8_t wrong = 1;
            if (mem::Write(facts.tls + 8, &wrong, sizeof(wrong)) != mem::AccessStatus::kOk)
                return finish("could not corrupt the TLS block", false);
            const bool valid = VerifyTls(facts, error);
            if (valid) return finish("corrupted TLS block was accepted", false);
            return finish(error.c_str(), true);
        }
        const uint32_t S = facts.entry_esp;
        const uint32_t zero = 0, next = 0xFFFFFFFF, handler = kSehHandler, scope = kScopeTable;
        const uint32_t saved_esp = S - 0x38, ethread = facts.ethread, tls4 = facts.tls + 4;
        mem::Write(facts.kpcr, &facts.registration, sizeof(facts.registration));
        mem::Write(facts.registration, &next, sizeof(next));
        mem::Write(facts.registration + 4, &handler, sizeof(handler));
        mem::Write(S - 0x0C, &scope, sizeof(scope));
        mem::Write(S - 0x08, &zero, sizeof(zero));
        mem::Write(S - 0x04, &zero, sizeof(zero));
        mem::Write(S - 0x1C, &saved_esp, sizeof(saved_esp));
        mem::Write(S - 0x38, &zero, sizeof(zero));
        mem::Write(S - 0x34, &zero, sizeof(zero));
        mem::Write(S - 0x30, &zero, sizeof(zero));
        mem::Write(S - 0x24, &ethread, sizeof(ethread));
        mem::Write(S - 0x28, &tls4, sizeof(tls4));
        mem::Write(S - 0x2C, &zero, sizeof(zero));
        const uint32_t wrong = 0xDEADBEEF;
        mem::Write(S - 0x04, &wrong, sizeof(wrong));
        const bool valid = VerifySeh(facts, error);
        if (valid) return finish("corrupted SEH frame was accepted", false);
        return finish(error.c_str(), true);
    }
    if (scenario == "handler-entry" || scenario == "start-entry" || scenario == "next-helper") {
        if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error))
            return finish(error.c_str(), false);
        const uint32_t target = scenario == "handler-entry" ? kSehHandler
                                : scenario == "start-entry" ? kStartRoutine
                                                            : kNextHelper;
        const dispatch::ResolvedTarget resolved = dispatch::ResolveTarget(target);
        if (scenario == "handler-entry") {
            const bool executable = resolved.kind == dispatch::TargetKind::kGeneratedFunction ||
                                    resolved.kind == dispatch::TargetKind::kContinuation ||
                                    resolved.kind == dispatch::TargetKind::kKernelThunk ||
                                    resolved.kind == dispatch::TargetKind::kHostThunk;
            if (executable) return finish("exception handler is mapped executable", false);
            std::printf("RESULT ok exception handler is not executable kind=%s\n",
                        dispatch::TargetKindName(resolved.kind));
            return finish(nullptr, false);
        }
        if (resolved.kind != dispatch::TargetKind::kGeneratedFunction)
            return finish("trap boundary is not a generated function", false);
        std::printf("RESULT ok %s is a trap-only boundary\n",
                    scenario == "start-entry" ? "start routine" : "next helper");
        return finish(nullptr, false);
    }
    if (scenario == "worker-state") {
        if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error))
            return finish(error.c_str(), false);
        if (!kernel::Threads().empty() || kernel::KernelWorkerCountForTest() != 0 ||
            kernel::ThreadWorkerLaunchCountForTest() != facts.worker_baseline)
            return finish("a guest thread or worker became active", false);
        std::printf("RESULT ok no guest thread or worker\n");
        return finish(nullptr, false);
    }
    if (scenario == "prepare-failure") {
        if (!mem::Commit(kOutputPage, mem::kPage)) return finish("could not commit the output page", false);
        kernel::SetThreadPreparationFailureForTest(kernel::ThreadPreparationFailPoint::kAfterStack);
        const uint32_t status = kernel::PrepareSystemThreadEx(
            kRealHandleOut, kStackRequest, kTlsSize, kRealIdOut, kStartRoutine, kStartContext, false, kSystemRoutine, thread);
        kernel::SetThreadPreparationFailureForTest(kernel::ThreadPreparationFailPoint::kNone);
        if (status != kernel::kNoMemory || thread) return finish("preparation failure was not reported", false);
        uint32_t handle = 1;
        if (mem::Read(kRealHandleOut, &handle, sizeof(handle)) != mem::AccessStatus::kOk || handle != 0)
            return finish("failed preparation published a handle", false);
        return finish("thread preparation failed", true);
    }
    if (scenario == "cancellation-failure") {
        if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error))
            return finish(error.c_str(), false);
        thread->ctx = nullptr;
        if (!CancelThread(thread, facts, error)) return finish(error.c_str(), false);
        if (kernel::CancelPreparedThread(thread)) return finish("second cancellation was accepted", false);
        const bool cancelled = thread->state.load() == kernel::ThreadState::kCancelled;
        thread.reset();
        if (!cancelled) return finish("thread is not cancelled", false);
        std::printf("RESULT ok second cancellation refused\n");
        return finish(nullptr, false);
    }
    if (scenario == "preflight") {
        if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error)) {
            std::printf("SETUP fail %s\n", error.c_str());
            return finish(error.c_str(), false);
        }
        PrintFacts(facts);
        return finish(nullptr, false);
    }
    std::printf("SETUP fail unknown secondary real scenario %s\n", scenario.c_str());
    return 4;
}

int RunExecute() {
    if (!mem::Init()) {
        std::printf("SETUP fail memory init\n");
        return 4;
    }
    if (!kernel::RestartKernelWorkers()) {
        std::printf("SETUP fail callback gate\n");
        mem::Shutdown();
        return 4;
    }
    RealFacts facts;
    xbe::Image image;
    kernel::ThreadRef thread;
    X86Context context{};
    kernel::GuestCallbackTicket ticket;
    std::string error;
    auto cleanup = [&]() {
        if (thread) {
            thread->ctx = nullptr;
            std::string cancel_error;
            CancelThread(thread, facts, cancel_error);
        }
        return Teardown(facts.base, true);
    };
    if (!PrepareReal(EXECUTION_PROBE_SECONDARY_REAL_XBE, facts, image, thread, context, ticket, error)) {
        std::printf("SETUP fail %s\n", error.c_str());
        cleanup();
        return 4;
    }
    g_probe = ProbeState{};
    g_probe.running = true;
    g_probe.stack_base = facts.stack_base;
    g_probe.stack_top = facts.stack_top;
    g_probe.kpcr = facts.kpcr;
    g_probe.ethread = facts.ethread;
    g_probe.tls = facts.tls;
    g_probe.worker_baseline = facts.worker_baseline;
    g_probe.snapshot_pages = facts.snapshot_pages;
    g_probe.corrupt_helper_return = false;
    const std::vector<SnapshotPage> snapshot = Snapshot(g_probe.snapshot_pages);
    const std::array<uint32_t, 2> arguments{kStartRoutine, kStartContext};
    uint32_t result = 0;
    bool normal_stop = false;
    try {
        (void)kernel::CallGuestCallbackForTest(context, ticket, kSystemRoutine, arguments, facts.stack_base,
                                               thread->stack_size, result);
        std::printf("RESULT fail reason=\"callback returned without the mandatory stop\"\n");
        cleanup();
        return 3;
    } catch (const ProbeNormalStop&) {
        normal_stop = true;
    } catch (const ProbeFailure& failure) {
        g_probe.running = false;
        PrintFailure(failure);
        cleanup();
        return 3;
    } catch (const X86MemoryFault& fault) {
        g_probe.running = false;
        ProbeFailure failure;
        failure.reason = std::string("guest memory fault: ") + mem::StatusName(fault.status);
        failure.pc = fault.instruction;
        failure.address = fault.address;
        failure.has_address = true;
        PrintFailure(failure);
        cleanup();
        return 3;
    }
    g_probe.running = false;
    if (!normal_stop) {
        std::printf("RESULT fail reason=\"callback did not stop normally\"\n");
        cleanup();
        return 3;
    }
    PrintTrace();
    PrintStop();
    PrintMemoryDiff(snapshot);
    if (!VerifyRealPostconditions(facts, error) || !VerifySnapshotPages(facts, snapshot, error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        cleanup();
        return 3;
    }
    thread->ctx = nullptr;
    if (!CancelThread(thread, facts, error)) {
        std::printf("RESULT fail reason=\"%s\"\n", error.c_str());
        cleanup();
        return 3;
    }
    thread.reset();
    if (!Teardown(facts.base, true)) {
        std::printf("RESULT fail reason=\"teardown failed\"\n");
        return 5;
    }
    std::printf("SECONDARY_REAL_EXEC ok instructions=53 direct_calls=1 kernel_calls=0 import_reads=0 workers=0 "
                "last=002B5C74 stop=002B5C76\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) {
        const std::string scenario = argv[2];
        if (scenario == "execute" || scenario == "supervised-check") {
            if (!SupervisionTokenValid()) {
                std::printf("REFUSED: real secondary execution requires the supervised parent\n");
                return 2;
            }
            if (scenario == "supervised-check") {
                std::printf("RESULT ok supervision=1\n");
                return 0;
            }
            alarm(12);
            return RunExecute();
        }
        return RunPreflight(scenario);
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
    const std::string self = SelfPath();
    if (self.empty()) {
        std::printf("FAIL: cannot locate the real secondary probe executable\n");
        return 1;
    }
    if (std::strcmp(mode, "preflight") == 0) {
        const ChildResult result = SpawnPlainChild(self, "preflight", std::chrono::seconds(15));
        std::printf("%s", result.output.c_str());
        return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 ? 0 : 1;
    }
    if (std::strcmp(mode, "run-once") == 0) {
        if (!execute_real) {
            std::printf("REFUSED: pass --execute-real only after explicit approval\n");
            return 2;
        }
        const ChildResult result = SpawnChild(self, "execute", std::chrono::seconds(15));
        std::printf("%s", result.output.c_str());
        if (result.timed_out) {
            std::printf("SECONDARY REAL EXECUTION FAIL: timeout\n");
            return 1;
        }
        return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 ? 0 : 1;
    }

    struct Case {
        const char* scenario;
        bool expect_ok;
        const char* expected;
    };
    const Case cases[] = {
        {"preflight", true, "PREFLIGHT PASS"},
        {"identity-missing", false, "real XBE is missing"},
        {"identity-mismatch", false, "SHA-1 mismatch"},
        {"route-mismatch", false, "route text/length mismatch"},
        {"route-call-mismatch", false, "route text/length mismatch"},
        {"hook-mismatch", false, "hook coverage"},
        {"bad-ticket", false, "callback ticket"},
        {"bad-fs", false, "FS base"},
        {"bad-kpcr", false, "fs:[0]"},
        {"bad-ethread", false, "ETHREAD"},
        {"read-only-kpcr", false, "KPCR is not writable"},
        {"uncommitted-stack", false, "stack is not committed"},
        {"tls-globals", false, "TLS globals"},
        {"tls-overflow", false, "exceed"},
        {"tls-contents", false, "TLS zero-fill mismatch"},
        {"seh-corrupt", false, "SEH saved"},
        {"handler-entry", true, "exception handler is not executable"},
        {"start-entry", true, "start routine is a trap-only"},
        {"next-helper", true, "next helper is a trap-only"},
        {"worker-state", true, "no guest thread or worker"},
        {"prepare-failure", false, "thread preparation failed"},
        {"cancellation-failure", true, "second cancellation refused"},
    };
    int failed = 0;
    for (const Case& test : cases) {
        const ChildResult result = SpawnPlainChild(self, test.scenario, std::chrono::seconds(15));
        const int exit_code = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : -1;
        const int expected_exit = test.expect_ok ? 0 : 3;
        const bool condition = !result.timed_out && exit_code == expected_exit &&
                               result.output.find(test.expected) != std::string::npos &&
                               result.output.find("TEARDOWN system_used=0 contiguous_used=0 virtual_committed=0 "
                                                  "workers=0 initialized=0") != std::string::npos;
        std::printf("%s: real secondary child %s %s\n", condition ? "PASS" : "FAIL", test.scenario,
                    test.expect_ok ? "completes" : "rejects");
        if (!condition) ++failed;
    }

    const ChildResult unauthorised = SpawnPlainChild(self, "execute", std::chrono::seconds(5));
    const int unauthorised_exit = WIFEXITED(unauthorised.status) ? WEXITSTATUS(unauthorised.status) : -1;
    const bool unauthorised_ok = !unauthorised.timed_out && unauthorised_exit == 2 &&
                                 unauthorised.output.find("REFUSED") != std::string::npos;
    std::printf("%s: direct --child execute is refused without the supervised parent\n",
                unauthorised_ok ? "PASS" : "FAIL");
    if (!unauthorised_ok) ++failed;

    const ChildResult supervised = SpawnChild(self, "supervised-check", std::chrono::seconds(5));
    const int supervised_exit = WIFEXITED(supervised.status) ? WEXITSTATUS(supervised.status) : -1;
    const bool supervised_ok = !supervised.timed_out && supervised_exit == 0 &&
                               supervised.output.find("RESULT ok supervision=1") != std::string::npos;
    std::printf("%s: supervised token/fd pathway is wired without execution\n", supervised_ok ? "PASS" : "FAIL");
    if (!supervised_ok) ++failed;

    if (execute_real) {
        std::printf("REFUSED: --execute-real requires --run-once\n");
        ++failed;
    }
    std::printf("real secondary probe: %s\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}
