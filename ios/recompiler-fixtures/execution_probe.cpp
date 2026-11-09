// Phase 2C-15 test-only execution probe.
//
// This binary has two modes:
//   * a parent mode that spawns one disposable child per scenario, enforces an
//     external timeout, and checks the child's structured report;
//   * a child mode that prepares a synthetic generated-XBE image, instruments
//     the reviewed startup prefix through recompiler hooks, runs exactly that
//     prefix, records the trace and memory effects, and tears everything down.
//
// The probe never calls kernel::Boot(), never dispatches a kernel export, never
// invokes a guest callback and never starts a worker.  Real-XBE handling is
// limited to the non-executing static validation at the end of this file.

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
#include "execution_probe_route.h"
#if defined(EXECUTION_PROBE_MODE_REAL)
#include "execution_probe_real_route.h"
#endif
#include "kernel/crypto.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/worker_lifecycle.h"
#include "xbe/image_loader.h"
#include "xbe/xbe.h"
#include "x86_recomp_shared.h"

namespace kernel {
uint8_t* g_base = nullptr;
}

#if defined(EXECUTION_PROBE_MODE_SYNTHETIC) && !defined(EXECUTION_PROBE_FIXTURE_XBE)
#error "EXECUTION_PROBE_FIXTURE_XBE must name the generated synthetic fixture"
#endif
#if defined(EXECUTION_PROBE_MODE_REAL) && !defined(EXECUTION_PROBE_REAL_XBE)
#error "EXECUTION_PROBE_REAL_XBE must name the real image"
#endif
#if defined(EXECUTION_PROBE_MODE_REAL) && !defined(EXECUTION_PROBE_REAL_GENERATED_DIR)
#error "EXECUTION_PROBE_REAL_GENERATED_DIR must name the generated real mapping"
#endif
#if !defined(EXECUTION_PROBE_MODE_SYNTHETIC) && !defined(EXECUTION_PROBE_MODE_REAL)
#error "define EXECUTION_PROBE_MODE_SYNTHETIC or EXECUTION_PROBE_MODE_REAL"
#endif
#ifndef EXECUTION_PROBE_REPO_ROOT
#define EXECUTION_PROBE_REPO_ROOT "."
#endif

namespace {

using namespace execution_probe;
using Clock = std::chrono::steady_clock;

constexpr const char* kRealSha1 = "f636a16af04171089f879a4bed646e185c734233";
constexpr uint32_t kRealEntry = 0x002B3916;
constexpr uint32_t kRealTitle = 0x4541007B;

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
    StepKind kind;
    bool branch_valid;
    bool branch_taken;
};

struct Span {
    uint32_t begin, end;
    bool write;
};

struct Options {
    uint32_t start_pc = kEntry;
    uint32_t instruction_budget = kMaximumInstructions;
    uint32_t direct_call_budget = kMaximumDirectCalls;
    uint32_t kernel_call_budget = kMaximumKernelCalls;
    bool allow_call_target = true;
    bool allow_kernel_call = false;
    bool require_kernel_success = false;
    bool import_unpatched = false;
    uint32_t import_target = 0;  // 0 means the reviewed ordinal-255 thunk
    uint32_t helper_value = 0;
    bool revoke_ret_stack = false;
    bool allow_counter_page = true;
    bool decommit_global = false;
    bool entry_read_only = false;
    bool helper_read_only = false;
    bool not_taken = false;
    bool use_resume = false;
    bool reenter = false;
    uint32_t reenter_pc = 0;
    std::chrono::milliseconds delay{0};
    uint32_t delay_pc = 0;
    std::chrono::milliseconds deadline{0};
    uint32_t initial_esp = kStackTop;
    std::vector<Span> allowed_spans;
    std::vector<uint32_t> snapshot_pages;
    std::vector<uint32_t> owned_pages;
    uint32_t image_base = 0, image_size = 0;
    bool free_image = false;
    uint32_t stack_top = 0;
    bool free_stack = false;
};

struct ProbeState {
    Options options;
    uint32_t executed = 0;
    uint32_t direct_calls = 0;
    uint32_t kernel_calls = 0;
    uint32_t import_reads = 0;
    uint32_t worker_launch_baseline = 0;
    size_t thread_baseline = 0;
    bool running = false;
    bool in_step = false;
    bool first_step = true;
    uint32_t previous_pc = 0;
    bool have_previous = false;
    StepRecord stop{};
    bool have_stop = false;
    bool stop_checked_call_clear = false;
    std::vector<StepRecord> trace;
    std::vector<MemoryRecord> memory;
    Clock::time_point deadline{};
    bool deadline_set = false;
};

ProbeState g_probe;

const RouteStep* FindStep(uint32_t pc) {
    for (const RouteStep& step : kRoute)
        if (step.pc == pc) return &step;
    return nullptr;
}

bool InImportPage(uint32_t address, uint64_t end) {
    return address < uint64_t(kImportPage) + mem::kPage && end > kImportPage;
}

[[noreturn]] void Fail(const char* reason, uint32_t pc, uint32_t address = 0, bool has_address = false) {
    ProbeFailure failure;
    failure.reason = reason;
    failure.pc = pc;
    failure.address = address;
    failure.has_address = has_address;
    throw failure;
}

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
    record.kind = step.kind;
    record.branch_valid = false;
    record.branch_taken = false;
    g_probe.trace.push_back(record);
}

struct InStep {
    InStep() { g_probe.in_step = true; }
    ~InStep() { g_probe.in_step = false; }
};

// Test-only ordinal-255 service state; the handler prepares an inactive thread
// with the authentic arguments and never starts it.
enum class KernelBridgeMode : uint8_t { kPrepare, kFail, kBadCleanup, kBadReturn, kBadDirectReturn, kStartAttempt, kZeroHandle };
KernelBridgeMode g_kernel_bridge_mode = KernelBridgeMode::kPrepare;
uint32_t g_kernel255_calls = 0;
uint32_t g_kernel255_status = 0;
uint32_t g_kernel255_esp = 0;
uint32_t g_kernel255_return = 0;
std::array<uint32_t, 10> g_kernel255_args{};
std::array<uint32_t, 10> g_expected_kernel_args{};
bool g_have_expected_kernel_args = false;
kernel::ThreadRef g_prepared_thread;

void ProbeStep(uint32_t pc, X86Context& ctx) {
    if (!g_probe.running) Fail("instruction step outside a probe run", pc);
    if (g_probe.in_step) Fail("re-entry during a step", pc);
    InStep guard;

    const RouteStep* step = FindStep(pc);
    if (!step) Fail("instruction PC is not in the reviewed route", pc);
    if (g_probe.executed >= g_probe.options.instruction_budget)
        Fail("instruction budget exhausted", pc);
    if (g_probe.deadline_set && Clock::now() > g_probe.deadline)
        Fail("instruction deadline exceeded", pc);
    if (mem::CheckAccess(pc, step->length, mem::kExecute) != mem::AccessStatus::kOk)
        Fail("guest code page is not executable", pc);

    if (g_probe.first_step) {
        if (pc != kEntry) Fail("first instruction is not the reviewed entry", pc);
        g_probe.first_step = false;
    } else {
        const RouteStep* previous = FindStep(g_probe.previous_pc);
        const bool next_edge = previous && pc == previous->next;
        const bool alternate_edge = previous && previous->kind == StepKind::kConditional && pc == previous->alternate;
        if (!next_edge && !alternate_edge) Fail("unexpected control transfer", pc);
        if (previous && previous->kind == StepKind::kConditional && !g_probe.trace.empty()) {
            g_probe.trace.back().branch_valid = true;
            g_probe.trace.back().branch_taken = alternate_edge;
        }
    }

    if (step->kind == StepKind::kForbidden) Fail("unreviewed control flow", pc);

    // a failed kernel status would divert into the unreviewed debug arm
    if (pc == kImportReturn && g_probe.options.require_kernel_success && ctx.eax != kernel::kSuccess)
        Fail("ordinal-255 thread preparation failed", pc);

    if (step->kind == StepKind::kCall) {
        if (!g_probe.options.allow_call_target) Fail("direct call target is not permitted", pc);
        if (g_probe.direct_calls >= g_probe.options.direct_call_budget)
            Fail("direct call budget exhausted", pc);
        const RouteStep* target = FindStep(kCallTarget);
        if (!target || mem::CheckAccess(kCallTarget, target->length, mem::kExecute) != mem::AccessStatus::kOk)
            Fail("direct call target page is not executable", pc);
        ++g_probe.direct_calls;
    }

    if (step->kind == StepKind::kKernelCall) {
        if (!g_probe.options.allow_kernel_call) Fail("ordinal-255 import call is not permitted", pc);
        if (g_probe.kernel_calls >= g_probe.options.kernel_call_budget) Fail("kernel call budget exhausted", pc);
        if (pc != kImportCall) Fail("kernel call is not the reviewed import call", pc);
        if (dispatch::ResolveTarget(dispatch::KernelThunkAddress(kImportOrdinal)).kind !=
            dispatch::TargetKind::kKernelThunk)
            Fail("ordinal-255 kernel thunk is not registered", pc);
        if (kImportReturn < X86CodeBase || kImportReturn >= X86CodeEnd)
            Fail("kernel call return PC is outside the generated code", pc);
        if (mem::CheckAccess(kImportReturn, 1, mem::kExecute) != mem::AccessStatus::kOk)
            Fail("kernel call return PC is not executable", pc);
        ++g_probe.kernel_calls;
    }

    // the reviewed RET reads the caller's return address from the stack
    if (g_probe.options.revoke_ret_stack && pc == kRetInstruction &&
        mem::Protect(ctx.esp & ~(mem::kPage - 1), mem::kPage, mem::kNoAccess) != mem::AccessStatus::kOk)
        Fail("could not revoke the return stack", pc);

    if (step->kind == StepKind::kStop) {
        if (g_probe.kernel_calls != 1) Fail("boundary without the reviewed kernel call", pc);
        if (g_kernel255_calls != 1) Fail("ordinal-255 service was not called exactly once", pc);
        if (g_probe.options.require_kernel_success && g_kernel255_status != kernel::kSuccess)
            Fail("ordinal-255 thread preparation failed", pc);
        if (!kernel::Threads().empty()) Fail("a guest thread became active", pc);
        if (kernel::ThreadWorkerLaunchCountForTest() != g_probe.worker_launch_baseline)
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
        g_probe.stop.kind = StepKind::kStop;
        g_probe.stop.branch_valid = false;
        g_probe.stop.branch_taken = false;
        g_probe.stop_checked_call_clear = ctx.checked_call == nullptr;
        g_probe.have_stop = true;
        throw ProbeNormalStop{};
    }

    if (g_probe.options.delay.count() && g_probe.options.delay_pc == pc)
        std::this_thread::sleep_for(g_probe.options.delay);

    if (g_probe.options.reenter && g_probe.options.reenter_pc == pc) {
        X86Context nested{};
        nested.esp = kStackTop;
        sub_002B3916(nested, nullptr);
        Fail("re-entry returned unexpectedly", pc);  // the nested step hook throws first
    }

    RecordStep(pc, ctx, *step);
    ++g_probe.executed;
    g_probe.previous_pc = pc;
    g_probe.have_previous = true;
}

// Test-only ordinal-255 service.  It prepares an inactive guest thread with the
// authentic arguments and never starts it; the probe cancels it during teardown.
void ProbeKernel255Handler(X86Context& ctx, uint8_t*) {
    ++g_kernel255_calls;
    g_kernel255_esp = ctx.esp;
    g_kernel255_return = kernel::R32(ctx.esp);
    std::array<uint32_t, 10> args{};
    if (mem::Read(ctx.esp + 4, args.data(), sizeof(args)) != mem::AccessStatus::kOk) {
        g_kernel255_status = kernel::kAccessViolation;
        kernel::Return(ctx, g_kernel255_status, 10);
        return;
    }
    g_kernel255_args = args;
    if (g_have_expected_kernel_args && args != g_expected_kernel_args) {
        g_kernel255_status = kernel::kInvalidParameter;
        kernel::Return(ctx, g_kernel255_status, 10);
        return;
    }
    if (g_kernel_bridge_mode == KernelBridgeMode::kBadCleanup) {
        // deliberately wrong stdcall cleanup; the checked ABI must trap it
        g_kernel255_status = kernel::kSuccess;
        ctx.eax = kernel::kSuccess;
        ctx.esp += 4;
        return;
    }
    if (g_kernel_bridge_mode == KernelBridgeMode::kBadReturn) {
        g_kernel255_status = kernel::kSuccess;
        kernel::Return(ctx, g_kernel255_status, 10);
        const uint32_t invalid = 0xDEADBEEF;
        mem::Write(ctx.esp - 44, &invalid, sizeof(invalid));
        return;
    }
    if (g_kernel_bridge_mode == KernelBridgeMode::kFail) {
        g_kernel255_status = kernel::kNoMemory;
        kernel::Return(ctx, g_kernel255_status, 10);
        return;
    }
    kernel::ThreadRef prepared;
    g_kernel255_status = kernel::PrepareSystemThreadEx(args[0], args[2] ? args[2] : 0x10000, args[3], args[4],
                                                       args[5], args[6], args[7] & 0xFF, args[9], prepared);
    if (g_kernel255_status != kernel::kSuccess) {
        if (prepared) kernel::CancelPreparedThread(prepared);
    } else if (g_kernel_bridge_mode == KernelBridgeMode::kStartAttempt) {
        // must be refused: the authentic routines are not mapped callbacks
        if (!kernel::StartThread(prepared)) g_kernel255_status = kernel::kNoMemory;
        if (prepared) kernel::CancelPreparedThread(prepared);
    } else {
        g_prepared_thread = std::move(prepared);
        if (g_kernel_bridge_mode == KernelBridgeMode::kBadDirectReturn) {
            // corrupt the direct call's return address, 0x30 above the handler frame
            const uint32_t invalid = 0xDEADBEEF;
            mem::Write(ctx.esp + 0x30, &invalid, sizeof(invalid));
        } else if (g_kernel_bridge_mode == KernelBridgeMode::kZeroHandle) {
            // publish no handle so the caller's JNZ is not taken and the
            // forbidden zero-handle arm at 0x002B3975 is reached
            const uint32_t zero = 0;
            mem::Write(args[0], &zero, sizeof(zero));
        }
    }
    kernel::Return(ctx, g_kernel255_status, 10);
}

bool PrepareKernelBridge(bool exact_name, std::string& error) {
    kernel::RegisterExport(kImportOrdinal, exact_name ? "PsCreateSystemThreadEx" : "WrongExportName",
                           ProbeKernel255Handler);
    if (!dispatch::SetKernel255FixtureBridge(true)) {
        error = "ordinal-255 test bridge was not authorized";
        return false;
    }
    return true;
}

void ReleaseKernelBridge() {
    if (g_prepared_thread) kernel::CancelPreparedThread(g_prepared_thread);
    g_prepared_thread.reset();
    dispatch::SetKernel255FixtureBridge(false);
}

// one hook per route PC; names are emitted by the fixture builder from the same table.
// These must have external linkage: the generated code calls them by name.
}  // namespace

#define DEFINE_PROBE_STEP(pc, len, kind, next, alt)                         \
    void ProbeStep_##pc(X86Context& ctx, uint8_t*) { ProbeStep(pc, ctx); }
EXECUTION_PROBE_ROUTE(DEFINE_PROBE_STEP)
#undef DEFINE_PROBE_STEP

// Test-only replacements for the production handlers.  The probe target does
// not link sdk/runtime/cpu/memory_fault.cpp; these turn a fault into a structured
// report instead of aborting the disposable child.
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

// Called by x86_mem_read/x86_mem_write before the access is performed.
void execution_probe_memory_access(uint32_t instruction, uint32_t address, std::size_t size, bool write) {
    if (!g_probe.running) Fail("memory access outside a probe run", instruction, address, true);
    if (size == 0) Fail("zero-length memory access", instruction, address, true);
    const uint64_t end = uint64_t(address) + size;
    if (end > (uint64_t(1) << 32)) Fail("memory access overflows the guest address space", instruction, address, true);
    if (g_probe.memory.size() >= kMaximumMemoryOperations)
        Fail("memory-operation budget exhausted", instruction, address, true);

    g_probe.memory.push_back({instruction, address, uint32_t(size), write});
    if (InImportPage(address, end)) {
        // exactly one reviewed read: the checked import call's operand fetch.
        if (instruction != kImportCall || address != kImportSlot || size != 4 || write)
            Fail("unreviewed ordinal-255 import slot access", instruction, address, true);
        if (!g_probe.options.allow_kernel_call)
            Fail("ordinal-255 import operand read is not permitted", instruction, address, true);
        if (g_probe.import_reads >= 1) Fail("ordinal-255 import slot read twice", instruction, address, true);
        ++g_probe.import_reads;
        return;
    }

    bool allowed = false;
    for (const Span& span : g_probe.options.allowed_spans) {
        if (address >= span.begin && end <= span.end && (!write || span.write)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) Fail("memory access outside the reviewed memory spans", instruction, address, true);
}

namespace {

struct SnapshotPage {
    uint32_t page;
    std::array<uint8_t, mem::kPage> bytes{};
    bool valid = false;
};

std::vector<SnapshotPage> Snapshot() {
    std::vector<SnapshotPage> pages;
    for (uint32_t page : g_probe.options.snapshot_pages) {
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

void PrintMemoryDiff(const std::vector<SnapshotPage>& before) {
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
            if (changes < 16)
                std::printf("MEM pc=%08X address=%08X before=%08X after=%08X\n", WritePcFor(address), address,
                            old_value, new_value);
            ++changes;
        }
    }
    std::printf("guest_memory_changes=%u\n", changes);
}

void PrintTrace(bool full) {
    const size_t limit = full ? g_probe.trace.size() : std::min<size_t>(g_probe.trace.size(), 8);
    const size_t begin = full ? 0 : g_probe.trace.size() - limit;
    for (size_t i = begin; i < g_probe.trace.size(); ++i) {
        const StepRecord& step = g_probe.trace[i];
        std::printf("STEP %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
                    "flags=%u%u%u%u%u%u",
                    step.pc, step.eax, step.ecx, step.edx, step.ebx, step.esp, step.ebp, step.esi, step.edi,
                    step.cf, step.zf, step.sf, step.of, step.pf, step.af);
        if (step.branch_valid) std::printf(" branch=%s", step.branch_taken ? "taken" : "not-taken");
        std::printf("\n");
    }
}

void PrintStop() {
    if (!g_probe.have_stop) return;
    const StepRecord& stop = g_probe.stop;
    std::printf("STOP %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
                "flags=%u%u%u%u%u%u frame=%s\n",
                stop.pc, stop.eax, stop.ecx, stop.edx, stop.ebx, stop.esp, stop.ebp, stop.esi, stop.edi, stop.cf, stop.zf,
                stop.sf, stop.of, stop.pf, stop.af,
                g_probe.stop_checked_call_clear ? "clear" : "active");
}

uint32_t ImportReads() {
    uint32_t count = 0;
    for (const MemoryRecord& record : g_probe.memory)
        if (InImportPage(record.address, uint64_t(record.address) + record.size)) ++count;
    return count;
}

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

void Write32(uint32_t address, uint32_t value) {
    if (mem::Write(address, &value, sizeof(value)) != mem::AccessStatus::kOk)
        std::fprintf(stderr, "probe setup: could not write %08X\n", address);
}

bool ProtectRange(uint32_t base, uint32_t size, uint8_t permissions) {
    return mem::Protect(base, size, permissions) == mem::AccessStatus::kOk;
}

#if defined(EXECUTION_PROBE_MODE_SYNTHETIC)
bool PrepareFixture(xbe::Image& image, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(EXECUTION_PROBE_FIXTURE_XBE, bytes)) {
        error = std::string("cannot read ") + EXECUTION_PROBE_FIXTURE_XBE;
        return false;
    }
    if (!image.Parse(std::move(bytes), error)) return false;
    if (image.entry != kEntry) {
        error = "synthetic fixture entry does not match the route";
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

bool PrepareData(Options& options) {
    const std::array<uint32_t, 7> pages = {kDataPage,  kPointerPage, kAllocationPage, kGlobalPage,
                                           kCounterPage, kImportPage,   kStackPage};
    for (uint32_t page : pages)
        if (!mem::Commit(page, mem::kPage)) return false;
    if (!ProtectRange(kSectionVa, kSectionSize, mem::kRead | mem::kExecute)) return false;
    if (options.entry_read_only && !ProtectRange(kEntry & ~(mem::kPage - 1), mem::kPage, mem::kRead)) return false;
    if (options.helper_read_only && !ProtectRange(kCallTarget & ~(mem::kPage - 1), mem::kPage, mem::kRead)) return false;

    const uint32_t entry_pointer = options.not_taken ? kPointerPage : kDataPage;
    Write32(kEntryValue, 0x1000);
    Write32(kEntryPointer, entry_pointer);
    Write32(kDataPage, 0);
    Write32(kHelperValue, options.helper_value);
    Write32(kPointerPage, 0xFFFFFFFF);
    Write32(kAllocationPage, 0);
    Write32(kGlobalReadA, 0x1000);
    Write32(kGlobalReadB, 0x800);
    Write32(kGlobalLength, 0x100);
    Write32(kGlobalStore, kAllocationPage);
    Write32(kCounterAddress, 0);
    const uint32_t import_target = options.import_unpatched
                                        ? kImportEncoding
                                        : (options.import_target ? options.import_target
                                                                 : dispatch::KernelThunkAddress(kImportOrdinal));
    Write32(kImportSlot, import_target);
    // the checked import call must read this page once; nothing may write it or
    // execute from it during the probe.
    if (!ProtectRange(kImportPage, mem::kPage, mem::kRead)) return false;
    if (options.decommit_global && !mem::Decommit(kGlobalPage, mem::kPage)) return false;

    options.allowed_spans = {{kDataPage, kDataPage + mem::kPage, true},
                             {kPointerPage, kPointerPage + 2 * mem::kPage, true},
                             {kGlobalPage, kGlobalPage + mem::kPage, true},
                             {kCounterPage, kCounterPage + mem::kPage, true},
                             {kStackPage, kStackTop, true}};
    if (!options.allow_counter_page)
        options.allowed_spans.erase(std::remove_if(options.allowed_spans.begin(), options.allowed_spans.end(),
                                                   [](const Span& span) {
                                                       return span.begin == kCounterPage;
                                                   }),
                                    options.allowed_spans.end());
    options.snapshot_pages = {kDataPage, kPointerPage, kAllocationPage, kGlobalPage, kCounterPage, kStackPage};
    options.owned_pages = {kDataPage, kPointerPage, kAllocationPage, kGlobalPage, kCounterPage, kImportPage,
                           kStackPage};
    options.image_base = kImageBase;
    options.image_size = kImageSize;
    options.free_image = true;
    options.initial_esp = kStackTop;
    options.stack_top = 0;
    options.free_stack = false;
    return true;
}
#endif  // EXECUTION_PROBE_MODE_SYNTHETIC

struct Report {
    bool normal_stop = false;
    bool failure = false;
    ProbeFailure error;
};

Report RunProbe(const Options& options) {
    Report report;
    g_probe = ProbeState{};
    g_probe.options = options;
    g_probe.running = true;
    g_probe.first_step = true;
    g_probe.worker_launch_baseline = kernel::ThreadWorkerLaunchCountForTest();
    g_probe.thread_baseline = kernel::Threads().size();
    if (options.deadline.count()) {
        g_probe.deadline = Clock::now() + options.deadline;
        g_probe.deadline_set = true;
    }
    const std::vector<SnapshotPage> snapshot = Snapshot();

    const RouteStep* first = FindStep(options.start_pc);
    if (!first) {
        report.failure = true;
        report.error.reason = "start PC is not in the reviewed route";
        report.error.pc = options.start_pc;
    } else if (mem::CheckAccess(options.start_pc, first->length, mem::kExecute) != mem::AccessStatus::kOk) {
        report.failure = true;
        report.error.reason = "entry page is not executable";
        report.error.pc = options.start_pc;
    } else {
        const dispatch::ResolvedTarget target = dispatch::ResolveTarget(options.start_pc);
        const bool admissible = options.use_resume
                                   ? target.kind == dispatch::TargetKind::kContinuation && target.function
                                   : target.kind == dispatch::TargetKind::kGeneratedFunction && target.function;
        if (!admissible) {
            report.failure = true;
            report.error.reason = options.use_resume ? "start PC is not a generated continuation"
                                                     : "start PC is not a generated function entry";
            report.error.pc = options.start_pc;
        } else {
            X86Context ctx{};
            ctx.esp = options.initial_esp;
            try {
                if (options.use_resume)
                    x86_resume(ctx, nullptr, {options.start_pc, options.initial_esp});
                else
                    target.function(ctx, nullptr);
                report.failure = true;
                report.error.reason = "generated function returned before the mandatory stop";
                report.error.pc = kStopBefore;
            } catch (const ProbeNormalStop&) {
                report.normal_stop = true;
            } catch (const ProbeFailure& failure) {
                report.failure = true;
                report.error = failure;
            } catch (const X86MemoryFault& fault) {
                report.failure = true;
                report.error.reason = std::string("guest memory fault: ") + mem::StatusName(fault.status);
                report.error.pc = fault.instruction;
                report.error.address = fault.address;
                report.error.has_address = true;
            }
        }
    }
    g_probe.running = false;
    // the step hook runs before the instruction, so every STEP line is the
    // guest state the instruction is about to consume, not its result.
    std::printf("TRACE registers are read before each instruction\n");
    if (report.normal_stop) PrintTrace(true);
    else PrintTrace(false);
    PrintStop();
    PrintMemoryDiff(snapshot);
    return report;
}

void PrintFailure(const ProbeFailure& failure) {
    std::printf("RESULT fail reason=\"%s\" pc=%08X", failure.reason.c_str(), failure.pc);
    if (failure.has_address) std::printf(" address=%08X", failure.address);
    std::printf(" instructions=%u direct_calls=%u kernel_calls=%u import_reads=%u\n", g_probe.executed,
                g_probe.direct_calls, g_probe.kernel_calls, ImportReads());
}

bool Teardown(const Options& options) {
    ReleaseKernelBridge();
    kernel::StopKernelWorkers();
    dispatch::Reset();
    if (options.free_image) {
        uint32_t base = options.image_base, size = options.image_size;
        mem::FreeVirtual(base, size, mem::kMemRelease);
    }
    if (options.free_stack && options.stack_top) mem::FreeStack(options.stack_top);
    for (uint32_t page : options.owned_pages) mem::Decommit(page, mem::kPage);
    const mem::Stats stats = mem::Query();
    const size_t workers = kernel::KernelWorkerCountForTest();
    mem::Shutdown();
    std::printf("TEARDOWN system_used=%u contiguous_used=%u virtual_committed=%u workers=%zu initialized=%d\n",
                stats.system_used, stats.contiguous_used, stats.virtual_committed, workers,
                int(mem::IsInitialized()));
    return stats.system_used == 0 && stats.contiguous_used == 0 && stats.virtual_committed == 0 && workers == 0 &&
           !mem::IsInitialized();
}

#if defined(EXECUTION_PROBE_MODE_SYNTHETIC)
// cancels the thread the ordinal-255 service prepared and checks the rollback
bool CancelPreparedThreadForProbe() {
    if (!g_prepared_thread) return false;
    const kernel::ThreadRef thread = g_prepared_thread;
    g_prepared_thread.reset();
    const uint32_t handle_slot = g_kernel255_args[0];
    uint32_t handle = 0;
    const bool handle_read = mem::Read(handle_slot, &handle, sizeof(handle)) == mem::AccessStatus::kOk;
    const uint32_t stack_top = thread->stack_top;
    const bool prepared = thread->state.load() == kernel::ThreadState::kPrepared && handle_read && handle != 0 &&
                          kernel::FromHandle(handle).get() == thread.get() && mem::IsCommitted(stack_top - 1) &&
                          kernel::Threads().empty() &&
                          kernel::ThreadWorkerLaunchCountForTest() == g_probe.worker_launch_baseline;
    const bool cancelled = kernel::CancelPreparedThread(thread);
    uint32_t cleared = 1;
    const bool cleared_ok =
        mem::Read(handle_slot, &cleared, sizeof(cleared)) == mem::AccessStatus::kOk && cleared == 0;
    const bool stack_freed = !mem::IsCommitted(stack_top - 1);
    const bool ok = prepared && cancelled && cleared_ok && stack_freed && !kernel::FromHandle(handle) &&
                    kernel::Threads().empty();
    std::printf("CANCEL prepared=%d cancelled=%d output_cleared=%d stack_freed=%d workers=%zu\n", int(prepared),
                int(cancelled), int(cleared_ok), int(stack_freed), kernel::KernelWorkerCountForTest());
    return ok;
}

bool VerifySyntheticPostconditions(std::string& error) {
    if (!g_probe.have_stop || g_probe.stop.pc != kStopBefore) {
        error = "mandatory stop is not the close-call boundary";
        return false;
    }
    const uint32_t expected = 60 + (g_probe.options.not_taken ? 1 : 0);
    if (g_probe.executed != expected) {
        error = "expected " + std::to_string(expected) + " instructions, got " + std::to_string(g_probe.executed);
        return false;
    }
    if (g_probe.stop.esp != g_probe.options.initial_esp - kImportStackBytes + 0x44) {
        error = "caller push did not leave the reviewed stack";
        return false;
    }
    if (!g_probe.stop_checked_call_clear) {
        error = "checked call frame was not completed by the RET";
        return false;
    }
    const StepRecord* caller_branch = nullptr;
    for (const StepRecord& step : g_probe.trace) {
        if (step.pc == kStopBefore) {
            error = "the stop boundary instruction was executed";
            return false;
        }
        if (step.pc == kCallerBranch) caller_branch = &step;
    }
    if (g_probe.trace.empty() || g_probe.trace.back().pc != kLastInstruction) {
        error = "the caller continuation was not the last executed instruction";
        return false;
    }
    if (!caller_branch || !caller_branch->branch_valid || !caller_branch->branch_taken) {
        error = "caller JNZ was not taken";
        return false;
    }
    if (g_probe.direct_calls != 1) {
        error = "expected one direct call";
        return false;
    }
    if (g_probe.kernel_calls != 1) {
        error = "expected one kernel call";
        return false;
    }
    if (ImportReads() != 1 || g_probe.import_reads != 1) {
        error = "expected exactly one import operand read";
        return false;
    }
    const StepRecord* status = nullptr;
    const StepRecord* branch = nullptr;
    for (const StepRecord& step : g_probe.trace) {
        if (step.pc == kImportReturn) status = &step;
        if (step.pc == kPostImportBranch) branch = &step;
    }
    if (!status || status->eax != kernel::kSuccess) {
        error = "kernel status was not success at the post-import branch";
        return false;
    }
    if (!branch || !branch->branch_valid || !branch->branch_taken) {
        error = "post-import JNL was not taken";
        return false;
    }
    if (!g_prepared_thread || g_prepared_thread->state.load() != kernel::ThreadState::kPrepared) {
        error = "no thread was prepared";
        return false;
    }
    uint32_t handle = 0;
    if (mem::Read(g_kernel255_args[0], &handle, sizeof(handle)) != mem::AccessStatus::kOk || !handle ||
        g_probe.stop.eax != handle || g_probe.stop.edi != handle || g_probe.stop.esi != 0) {
        error = "the caller did not carry the published thread handle";
        return false;
    }
    uint32_t pushed = 0;
    if (mem::Read(g_probe.stop.esp, &pushed, sizeof(pushed)) != mem::AccessStatus::kOk || pushed != handle) {
        error = "the caller did not push the published thread handle";
        return false;
    }
    unsigned handle_reads = 0;
    for (const MemoryRecord& record : g_probe.memory)
        if (!record.write && record.pc == kHandleRead && record.address == g_kernel255_args[0] &&
            record.size == 4)
            ++handle_reads;
    if (handle_reads != 1) {
        error = "the handle output was not read exactly once";
        return false;
    }
    if (!kernel::Threads().empty() || kernel::ThreadWorkerLaunchCountForTest() != g_probe.worker_launch_baseline) {
        error = "a guest thread or worker became active";
        return false;
    }
    return true;
}

int RunChild(const std::string& scenario) {
    Options options;
    enum class Mode { kRun, kImportProbe, kSecondRead, kGuards, kCheckedCall } mode = Mode::kRun;
    bool exact_name = true;
    if (scenario == "taken") {
        options.allow_kernel_call = true;
        options.require_kernel_success = true;
    } else if (scenario == "not-taken") {
        options.not_taken = true;
        options.allow_kernel_call = true;
        options.require_kernel_success = true;
    } else if (scenario == "import-not-permitted") {
    } else if (scenario == "import-unpatched") {
        options.allow_kernel_call = true;
        options.import_unpatched = true;
    } else if (scenario == "import-wrong-ordinal") {
        options.allow_kernel_call = true;
        options.import_target = dispatch::KernelThunkAddress(254);
    } else if (scenario == "import-wrong-name") {
        options.allow_kernel_call = true;
        exact_name = false;
    } else if (scenario == "import-bad-cleanup") {
        options.allow_kernel_call = true;
        g_kernel_bridge_mode = KernelBridgeMode::kBadCleanup;
    } else if (scenario == "import-bad-return") {
        options.allow_kernel_call = true;
        g_kernel_bridge_mode = KernelBridgeMode::kBadReturn;
    } else if (scenario == "import-bad-return-span" || scenario == "import-arg-span") {
        options.allow_kernel_call = true;
        mode = Mode::kCheckedCall;
    } else if (scenario == "import-prepare-failure") {
        options.allow_kernel_call = true;
        options.require_kernel_success = true;
        options.helper_value = UINT32_MAX;
    } else if (scenario == "import-start-attempt") {
        options.allow_kernel_call = true;
        options.require_kernel_success = true;
        g_kernel_bridge_mode = KernelBridgeMode::kStartAttempt;
    } else if (scenario == "import-failure-path") {
        // a failed kernel status must diverge to the unreviewed debug arm and stop there
        options.allow_kernel_call = true;
        g_kernel_bridge_mode = KernelBridgeMode::kFail;
    } else if (scenario == "ret-bad-address") {
        options.allow_kernel_call = true;
        options.require_kernel_success = true;
        g_kernel_bridge_mode = KernelBridgeMode::kBadDirectReturn;
    } else if (scenario == "ret-stack-denied") {
        options.allow_kernel_call = true;
        options.require_kernel_success = true;
        options.revoke_ret_stack = true;
    } else if (scenario == "caller-not-taken") {
        // a zero published handle sends the reviewed JNZ down its forbidden arm
        options.allow_kernel_call = true;
        options.require_kernel_success = true;
        g_kernel_bridge_mode = KernelBridgeMode::kZeroHandle;
    } else if (scenario == "import-read") {
        mode = Mode::kImportProbe;
    } else if (scenario == "import-second-read") {
        options.allow_kernel_call = true;
        mode = Mode::kSecondRead;
    } else if (scenario == "post-import-entry") {
        options.start_pc = 0x002B3971;
    } else if (scenario == "entry-not-executable") {
        options.entry_read_only = true;
    } else if (scenario == "helper-not-executable") {
        options.helper_read_only = true;
    } else if (scenario == "call-target-not-permitted") {
        options.allow_call_target = false;
    } else if (scenario == "unexpected-entry") {
        options.start_pc = kCallTarget;
    } else if (scenario == "continuation-entry") {
        options.start_pc = kCallTarget;
        options.use_resume = true;
    } else if (scenario == "unreviewed-memory") {
        options.allow_counter_page = false;
    } else if (scenario == "budget") {
        options.instruction_budget = 10;
    } else if (scenario == "deadline") {
        options.deadline = std::chrono::milliseconds(10);
        options.delay = std::chrono::milliseconds(50);
        options.delay_pc = 0x002B3940;
    } else if (scenario == "parent-timeout") {
        options.delay = std::chrono::seconds(10);
        options.delay_pc = kEntry;
    } else if (scenario == "memory-fault") {
        options.decommit_global = true;
    } else if (scenario == "re-entry") {
        options.reenter = true;
        options.reenter_pc = 0x002B3940;
    } else if (scenario == "guards") {
        mode = Mode::kGuards;
    } else {
        std::printf("SETUP fail unknown scenario %s\n", scenario.c_str());
        return 4;
    }

    if (!mem::Init()) {
        std::printf("SETUP fail memory init\n");
        return 4;
    }
    bool teardown_ok = false;
    if (mode == Mode::kGuards) {
        const bool raw_guard = mem::Base() == nullptr;
        const size_t workers = kernel::KernelWorkerCountForTest();
        teardown_ok = Teardown(Options{});
        std::printf("RESULT ok guards=1 raw_base=%s workers=%zu\n", raw_guard ? "null" : "non-null", workers);
        return teardown_ok ? 0 : 5;
    }

    xbe::Image image;
    std::string error;
    if (!PrepareFixture(image, error)) {
        std::printf("SETUP fail %s\n", error.c_str());
        mem::Shutdown();
        return 4;
    }
    if (!PrepareData(options)) {
        std::printf("SETUP fail data pages\n");
        mem::Shutdown();
        return 4;
    }
    if (options.use_resume) {
        // Mark the helper entry as a generated continuation, then enter it through
        // x86_resume.  The probe must still count or reject that entry.
        static const uint32_t continuations[] = {kCallTarget, 0};
        const dispatch::Metadata metadata{X86CodeBase, X86CodeEnd, X86EntryPoint,
                                          std::span<const X86FuncMapping>(X86FuncMappings, X86FuncMappingCount),
                                          std::span<const uint32_t>(continuations, 1)};
        if (!dispatch::Init(metadata, error)) {
            std::printf("SETUP fail continuation dispatch\n");
            mem::Shutdown();
            return 4;
        }
    }

    if (mode == Mode::kRun || mode == Mode::kCheckedCall) {
        if (!PrepareKernelBridge(exact_name, error)) {
            ProbeFailure failure;
            failure.reason = error;
            failure.pc = kImportCall;
            PrintFailure(failure);
            teardown_ok = Teardown(options);
            return teardown_ok ? 3 : 5;
        }
    }

    if (mode == Mode::kImportProbe || mode == Mode::kSecondRead) {
        g_probe = ProbeState{};
        g_probe.options = options;
        g_probe.running = true;
        try {
            if (mode == Mode::kImportProbe) {
                (void)x86_mem_read<uint32_t>(kStopBefore, kImportSlot);
            } else {
                (void)x86_mem_read<uint32_t>(kImportCall, kImportSlot);
                (void)x86_mem_read<uint32_t>(kImportCall, kImportSlot);
            }
            std::printf("RESULT fail reason=\"import read was not rejected\" pc=%08X\n", kImportCall);
            g_probe.running = false;
            Teardown(options);
            return 3;
        } catch (const ProbeFailure& failure) {
            g_probe.running = false;
            PrintFailure(failure);
            teardown_ok = Teardown(options);
            return teardown_ok ? 3 : 5;
        }
    }

    if (mode == Mode::kCheckedCall) {
        g_probe = ProbeState{};
        g_probe.options = options;
        g_probe.running = true;
        X86Context ctx{};
        const bool bad_args = scenario == "import-arg-span";
        ctx.esp = bad_args ? kStackTop : kStackTop - 16;
        const uint32_t return_address = bad_args ? kImportReturn : 0x00090000;
        try {
            x86_checked_indirect_call(ctx, nullptr, kImportSlot, return_address, kImportCall);
            std::printf("RESULT fail reason=\"checked call was not rejected\" pc=%08X\n", kImportCall);
            g_probe.running = false;
            Teardown(options);
            return 3;
        } catch (const ProbeFailure& failure) {
            g_probe.running = false;
            PrintFailure(failure);
            teardown_ok = Teardown(options);
            return teardown_ok ? 3 : 5;
        } catch (const X86MemoryFault& fault) {
            g_probe.running = false;
            ProbeFailure failure;
            failure.reason = std::string("guest memory fault: ") + mem::StatusName(fault.status);
            failure.pc = fault.instruction;
            failure.address = fault.address;
            failure.has_address = true;
            PrintFailure(failure);
            teardown_ok = Teardown(options);
            return teardown_ok ? 3 : 5;
        }
    }

    const Report report = RunProbe(options);
    if (report.failure) {
        PrintFailure(report.error);
        teardown_ok = Teardown(options);
        return teardown_ok ? 3 : 5;
    }
    std::string post_error;
    if (!VerifySyntheticPostconditions(post_error)) {
        ProbeFailure failure;
        failure.reason = post_error;
        failure.pc = kImportCall;
        PrintFailure(failure);
        teardown_ok = Teardown(options);
        return teardown_ok ? 3 : 5;
    }
    const bool cancel_ok = CancelPreparedThreadForProbe();
    std::printf("RESULT ok instructions=%u direct_calls=%u kernel_calls=%u import_reads=%u workers=%zu stop=%08X "
                "cancelled=%d\n",
                g_probe.executed, g_probe.direct_calls, g_probe.kernel_calls, ImportReads(),
                kernel::KernelWorkerCountForTest(), kStopBefore, int(cancel_ok));
    teardown_ok = Teardown(options);
    return (teardown_ok && cancel_ok) ? 0 : 5;
}
#endif  // EXECUTION_PROBE_MODE_SYNTHETIC

#if defined(EXECUTION_PROBE_MODE_SYNTHETIC)
bool StaticValidateReal(const std::string& path, std::string& report) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path.c_str(), bytes)) {
        report = "real XBE not present";
        return true;  // gitignored input: skip without failing
    }
    xbe::Image image;
    std::string error;
    if (!image.Parse(std::move(bytes), error)) {
        report = "parse failed: " + error;
        return false;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "real: title=%08X entry=%08X base=%08X image=%X sections=%zu",
                  image.title_id, image.entry, image.base, image.image_size, image.sections.size());
    report = line;
    if (image.entry != kRealEntry) return false;
    if (image.title_id != kRealTitle) return false;
    if (Sha1Hex(image.file) != kRealSha1) return false;

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    for (const RouteStep& step : kRoute) {
        const int64_t offset = image.Offset(step.pc);
        const size_t span = image.FileSpan(step.pc);
        if (offset < 0 || span == 0) {
            report += "\nroute PC is not file-backed";
            return false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image.file.data() + offset, span, &instruction, operands))) {
            report += "\nroute instruction did not decode";
            return false;
        }
        if (instruction.length != step.length) {
            report += "\nroute instruction length mismatch";
            return false;
        }
        if (step.kind == StepKind::kConditional) {
            if (operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                uint32_t(step.pc + instruction.length + operands[0].imm.value.s) != step.alternate) {
                report += "\nconditional target mismatch";
                return false;
            }
        } else if (step.kind == StepKind::kCall) {
            if (operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                uint32_t(step.pc + instruction.length + operands[0].imm.value.s) != step.next) {
                report += "\ndirect call target mismatch";
                return false;
            }
        } else if (step.kind == StepKind::kKernelCall) {
            if (operands[0].type != ZYDIS_OPERAND_TYPE_MEMORY || operands[0].mem.base != ZYDIS_REGISTER_NONE ||
                operands[0].mem.index != ZYDIS_REGISTER_NONE || operands[0].size != 32 ||
                operands[0].mem.disp.value != int64_t(kImportSlot)) {
                report += "\nimport call operand mismatch";
                return false;
            }
        }
    }

    const RouteStep* stop_step = FindStep(kStopBefore);
    const RouteStep* ret_step = FindStep(kRetInstruction);
    const RouteStep* caller_step = FindStep(kCallerBranch);
    const RouteStep* close_step = FindStep(kCloseCall);
    if (!stop_step || stop_step->kind != StepKind::kStop || !ret_step || ret_step->next != kRetTarget ||
        !caller_step || caller_step->kind != StepKind::kConditional || caller_step->next != kCallerOffPath ||
        caller_step->alternate != kCallerContinuation || !close_step || close_step->pc != kStopBefore) {
        report += "\nthe caller boundary is not the reviewed pre-instruction stop";
        return false;
    }
    uint32_t import = 0;
    if (!image.Read32(kImportSlot, import) || import != kImportEncoding) {
        report += "\nimport slot is not the ordinal-255 encoding";
        return false;
    }
    std::snprintf(line, sizeof(line), "real: route=%zu instructions decoded, stop=%08X, import=%08X:%08X",
                  kRouteCount, kStopBefore, kImportSlot, import);
    report += "\n";
    report += line;
    return true;
}
#endif  // EXECUTION_PROBE_MODE_SYNTHETIC

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

#if defined(EXECUTION_PROBE_MODE_SYNTHETIC)
struct Case {
    const char* scenario;
    bool expect_ok;
    const char* expected;
    int timeout_ms;
    bool expect_timeout;
};
#endif

}  // namespace

#if defined(EXECUTION_PROBE_MODE_SYNTHETIC)
int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return RunChild(argv[2]);

    if (argc == 3 && std::strcmp(argv[1], "--static-real") == 0) {
        std::string report;
        const bool ok = StaticValidateReal(argv[2], report);
        std::printf("%s\n", report.c_str());
        return ok ? 0 : 1;
    }

    std::string report;
    const std::string real_path = std::string(EXECUTION_PROBE_REPO_ROOT) + "/assets/default.xbe";
    const bool static_ok = StaticValidateReal(real_path, report);
    std::printf("STATIC %s\n%s\n", static_ok ? "PASS" : "FAIL", report.c_str());
    if (!static_ok) ++failures;

    const std::string self = SelfPath();
    Check(!self.empty(), "probe can locate its own executable");

    const Case cases[] = {
        {"guards", true, "RESULT ok guards=1", 5000, false},
        {"taken", true, "RESULT ok instructions=60", 5000, false},
        {"not-taken", true, "RESULT ok instructions=61", 5000, false},
        {"caller-not-taken", false, "unreviewed control flow", 5000, false},
        {"entry-not-executable", false, "entry page is not executable", 5000, false},
        {"helper-not-executable", false, "direct call target page is not executable", 5000, false},
        {"call-target-not-permitted", false, "direct call target is not permitted", 5000, false},
        {"unexpected-entry", false, "first instruction is not the reviewed entry", 5000, false},
        {"continuation-entry", false, "first instruction is not the reviewed entry", 5000, false},
        {"unreviewed-memory", false, "outside the reviewed memory spans", 5000, false},
        {"budget", false, "instruction budget exhausted", 5000, false},
        {"deadline", false, "instruction deadline exceeded", 5000, false},
        {"memory-fault", false, "guest memory fault", 5000, false},
        {"import-read", false, "unreviewed ordinal-255 import slot access", 5000, false},
        {"import-second-read", false, "ordinal-255 import slot read twice", 5000, false},
        {"import-not-permitted", false, "ordinal-255 import call is not permitted", 5000, false},
        {"import-unpatched", false, "checked CALL target 800000FF rejected", 5000, false},
        {"import-wrong-ordinal", false, "kernel export thunk", 5000, false},
        {"import-wrong-name", false, "ordinal-255 test bridge was not authorized", 5000, false},
        {"import-bad-cleanup", false, "stdcall", 5000, false},
        {"import-bad-return", false, "return address was modified", 5000, false},
        {"import-bad-return-span", false, "return address", 5000, false},
        {"import-arg-span", false, "guest memory fault", 5000, false},
        {"import-prepare-failure", false, "ordinal-255 thread preparation failed", 5000, false},
        {"import-start-attempt", false, "ordinal-255 thread preparation failed", 5000, false},
        {"import-failure-path", false, "unreviewed control flow", 5000, false},
        {"ret-bad-address", false, "invalid guest return address", 5000, false},
        {"ret-stack-denied", false, "guest memory fault", 5000, false},
        {"post-import-entry", false, "start PC is not a generated function entry", 5000, false},
        {"re-entry", false, "re-entry during a step", 5000, false},
        {"parent-timeout", false, "", 800, true},
    };

    for (const Case& test : cases) {
        const ChildResult result = SpawnChild(self, test.scenario, std::chrono::milliseconds(test.timeout_ms));
        char message[256];
        std::snprintf(message, sizeof(message), "isolated child %s terminates", test.scenario);
        if (test.expect_timeout) {
            Check(result.timed_out && result.elapsed.count() >= test.timeout_ms - 100, message);
            continue;
        }
        const int exit_code = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : -1;
        const int expected_exit = test.expect_ok ? 0 : 3;
        std::snprintf(message, sizeof(message), "isolated child %s reports %s", test.scenario,
                      test.expect_ok ? "success" : "a closed failure");
        Check(!result.timed_out && exit_code == expected_exit, message);
        if (test.expected[0]) {
            std::snprintf(message, sizeof(message), "isolated child %s reports the expected condition", test.scenario);
            Check(result.output.find(test.expected) != std::string::npos, message);
        }
        if (!test.expect_ok) {
            std::snprintf(message, sizeof(message), "isolated child %s reports a PC and tears down cleanly",
                          test.scenario);
            Check(result.output.find("pc=") != std::string::npos &&
                      result.output.find("TEARDOWN system_used=0 contiguous_used=0 virtual_committed=0 workers=0 "
                                         "initialized=0") != std::string::npos,
                  message);
        } else {
            std::snprintf(message, sizeof(message), "isolated child %s leaves no resources", test.scenario);
            Check(result.output.find("TEARDOWN system_used=0 contiguous_used=0 virtual_committed=0 workers=0 "
                                     "initialized=0") != std::string::npos,
                  message);
        }
    }

    // the two data-dependent outcomes differ in both the trace and the guest memory
    const ChildResult taken = SpawnChild(self, "not-taken", std::chrono::milliseconds(5000));
    Check(taken.output.find("branch=not-taken") != std::string::npos &&
              taken.output.find("MEM pc=002B392C address=00020000") != std::string::npos,
          "not-taken entry branch records the conditional path and its guest-memory write");

    // the success path prepares and cancels an inactive thread and stops after the import call
    const ChildResult success = SpawnChild(self, "taken", std::chrono::milliseconds(5000));
    Check(success.output.find("CANCEL prepared=1 cancelled=1 output_cleared=1 stack_freed=1") != std::string::npos &&
              success.output.find("STOP 002B3980") != std::string::npos &&
              success.output.find("kernel_calls=1 import_reads=1") != std::string::npos &&
              success.output.find("frame=clear") != std::string::npos,
          "successful probe returns through RET 0x18, checks the handle, stops before NtClose, and cancels the thread");

    std::printf("execution probe: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
#endif  // EXECUTION_PROBE_MODE_SYNTHETIC

#if defined(EXECUTION_PROBE_MODE_REAL)
#include "execution_probe_real.inc"
#endif
