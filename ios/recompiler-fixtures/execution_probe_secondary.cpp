// Phase 2C-31 test-only secondary-thread entry fixture.
//
// This binary has three modes:
//   * a parent/self-test mode that spawns one disposable child per scenario,
//     enforces an external timeout, and checks the child's structured report;
//   * a child mode that prepares the synthetic secondary-entry image, builds a
//     prepared-but-inactive guest thread, validates the callback ticket, runs
//     only the first two system-routine instructions, stops before the SEH
//     prolog, and tears everything down;
//   * a static mode that validates the authentic XBE's secondary-entry bytes
//     without executing any guest instruction.
//
// It never calls kernel::Boot(), never starts a worker, never invokes a guest
// callback through production CallGuestCallback and never executes the start
// routine or either unreviewed callee.

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
    Clock::time_point deadline{};
    bool deadline_set = false;
    std::chrono::milliseconds delay{0};
    uint32_t delay_pc = 0;
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

void RecordStep(uint32_t pc, X86Context& ctx) {
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
    g_probe.trace.push_back(record);
}

void SecondaryStep(uint32_t pc, X86Context& ctx) {
    if (!g_probe.running) Fail("instruction step outside a probe run", pc);
    if (g_probe.in_step) Fail("re-entry during a step", pc);
    InStep guard;

    // trap-only boundaries are mapped for callback resolution, never executed
    if (pc == kStartRoutine) Fail("start routine entry is forbidden", pc);
    if (pc == kStackHelper) Fail("stack helper entry is forbidden", pc);
    if (pc == kSehHelper) Fail("SEH helper entry is forbidden", pc);

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
        if (!previous || pc != previous->next) Fail("unexpected control transfer", pc);
    }

    // the stop boundary is not an executed instruction: it is checked before
    // the budget so a prefix of exactly kMaximumInstructions can stop here
    if (step->kind == StepKind::kStop) {
        if (g_probe.executed != kMaximumInstructions) Fail("stop reached before the reviewed prefix completed", pc);
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

    if (g_probe.delay.count() && g_probe.delay_pc == pc) std::this_thread::sleep_for(g_probe.delay);

    RecordStep(pc, ctx);
    ++g_probe.executed;
    g_probe.previous_pc = pc;
    g_probe.have_previous = true;
}

}  // namespace

// external linkage: the generated code calls these by name
void SecondaryStep_0x002B5C0F(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C0F, ctx); }
void SecondaryStep_0x002B5C11(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C11, ctx); }
void SecondaryStep_0x002B5C16(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B5C16, ctx); }
void SecondaryStep_0x002B38A2(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B38A2, ctx); }
void SecondaryStep_0x002B58B8(X86Context& ctx, uint8_t*) { SecondaryStep(0x002B58B8, ctx); }
void SecondaryStep_0x00319804(X86Context& ctx, uint8_t*) { SecondaryStep(0x00319804, ctx); }

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

// only the prepared thread's stack may be touched, and only by writes; the
// two reviewed pushes are the whole guest-memory effect of this route.
void execution_probe_memory_access(uint32_t instruction, uint32_t address, std::size_t size, bool write) {
    if (!g_probe.running) Fail("memory access outside a probe run", instruction, address, true);
    if (size == 0) Fail("zero-length memory access", instruction, address, true);
    if (g_probe.memory.size() >= kMaximumMemoryOperations)
        Fail("memory-operation budget exhausted", instruction, address, true);
    const uint64_t end = uint64_t(address) + size;
    if (end > (uint64_t(1) << 32)) Fail("memory access overflows the guest address space", instruction, address, true);
    if (address < 0x003C9000 || address >= 0x003CA000) {
        if (write && address >= g_probe.stack_base && end <= g_probe.stack_top) {
            g_probe.memory.push_back({instruction, address, uint32_t(size), write});
            return;
        }
        Fail("memory access outside the reviewed stack span", instruction, address, true);
    }
    Fail("unreviewed import page access", instruction, address, true);
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

bool PrepareFixture(xbe::Image& image, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(EXECUTION_PROBE_SECONDARY_XBE, bytes)) {
        error = std::string("cannot read ") + EXECUTION_PROBE_SECONDARY_XBE;
        return false;
    }
    if (!image.Parse(std::move(bytes), error)) return false;
    if (image.entry != kEntry) {
        error = "synthetic fixture entry does not match the secondary route";
        return false;
    }
    const std::string sha1 = Sha1Hex(image.file);
    if (!xbe::PrepareImage(image, kEntry, sha1, error)) return false;
    kernel::g_base = mem::Base();
    std::string dispatch_error;
    if (!dispatch::Init(dispatch_error)) {
        error = "dispatch init failed: " + dispatch_error;
        return false;
    }
    return true;
}

struct SnapshotPage {
    uint32_t page;
    std::array<uint8_t, mem::kPage> bytes{};
    bool valid = false;
};

std::vector<SnapshotPage> Snapshot() {
    std::vector<SnapshotPage> pages;
    for (uint32_t page : g_probe.snapshot_pages) {
        SnapshotPage snapshot;
        snapshot.page = page;
        snapshot.valid = mem::Read(page, snapshot.bytes.data(), snapshot.bytes.size()) == mem::AccessStatus::kOk;
        pages.push_back(std::move(snapshot));
    }
    return pages;
}

uint32_t WritePcFor(uint32_t address) {
    for (auto it = g_probe.memory.rbegin(); it != g_probe.memory.rend(); ++it)
        if (it->write && address >= it->address && address + 4 <= uint64_t(it->address) + it->size) return it->pc;
    return 0;
}

unsigned PrintMemoryDiff(const std::vector<SnapshotPage>& before) {
    unsigned changes = 0;
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
            std::printf("MEM pc=%08X address=%08X before=%08X after=%08X\n", WritePcFor(address), address, old_value,
                        new_value);
            ++changes;
        }
    }
    std::printf("guest_memory_changes=%u\n", changes);
    return changes;
}

void PrintTrace() {
    for (const StepRecord& step : g_probe.trace)
        std::printf("STEP %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
                    "flags=%u%u%u%u%u%u\n",
                    step.pc, step.eax, step.ecx, step.edx, step.ebx, step.esp, step.ebp, step.esi, step.edi, step.cf,
                    step.zf, step.sf, step.of, step.pf, step.af);
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

bool Teardown(uint32_t image_base, bool data_committed) {
    kernel::StopKernelWorkers();
    dispatch::Reset();
    if (image_base) {
        uint32_t base = image_base, size = 0;
        mem::FreeVirtual(base, size, mem::kMemRelease);
    }
    if (data_committed) mem::Decommit(kDataPage, mem::kPage);
    const mem::Stats stats = mem::Query();
    const size_t workers = kernel::KernelWorkerCountForTest();
    mem::Shutdown();
    std::printf("TEARDOWN system_used=%u contiguous_used=%u virtual_committed=%u workers=%zu initialized=%d\n",
                stats.system_used, stats.contiguous_used, stats.virtual_committed, workers, int(mem::IsInitialized()));
    return stats.system_used == 0 && stats.contiguous_used == 0 && stats.virtual_committed == 0 && workers == 0 &&
           !mem::IsInitialized();
}

bool ValidateContext(const kernel::ThreadRef& thread, const X86Context& ctx, std::string& error) {
    if (!thread) {
        error = "prepared thread is missing";
        return false;
    }
    if (thread->state.load() != kernel::ThreadState::kPrepared) {
        error = "prepared thread is not in kPrepared";
        return false;
    }
    if (thread->suspend.load() != 0) {
        error = "prepared thread is suspended";
        return false;
    }
    if (!kernel::Threads().empty()) {
        error = "thread became active during preparation";
        return false;
    }
    if (thread->system_routine != kSystemRoutine || thread->start_routine != kStartRoutine ||
        thread->start_context != kStartContext) {
        error = "prepared-thread routine arguments are not authentic";
        return false;
    }
    const uint32_t expected_esp = (thread->tls & ~15u) - 16;
    if (ctx.esp != expected_esp) {
        error = "initial ESP does not match the production InitContext contract";
        return false;
    }
    if (ctx.esp % 16 != 0) {
        error = "initial ESP is not 16-byte aligned";
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
    if (!thread->stack_top || !thread->stack_size || ctx.esp < stack_base + kCallbackFrameBytes + kGuestStackBytes ||
        ctx.esp > thread->stack_top) {
        error = "initial ESP is outside the owned stack's callback region";
        return false;
    }
    if (thread->tls < stack_base || thread->tls + kTlsSize > thread->stack_top) {
        error = "TLS block is not inside the owned stack";
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
    if (thread->ctx != &ctx) {
        error = "prepared thread does not point at the fixture context";
        return false;
    }
    return true;
}

bool ValidateCallbackContracts(std::string& error) {
    const kernel::GuestCallbackContract& system = kernel::CallbackContract(kernel::GuestCallbackClass::kSystemRoutine);
    if (system.callback_class != kernel::GuestCallbackClass::kSystemRoutine || system.variadic ||
        system.arguments.size() != 2 ||
        system.arguments[0].kind != kernel::GuestCallbackArgumentKind::kGuestFunction ||
        system.arguments[1].kind != kernel::GuestCallbackArgumentKind::kNullableUnresolvedPointer) {
        error = "system-routine callback contract is not (guest function, nullable pointer)";
        return false;
    }
    const kernel::GuestCallbackContract& start = kernel::CallbackContract(kernel::GuestCallbackClass::kThreadStart);
    if (start.callback_class != kernel::GuestCallbackClass::kThreadStart || start.variadic ||
        start.arguments.size() != 1 ||
        start.arguments[0].kind != kernel::GuestCallbackArgumentKind::kNullableUnresolvedPointer) {
        error = "thread-start callback contract is not (nullable pointer)";
        return false;
    }
    return true;
}

bool ValidateGeneratedMapping(std::string& error) {
    std::vector<uint8_t> header_bytes, source_bytes;
    const std::string header_path =
        std::string(EXECUTION_PROBE_SECONDARY_GENERATED_DIR) + "/x86_recomp_shared.h";
    const std::string source_path =
        std::string(EXECUTION_PROBE_SECONDARY_GENERATED_DIR) + "/x86_recomp.0.cpp";
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

    for (const RouteStep& step : kRoute) {
        char name[64];
        std::snprintf(name, sizeof(name), "SecondaryStep_0x%08X(", step.pc);
        if (CountOccurrences(header, name) != 1 || CountOccurrences(source, name) != 1) {
            error = std::string("route hook coverage mismatch at ") + name;
            return false;
        }
    }
    for (uint32_t pc : kTrapPcs) {
        char name[64];
        std::snprintf(name, sizeof(name), "SecondaryStep_0x%08X(", pc);
        if (CountOccurrences(header, name) != 1 || CountOccurrences(source, name) != 1) {
            error = std::string("trap hook coverage mismatch at ") + name;
            return false;
        }
    }
    const size_t expected_hooks = kRouteCount + kTrapCount;
    if (CountOccurrences(source, "SecondaryStep_0x") != expected_hooks ||
        CountOccurrences(header, "SecondaryStep_0x") != expected_hooks) {
        error = "generated hook counts are not exactly the reviewed set";
        return false;
    }
    if (CountOccurrences(source, "SecondaryStop_") != 0 || CountOccurrences(source, "ProbeStop_") != 0) {
        error = "generated source contains a post-instruction stop hook";
        return false;
    }
    if (CountOccurrences(source, "X86_FUNC(__imp__sub_") != 4 ||
        source.find("X86_FUNC(__imp__sub_002B5C0F)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_002B38A2)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_002B58B8)") == std::string::npos ||
        source.find("X86_FUNC(__imp__sub_00319804)") == std::string::npos) {
        error = "generated source has an unexpected function set";
        return false;
    }
    const std::string call_seh = "x86_checked_call(ctx, base, 0x319804u, 0x2B5C1Bu, 0x2B5C16u)";
    const std::string call_start = "x86_checked_call(ctx, base, 0x2B58B8u, 0x2B38A7u, 0x2B38A2u)";
    if (source.find(call_seh) == std::string::npos || source.find(call_start) == std::string::npos ||
        CountOccurrences(source, "x86_checked_call") != 2) {
        error = "generated source does not contain exactly the two reviewed calls";
        return false;
    }
    if (CountOccurrences(source, "x86_checked_ret") != 0) {
        error = "generated source contains an unexpected RET";
        return false;
    }
    for (const char* forbidden : {"x86_icall", "x86_kernel", "x86_resume", "ProbeStep_"}) {
        if (source.find(forbidden) != std::string::npos) {
            error = std::string("generated source contains ") + forbidden;
            return false;
        }
    }
    struct HookOrder {
        const char* hook;
        const char* instruction;
    };
    const HookOrder orders[] = {
        {"SecondaryStep_0x002B5C16(ctx", "// 002B5C16 call 0x00319804"},
        {"SecondaryStep_0x002B38A2(ctx", "// 002B38A2 call 0x002B58B8"},
        {"SecondaryStep_0x002B58B8(ctx", "// 002B58B8 mov eax, [0x003C9E34]"},
        {"SecondaryStep_0x00319804(ctx", "// 00319804 push 0x319858"},
    };
    for (const HookOrder& order : orders) {
        const size_t hook = source.find(order.hook);
        const size_t instruction = source.find(order.instruction);
        if (hook == std::string::npos || instruction == std::string::npos || hook > instruction) {
            error = std::string("the hook is missing or misplaced before ") + order.instruction;
            return false;
        }
    }
    for (const char* instruction : {"// 002B5C0F push 0x18", "// 002B5C11 push 0x3ED5B8"}) {
        if (source.find(instruction) == std::string::npos) {
            error = std::string("the reviewed instruction text is missing: ") + instruction;
            return false;
        }
    }
    for (const char* after_boundary : {"// 002B5C1B", "// 002B38A7", "// 002B58BD", "// 00319809"}) {
        if (source.find(after_boundary) != std::string::npos) {
            error = "generated source decoded past a reviewed boundary";
            return false;
        }
    }
    return true;
}

bool StaticValidateSecondary(const std::string& path, std::string& report) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path.c_str(), bytes)) {
        report = "real XBE is missing: " + path;
        return false;
    }
    xbe::Image image;
    std::string error;
    if (!image.Parse(std::move(bytes), error)) {
        report = "parse failed: " + error;
        return false;
    }
    if (Sha1Hex(image.file) != kRealSha1) {
        report = "real XBE SHA-1 mismatch";
        return false;
    }
    if (image.title_id != kRealTitle) {
        report = "real XBE title ID mismatch";
        return false;
    }
    if (image.entry != kRealEntry) {
        report = "real XBE entry mismatch";
        return false;
    }
    if (image.base != kRealImageBase) {
        report = "real XBE base mismatch";
        return false;
    }

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC, ZYAN_FALSE);

    auto decode = [&](uint32_t pc, std::string& text, ZydisDecodedInstruction& instruction,
                      ZydisDecodedOperand* operands) -> bool {
        const int64_t offset = image.Offset(pc);
        const size_t span = image.FileSpan(pc);
        if (offset < 0 || span == 0) return false;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction,
                                                 operands)))
            return false;
        char buffer[256];
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible,
                                        buffer, sizeof(buffer), pc, nullptr);
        text = buffer;
        return true;
    };

    uint64_t hash = 0xcbf29ce484222325ULL;
    size_t route_bytes = 0;
    for (size_t i = 0; i < kRouteCount; ++i) {
        const RouteStep& step = kRoute[i];
        std::string text;
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode(step.pc, text, instruction, operands)) {
            report = "secondary route instruction did not decode";
            return false;
        }
        if (text != kExpectedRouteText[i] || instruction.length != step.length) {
            report = std::string("secondary route text/length mismatch at ") + std::to_string(step.pc);
            return false;
        }
        if (step.kind == StepKind::kStop) {
            if (operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                uint32_t(step.pc + instruction.length + operands[0].imm.value.s) != kSehHelper) {
                report = "secondary stop is not the reviewed SEH-helper call";
                return false;
            }
        }
        const int64_t offset = image.Offset(step.pc);
        for (uint32_t j = 0; j < step.length; ++j) {
            hash ^= image.file[size_t(offset) + j];
            hash *= 0x100000001b3ULL;
        }
        route_bytes += step.length;
    }
    if (route_bytes != kExpectedRouteBytes || hash != kExpectedRouteHash) {
        report = "secondary route byte hash mismatch";
        return false;
    }

    for (size_t i = 0; i < kTrapCount; ++i) {
        const uint32_t pc = kTrapPcs[i];
        std::string text;
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode(pc, text, instruction, operands)) {
            report = std::string("trap boundary did not decode at ") + std::to_string(pc);
            return false;
        }
        if (text != kExpectedTrapText[i] || instruction.length != 5) {
            report = std::string("trap boundary text/length mismatch at ") + std::to_string(pc);
            return false;
        }
        if (pc == kStartRoutine &&
            (instruction.mnemonic != ZYDIS_MNEMONIC_CALL || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
             uint32_t(pc + instruction.length + operands[0].imm.value.s) != kStackHelper)) {
            report = "start routine entry is not the reviewed call";
            return false;
        }
    }
    if (kSystemRoutine == image.entry) {
        report = "system routine is the XBE entrypoint";
        return false;
    }
    char line[320];
    std::snprintf(line, sizeof(line),
                  "real secondary: sha1=%s title=%08X entry=%08X base=%08X system=%08X start=%08X stop=%08X "
                  "call=%08X route=%zu/%u bytes hash=%016llX",
                  kRealSha1, image.title_id, image.entry, image.base, kSystemRoutine, kStartRoutine, kStopBefore,
                  kSehHelper, kRouteCount, kExpectedRouteBytes, static_cast<unsigned long long>(hash));
    report = line;
    return true;
}

struct ChildResult {
    int status = -1;
    bool timed_out = false;
    std::string output;
    std::chrono::milliseconds elapsed{0};
};

ChildResult SpawnChild(const std::string& self, const std::string& scenario, std::chrono::milliseconds timeout) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
    const pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl(self.c_str(), self.c_str(), "--child", scenario.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ChildResult result;
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }
    close(pipefd[1]);
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
        const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
        if (count <= 0) break;
        if (result.output.size() < 65536) result.output.append(buffer, size_t(count));
    }
    close(pipefd[0]);
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

enum class Scenario {
    kPrepare,
    kBudget,
    kDeadline,
    kStackDenied,
    kCodePageDenied,
    kSystemRoutineUnmapped,
    kStartRoutineUnmapped,
    kWrongTicketClass,
    kStartRoutineEntry,
    kStackHelperEntry,
    kSehEntry,
    kPrepareFailure,
    kParentTimeout,
};

bool ParseScenario(const std::string& name, Scenario& scenario) {
    if (name == "prepare") scenario = Scenario::kPrepare;
    else if (name == "budget") scenario = Scenario::kBudget;
    else if (name == "deadline") scenario = Scenario::kDeadline;
    else if (name == "stack-denied") scenario = Scenario::kStackDenied;
    else if (name == "code-page-denied") scenario = Scenario::kCodePageDenied;
    else if (name == "system-routine-unmapped") scenario = Scenario::kSystemRoutineUnmapped;
    else if (name == "start-routine-unmapped") scenario = Scenario::kStartRoutineUnmapped;
    else if (name == "wrong-ticket-class") scenario = Scenario::kWrongTicketClass;
    else if (name == "start-routine-entry") scenario = Scenario::kStartRoutineEntry;
    else if (name == "stack-helper-entry") scenario = Scenario::kStackHelperEntry;
    else if (name == "seh-entry") scenario = Scenario::kSehEntry;
    else if (name == "prepare-failure") scenario = Scenario::kPrepareFailure;
    else if (name == "parent-timeout") scenario = Scenario::kParentTimeout;
    else return false;
    return true;
}

bool CancelPreparedThread(const kernel::ThreadRef& thread, std::string& error) {
    if (!thread) {
        error = "prepared thread is missing";
        return false;
    }
    const uint32_t handle = thread->published_handle;
    const uint32_t stack_top = thread->stack_top;
    uint32_t published = 0;
    const bool handle_read = mem::Read(kHandleOut, &published, sizeof(published)) == mem::AccessStatus::kOk;
    const bool prepared = thread->state.load() == kernel::ThreadState::kPrepared && handle && handle_read &&
                          published == handle && kernel::FromHandle(handle).get() == thread.get() &&
                          mem::IsCommitted(stack_top - 1);
    const bool cancelled = kernel::CancelPreparedThread(thread);
    uint32_t cleared = 1;
    uint32_t id_cleared = 1;
    const bool output_cleared =
        mem::Read(kHandleOut, &cleared, sizeof(cleared)) == mem::AccessStatus::kOk && cleared == 0;
    const bool id_ok = mem::Read(kIdOut, &id_cleared, sizeof(id_cleared)) == mem::AccessStatus::kOk &&
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
    return ok;
}

int RunChild(const std::string& name) {
    alarm(12);  // child-side fallback; the parent's watchdog is the primary limit
    Scenario scenario;
    if (!ParseScenario(name, scenario)) {
        std::printf("SETUP fail unknown scenario %s\n", name.c_str());
        return 4;
    }
    if (!mem::Init()) {
        std::printf("SETUP fail memory init\n");
        return 4;
    }
    if (!kernel::RestartKernelWorkers()) {
        std::printf("SETUP fail callback gate\n");
        mem::Shutdown();
        return 4;
    }

    xbe::Image image;
    std::string error;
    uint32_t image_base = 0;
    bool data_committed = false;
    kernel::ThreadRef thread;
    X86Context context{};
    bool context_initialized = false;

    auto finish = [&](const char* failure, bool expected_failure) {
        bool cancel_ok = true;
        if (thread) {
            if (context_initialized) thread->ctx = nullptr;
            std::string cancel_error;
            if (!CancelPreparedThread(thread, cancel_error)) {
                cancel_ok = false;
                if (!failure) failure = "prepared-thread cancellation failed";
            }
        }
        if (failure) {
            ProbeFailure report;
            report.reason = failure;
            report.pc = g_probe.have_stop ? g_probe.stop.pc : (g_probe.executed ? g_probe.trace.back().pc : kEntry);
            PrintFailure(report);
        }
        g_probe.running = false;
        const bool teardown_ok = Teardown(image_base, data_committed);
        const bool result = !failure && cancel_ok && teardown_ok && !expected_failure;
        std::printf("RESULT %s instructions=%u direct_calls=%u kernel_calls=%u import_reads=%u workers=%zu stop=%08X\n",
                    result ? "ok" : "fail", g_probe.executed, g_probe.direct_calls, g_probe.kernel_calls,
                    g_probe.import_reads, kernel::KernelWorkerCountForTest(), kStopBefore);
        return result ? 0 : ((cancel_ok && teardown_ok) ? 3 : 5);
    };

    if (!PrepareFixture(image, error)) {
        std::printf("SETUP fail %s\n", error.c_str());
        kernel::StopKernelWorkers();
        mem::Shutdown();
        return 4;
    }
    image_base = image.base;
    if (mem::Protect(kSectionVa, kSectionSize, mem::kRead | mem::kExecute) != mem::AccessStatus::kOk)
        return finish("code-page protection failed", false);
    if (!mem::Commit(kDataPage, mem::kPage)) return finish("data page commit failed", false);
    data_committed = true;

    if (scenario == Scenario::kCodePageDenied) {
        if (mem::Protect(kSectionVa, kSectionSize, mem::kRead) != mem::AccessStatus::kOk)
            return finish("could not remove execute permission", false);
        kernel::GuestCallbackTicket ticket;
        const kernel::GuestCallbackStatus status =
            kernel::PrepareGuestCallback(kSystemRoutine, kernel::GuestCallbackClass::kSystemRoutine, ticket);
        if (status == kernel::GuestCallbackStatus::kCodePageDenied) {
            std::printf("EXPECTED code page denied\n");
            return finish("expected: code page denied", true);
        }
        return finish("callback target with a non-executable code page was accepted", false);
    }

    if (scenario == Scenario::kSystemRoutineUnmapped) {
        kernel::GuestCallbackTicket ticket;
        const kernel::GuestCallbackStatus status =
            kernel::PrepareGuestCallback(0x002B4000, kernel::GuestCallbackClass::kSystemRoutine, ticket);
        if (status == kernel::GuestCallbackStatus::kRejectedTarget) {
            std::printf("EXPECTED system routine target rejected\n");
            return finish("expected: system routine target rejected", true);
        }
        return finish("an unmapped system routine was accepted", false);
    }

    if (scenario == Scenario::kPrepareFailure) {
        kernel::SetThreadPreparationFailureForTest(kernel::ThreadPreparationFailPoint::kAfterStack);
        const uint32_t status = kernel::PrepareSystemThreadEx(
            kHandleOut, kStackRequest, kTlsSize, kIdOut, kStartRoutine, kStartContext, false, kSystemRoutine, thread);
        kernel::SetThreadPreparationFailureForTest(kernel::ThreadPreparationFailPoint::kNone);
        if (status != kernel::kNoMemory || thread)
            return finish("thread-preparation failure was not reported", false);
        uint32_t handle = 0;
        if (mem::Read(kHandleOut, &handle, sizeof(handle)) != mem::AccessStatus::kOk || handle != 0)
            return finish("failed preparation published a handle", false);
        std::printf("EXPECTED thread preparation failed status=%u\n", status);
        return finish("expected: thread preparation failed", true);
    }

    const uint32_t status = kernel::PrepareSystemThreadEx(
        kHandleOut, kStackRequest, kTlsSize, kIdOut, kStartRoutine, kStartContext, false, kSystemRoutine, thread);
    if (status != kernel::kSuccess || !thread) {
        std::printf("SETUP fail thread preparation status=%u\n", status);
        return finish("thread preparation failed", false);
    }
    if (!kernel::InitializePreparedThreadContextForTest(thread, context)) {
        std::printf("SETUP fail prepared context\n");
        return finish("prepared context initialization failed", false);
    }
    context_initialized = true;
    if (!ValidateContext(thread, context, error)) return finish(error.c_str(), false);
    if (!ValidateCallbackContracts(error)) return finish(error.c_str(), false);

    const uint32_t stack_base = thread->stack_top - thread->stack_size;
    std::printf("CONTEXT esp=%08X fs=%08X tls=%08X kpcr=%08X ethread=%08X fcw=%04X mxcsr=%04X stack=%08X-%08X "
                "handle=%08X state=prepared suspend=%d\n",
                context.esp, context.fs_base, thread->tls, thread->kpcr, thread->ethread, context.fcw, context.mxcsr,
                stack_base, thread->stack_top, thread->published_handle, thread->suspend.load());

    kernel::GuestCallbackTicket ticket;
    const kernel::GuestCallbackClass ticket_class =
        scenario == Scenario::kWrongTicketClass ? kernel::GuestCallbackClass::kThreadStart
                                                : kernel::GuestCallbackClass::kSystemRoutine;
    const kernel::GuestCallbackStatus ticket_status =
        kernel::PrepareGuestCallback(kSystemRoutine, ticket_class, ticket);
    if (ticket_status != kernel::GuestCallbackStatus::kReady)
        return finish("callback ticket was rejected", false);

    g_probe = ProbeState{};
    g_probe.running = true;
    g_probe.stack_base = stack_base;
    g_probe.stack_top = thread->stack_top;
    g_probe.worker_baseline = kernel::ThreadWorkerLaunchCountForTest();
    g_probe.snapshot_pages = {uint32_t((thread->stack_top - 1) & ~(mem::kPage - 1)), kDataPage};
    if (scenario == Scenario::kBudget) g_probe.instruction_budget = 1;
    if (scenario == Scenario::kDeadline) {
        g_probe.deadline = Clock::now() + std::chrono::milliseconds(10);
        g_probe.deadline_set = true;
        g_probe.delay = std::chrono::milliseconds(50);
        g_probe.delay_pc = kEntry;
    }
    if (scenario == Scenario::kParentTimeout) {
        g_probe.delay = std::chrono::seconds(10);
        g_probe.delay_pc = kEntry;
    }

    if (scenario == Scenario::kStackDenied && !mem::Decommit(stack_base, mem::kPage))
        return finish("could not decommit the stack base page", false);

    if (scenario == Scenario::kStartRoutineEntry || scenario == Scenario::kStackHelperEntry ||
        scenario == Scenario::kSehEntry) {
        const uint32_t target = scenario == Scenario::kStartRoutineEntry ? kStartRoutine
                                : scenario == Scenario::kStackHelperEntry ? kStackHelper
                                                                          : kSehHelper;
        const dispatch::ResolvedTarget resolved = dispatch::ResolveTarget(target);
        if (resolved.kind != dispatch::TargetKind::kGeneratedFunction || !resolved.function)
            return finish("trap boundary is not a generated function", false);
        try {
            resolved.function(context, mem::Base());
            return finish("trap boundary returned instead of failing closed", false);
        } catch (const ProbeFailure& failure) {
            g_probe.running = false;
            PrintFailure(failure);
            if (failure.reason.find("entry is forbidden") == std::string::npos)
                return finish("trap boundary failed for the wrong reason", false);
            std::printf("EXPECTED trap boundary failed closed\n");
            return finish("expected: trap boundary failed closed", true);
        }
    }

    std::array<uint32_t, 2> arguments{kStartRoutine, kStartContext};
    if (scenario == Scenario::kStartRoutineUnmapped) arguments[0] = 0x002B4000;
    const std::vector<SnapshotPage> snapshot = Snapshot();
    uint32_t result = 0;
    bool normal_stop = false;
    try {
        const kernel::GuestCallbackStatus callback_status = kernel::CallGuestCallbackForTest(
            context, ticket, kSystemRoutine, arguments, stack_base, thread->stack_size, result);
        if (scenario == Scenario::kStackDenied &&
            callback_status == kernel::GuestCallbackStatus::kStackWriteDenied) {
            std::printf("EXPECTED stack write denied\n");
            return finish("expected: stack write denied", true);
        }
        if (scenario == Scenario::kStartRoutineUnmapped &&
            callback_status == kernel::GuestCallbackStatus::kArgumentRejected) {
            std::printf("EXPECTED start routine argument rejected\n");
            return finish("expected: start routine argument rejected", true);
        }
        if (scenario == Scenario::kWrongTicketClass &&
            callback_status == kernel::GuestCallbackStatus::kArgumentRejected) {
            std::printf("EXPECTED callback argument contract rejected\n");
            return finish("expected: callback argument contract rejected", true);
        }
        return finish("callback returned without the mandatory stop", false);
    } catch (const ProbeNormalStop&) {
        normal_stop = true;
    } catch (const ProbeFailure& failure) {
        g_probe.running = false;
        PrintFailure(failure);
        if (scenario == Scenario::kBudget &&
            failure.reason.find("instruction budget exhausted") != std::string::npos) {
            std::printf("EXPECTED instruction budget exhausted\n");
            return finish("expected: instruction budget exhausted", true);
        }
        if (scenario == Scenario::kDeadline &&
            failure.reason.find("instruction deadline exceeded") != std::string::npos) {
            std::printf("EXPECTED instruction deadline exceeded\n");
            return finish("expected: instruction deadline exceeded", true);
        }
        return finish(failure.reason.c_str(), false);
    } catch (const X86MemoryFault& fault) {
        g_probe.running = false;
        ProbeFailure failure;
        failure.reason = std::string("guest memory fault: ") + mem::StatusName(fault.status);
        failure.pc = fault.instruction;
        failure.address = fault.address;
        failure.has_address = true;
        PrintFailure(failure);
        return finish(failure.reason.c_str(), false);
    }
    g_probe.running = false;
    if (!normal_stop) return finish("callback did not stop normally", false);

    PrintTrace();
    PrintStop();
    const unsigned changes = PrintMemoryDiff(snapshot);

    if (scenario != Scenario::kPrepare) return finish("an unexpected scenario reached the success path", false);

    if (!g_probe.have_stop || g_probe.stop.pc != kStopBefore)
        return finish("mandatory stop is not the SEH call", false);
    if (g_probe.executed != kMaximumInstructions || g_probe.trace.size() != kMaximumInstructions)
        return finish("reviewed prefix instruction count mismatch", false);
    if (g_probe.trace[0].pc != kEntry || g_probe.trace[1].pc != 0x002B5C11)
        return finish("reviewed prefix PC sequence mismatch", false);
    if (g_probe.direct_calls != 0 || g_probe.kernel_calls != 0 || g_probe.import_reads != 0)
        return finish("unexpected call or import activity", false);
    if (!g_probe.stop_frame_clear) return finish("checked call frame was not clear at the stop", false);
    if (g_probe.stop.esp != context.esp - kCallbackFrameBytes - kGuestStackBytes)
        return finish("stack pointer is not the reviewed post-push value", false);
    if (g_probe.stop.eax != 0 || g_probe.stop.ecx != 0 || g_probe.stop.edx != 0 || g_probe.stop.ebx != 0 ||
        g_probe.stop.ebp != 0 || g_probe.stop.esi != 0 || g_probe.stop.edi != 0)
        return finish("register state changed before the stop", false);
    if (changes != 4) return finish("guest-memory change count is not the reviewed four words", false);
    if (g_probe.memory.size() != 2 || !g_probe.memory[0].write || !g_probe.memory[1].write)
        return finish("the two reviewed pushes were not the only guest accesses", false);
    if (!kernel::Threads().empty() || kernel::ThreadWorkerLaunchCountForTest() != g_probe.worker_baseline)
        return finish("a guest thread or worker became active", false);

    uint32_t push0 = 0, push1 = 0, sentinel = 0, start = 0, start_context = 0;
    if (mem::Read(g_probe.stop.esp, &push0, sizeof(push0)) != mem::AccessStatus::kOk ||
        mem::Read(g_probe.stop.esp + 4, &push1, sizeof(push1)) != mem::AccessStatus::kOk ||
        mem::Read(g_probe.stop.esp + 8, &sentinel, sizeof(sentinel)) != mem::AccessStatus::kOk ||
        mem::Read(g_probe.stop.esp + 12, &start, sizeof(start)) != mem::AccessStatus::kOk ||
        mem::Read(g_probe.stop.esp + 16, &start_context, sizeof(start_context)) != mem::AccessStatus::kOk)
        return finish("could not read the callback frame at the stop", false);
    if (push0 != kSecondPushValue || push1 != kFirstPushValue || sentinel != kHostCallbackReturn ||
        start != kStartRoutine || start_context != kStartContext)
        return finish("callback frame or pushed values are not authentic", false);
    std::printf("FRAME sentinel=%08X start=%08X context=%08X push0=%08X push1=%08X\n", sentinel, start,
                start_context, push0, push1);

    return finish(nullptr, false);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return RunChild(argv[2]);

    if (argc == 3 && std::strcmp(argv[1], "--static-real") == 0) {
        std::string report;
        const bool ok = StaticValidateSecondary(argv[2], report);
        std::printf("SECONDARY static %s\n%s\n", ok ? "PASS" : "FAIL", report.c_str());
        return ok ? 0 : 1;
    }

    if (argc == 2 && std::strcmp(argv[1], "--validate-mapping") == 0) {
        std::string error;
        const bool ok = ValidateGeneratedMapping(error);
        std::printf("SECONDARY mapping %s%s%s\n", ok ? "PASS" : "FAIL", ok ? "" : ": ", ok ? "" : error.c_str());
        return ok ? 0 : 1;
    }

    std::string mapping_error;
    Check(ValidateGeneratedMapping(mapping_error), "generated secondary mapping is the reviewed set");

    std::string report;
    const std::string real_path = std::string(EXECUTION_PROBE_REPO_ROOT) + "/assets/default.xbe";
    if (std::filesystem::exists(real_path)) {
        const bool static_ok = StaticValidateSecondary(real_path, report);
        std::printf("SECONDARY static %s\n%s\n", static_ok ? "PASS" : "FAIL", report.c_str());
        Check(static_ok, "authentic XBE secondary-entry route matches");
    } else {
        std::printf("SECONDARY static SKIP (real XBE absent)\n");
    }

    struct Case {
        const char* scenario;
        bool expect_ok;
        const char* expected;
        int timeout_ms;
        bool expect_timeout;
    };
    const Case cases[] = {
        {"prepare", true, "RESULT ok instructions=2", 5000, false},
        {"budget", false, "instruction budget exhausted", 5000, false},
        {"deadline", false, "instruction deadline exceeded", 5000, false},
        {"stack-denied", false, "stack write denied", 5000, false},
        {"code-page-denied", false, "code page denied", 5000, false},
        {"system-routine-unmapped", false, "system routine target rejected", 5000, false},
        {"start-routine-unmapped", false, "argument rejected", 5000, false},
        {"wrong-ticket-class", false, "contract rejected", 5000, false},
        {"start-routine-entry", false, "start routine entry is forbidden", 5000, false},
        {"stack-helper-entry", false, "stack helper entry is forbidden", 5000, false},
        {"seh-entry", false, "SEH helper entry is forbidden", 5000, false},
        {"prepare-failure", false, "thread preparation failed", 5000, false},
        {"parent-timeout", false, "", 800, true},
    };

    const std::string self = SelfPath();
    Check(!self.empty(), "secondary probe can locate its own executable");
    for (const Case& test : cases) {
        const ChildResult result = SpawnChild(self, test.scenario, std::chrono::milliseconds(test.timeout_ms));
        char message[288];
        std::snprintf(message, sizeof(message), "isolated secondary child %s terminates", test.scenario);
        if (test.expect_timeout) {
            Check(result.timed_out && result.elapsed.count() >= test.timeout_ms - 100, message);
            continue;
        }
        const int exit_code = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : -1;
        const int expected_exit = test.expect_ok ? 0 : 3;
        std::snprintf(message, sizeof(message), "isolated secondary child %s reports %s", test.scenario,
                      test.expect_ok ? "success" : "a closed failure");
        Check(!result.timed_out && exit_code == expected_exit, message);
        if (test.expected[0]) {
            std::snprintf(message, sizeof(message), "isolated secondary child %s reports the expected condition",
                          test.scenario);
            Check(result.output.find(test.expected) != std::string::npos, message);
        }
        std::snprintf(message, sizeof(message), "isolated secondary child %s leaves no resources", test.scenario);
        Check(result.output.find("TEARDOWN system_used=0 contiguous_used=0 virtual_committed=0 workers=0 "
                                 "initialized=0") != std::string::npos,
              message);
    }

    const ChildResult success = SpawnChild(self, "prepare", std::chrono::milliseconds(5000));
    Check(success.output.find("STEP 002B5C0F") != std::string::npos &&
              success.output.find("STEP 002B5C11") != std::string::npos &&
              success.output.find("STOP 002B5C16") != std::string::npos &&
              success.output.find("CANCEL handle_closed=1 prepared=1 cancelled=1 output_cleared=1") !=
                  std::string::npos &&
              success.output.find("guest_memory_changes=4") != std::string::npos,
          "successful secondary probe runs the two reviewed pushes, stops at the SEH call, and cancels the thread");

    std::printf("secondary execution probe: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
