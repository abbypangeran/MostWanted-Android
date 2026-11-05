#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include "x86_recomp_shared.h"

namespace {

struct FixtureStop {};
int failures = 0;
int faults = 0;
int traps = 0;
X86MemoryFault last_fault{};
uint32_t last_trap_pc = 0;
std::string last_trap_reason;

__attribute__((noinline)) void LocalCopy(void* destination, const void* source, size_t size) {
    std::memcpy(destination, source, size);
}

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE_CLI
void RunReleaseBenchmarks(uint32_t source, uint32_t destination) {
    constexpr int scalar_rounds = 200000;
    X86Context ctx{};
    ctx.ebx = source;
    ctx.edi = destination;
    auto ns_per_call = [&](X86Func* function) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < scalar_rounds; ++i) {
            ctx.esp = 0x1000;
            function(ctx, nullptr);
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
               scalar_rounds;
    };
    const double empty_ns = ns_per_call(sub_0001103F);
    const double scalar_ns = ns_per_call(sub_00011045);
    std::printf("release fixture benchmark: no-memory generated call %.1f ns; generated checked scalar load+store %.1f ns "
                "(delta %.1f ns/call; %.2f M checked ops/s)\n",
                empty_ns, scalar_ns, scalar_ns - empty_ns, 2.0 * scalar_rounds / (scalar_ns * scalar_rounds) * 1e9 / 1e6);

    constexpr uint32_t bulk_source = 0x40000, bulk_destination = 0x50000;
    constexpr int bulk_rounds = 10000;
    constexpr size_t bulk_size = 0x1000;
    if (!mem::Commit(bulk_source, 2 * mem::kPage) || !mem::Commit(bulk_destination, 2 * mem::kPage)) {
        Check(false, "release bulk benchmark pages commit");
        return;
    }
    std::array<uint8_t, bulk_size> local_source{}, local_destination{};
    local_source.fill(0x5A);
    bool checked_ok = mem::Write(bulk_source, local_source.data(), bulk_size) == mem::AccessStatus::kOk;
    volatile uint64_t local_checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < bulk_rounds; ++i) {
        LocalCopy(local_destination.data(), local_source.data(), bulk_size);
        local_checksum += local_destination[0];
    }
    const double local_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < bulk_rounds; ++i) {
        checked_ok &= mem::Copy(bulk_destination, bulk_source, bulk_size) == mem::AccessStatus::kOk;
    }
    const double checked_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double bytes = double(bulk_rounds) * bulk_size;
    std::printf("release bulk benchmark: checked mem::Copy %.2f GiB/s; local-buffer memcpy proxy %.2f GiB/s "
                "(proxy is not direct guest memory)\n",
                bytes / checked_seconds / (1024.0 * 1024.0 * 1024.0),
                bytes / local_seconds / (1024.0 * 1024.0 * 1024.0));
    Check(checked_ok && local_checksum == uint64_t(bulk_rounds) * local_source[0] &&
              mem::Read(bulk_destination, local_destination.data(), bulk_size) == mem::AccessStatus::kOk &&
              local_destination == local_source,
          "release checked bulk benchmark transfers all requested bytes");
}
#endif

bool ThrowsMemoryFault(X86Func* function, X86Context& ctx) {
    faults = 0;
    try {
        function(ctx, nullptr);
    } catch (const FixtureStop&) {
        return faults == 1;
    }
    return false;
}

bool ThrowsTrap(X86Func* function, X86Context& ctx) {
    traps = 0;
    try {
        function(ctx, nullptr);
    } catch (const FixtureStop&) {
        return traps == 1;
    }
    return false;
}

}  // namespace

[[noreturn]] void x86_memory_fault(X86Context&, const X86MemoryFault& fault) {
    last_fault = fault;
    ++faults;
    throw FixtureStop{};
}

[[noreturn]] void x86_checked_trap(X86Context&, uint32_t pc, const char* reason) {
    last_trap_pc = pc;
    last_trap_reason = reason;
    ++traps;
    throw FixtureStop{};
}

extern "C" int RunCheckedRecompilerFixtureTests() {
    static_assert(sizeof(uint64_t) == 8);
    Check(mem::Init(), "production guest memory initializes");
    if (!mem::IsInitialized()) return 1;

    constexpr uint32_t source = 0x20FFE, destination = 0x30000;
    Check(mem::Commit(0x20000, 0x2000), "source pages commit independently at 4 KB granularity");
    Check(mem::Commit(destination, mem::kPage), "destination page commits");
    std::array<uint8_t, 24> input{};
    input[0] = 0x80;
    input[1] = 0x22;
    input[2] = 0x33;
    input[3] = 0x44;
    input[4] = 0x80;
    input[5] = 0x01;
    input[6] = 0x80;
    for (size_t i = 8; i < 16; ++i) input[i] = uint8_t(i * 17);
    Check(mem::Write(source, input.data(), input.size()) == mem::AccessStatus::kOk,
          "fixture input crosses a 4 KB guest-page boundary");

    X86Context ctx{};
    ctx.ebx = source;
    ctx.edi = destination;
    ctx.esi = source;
    ctx.ecx = 0xA1B2;
    ctx.cf = 1;
    ctx.zf = 0;
    ctx.sf = 1;
    ctx.of = 1;
    ctx.pf = 0;
    sub_00011000(ctx, nullptr);

    uint32_t dword = 0;
    std::array<uint8_t, 16> output{};
    Check(mem::Read(destination, &dword, sizeof(dword)) == mem::AccessStatus::kOk && dword == 0x44332280,
          "generated 32 bit load and store preserve little-endian value across pages");
    Check(mem::Read(destination, output.data(), output.size()) == mem::AccessStatus::kOk &&
              output[4] == 0x80 && output[5] == 0xB2 && output[6] == 0xA1,
          "generated 8 and 16 bit stores use correct widths");
    Check(ctx.ecx == 0x80 && ctx.edx == 0xFFFFFF80 && ctx.ebx == 0x8001 && ctx.ebp == 0xFFFF8001,
          "generated signed and unsigned byte and word loads extend correctly");
    uint64_t expected_qword = 0;
    std::memcpy(&expected_qword, input.data() + 8, sizeof(expected_qword));
    Check(ctx.mm[0] == expected_qword,
          "generated 64 bit MMX load uses checked guest memory");
    uint64_t qword = 0;
    Check(mem::Read(destination + 8, &qword, sizeof(qword)) == mem::AccessStatus::kOk && qword == ctx.mm[0],
          "generated 64 bit MMX store uses checked guest memory");
    Check(ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0,
          "scalar MOV instructions preserve guest flags");

    ctx = X86Context{};
    ctx.ebx = UINT32_MAX - 2;
    ctx.edi = destination;
    ctx.esi = source;
    ctx.eax = 0xDEADBEEF;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.instruction == 0x11000 &&
              last_fault.address == UINT32_MAX - 2 && last_fault.status == mem::AccessStatus::kInvalidAddress &&
              !last_fault.write && ctx.eax == 0xDEADBEEF,
          "generated overflow read reports faulting PC without changing destination register");

    ctx = X86Context{};
    ctx.ebx = 0x24000;
    ctx.edi = destination;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.instruction == 0x11000 &&
              last_fault.status == mem::AccessStatus::kUncommitted && !last_fault.write,
          "generated uncommitted read halts at instruction address");

    Check(mem::Protect(source & ~(mem::kPage - 1), mem::kPage, mem::kNoAccess) == mem::AccessStatus::kOk,
          "source page becomes inaccessible");
    ctx = X86Context{};
    ctx.ebx = source;
    ctx.edi = destination;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.instruction == 0x11000 && !last_fault.write,
          "generated denied read reports access direction and PC");
    Check(mem::Protect(source & ~(mem::kPage - 1), mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "source page becomes readable again");

    Check(mem::Protect(destination, mem::kPage, mem::kNoAccess) == mem::AccessStatus::kOk,
          "destination page becomes inaccessible");
    ctx = X86Context{};
    ctx.ebx = source;
    ctx.edi = destination;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.instruction == 0x11002 && last_fault.write,
          "generated denied write reports access direction and PC");
    Check(mem::Protect(destination, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "destination page protection restored");

    uint16_t prefix_before = 0;
    Check(mem::Write(0x30FFE, &prefix_before, sizeof(prefix_before)) == mem::AccessStatus::kOk,
          "partial destination prefix initializes");
    const uint16_t prefix = 0xBEEF;
    Check(mem::Write(0x30FFE, &prefix, sizeof(prefix)) == mem::AccessStatus::kOk,
          "destination prefix sentinel writes");
    ctx = X86Context{};
    ctx.ebx = source;
    ctx.edi = 0x30FFE;
    ctx.esi = source;
    Check(ThrowsMemoryFault(sub_00011000, ctx) && last_fault.instruction == 0x11002 &&
              last_fault.address == 0x30FFE && last_fault.status == mem::AccessStatus::kUncommitted && last_fault.write,
          "generated cross-page store rejects uncommitted tail at the store PC");
    uint16_t prefix_after = 0;
    Check(mem::Read(0x30FFE, &prefix_after, sizeof(prefix_after)) == mem::AccessStatus::kOk && prefix_after == prefix,
          "rejected generated cross-page store leaves committed prefix unchanged");

    const uint32_t rmw_va = destination + 0x20;
    const uint32_t maximum = UINT32_MAX;
    Check(mem::Write(rmw_va, &maximum, sizeof(maximum)) == mem::AccessStatus::kOk,
          "RMW input initialized");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    ctx.ecx = 0xCAFE;
    sub_00011022(ctx, nullptr);
    uint32_t rmw_result = 1;
    Check(mem::Read(rmw_va, &rmw_result, sizeof(rmw_result)) == mem::AccessStatus::kOk && rmw_result == 0 &&
              ctx.ecx == 0xCAFE,
          "generated memory ADD updates data and preserves zero-flag branch behavior");

    const uint32_t read_only_value = 41;
    Check(mem::Write(rmw_va, &read_only_value, sizeof(read_only_value)) == mem::AccessStatus::kOk &&
              mem::Protect(rmw_va & ~(mem::kPage - 1), mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "RMW target becomes read-only");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    ctx.ecx = 0xCAFE;
    Check(ThrowsMemoryFault(sub_00011022, ctx) && last_fault.instruction == 0x11022 &&
              last_fault.status == mem::AccessStatus::kPermissionDenied && last_fault.write && ctx.ecx == 0xCAFE &&
              ctx.cf == 0 && ctx.zf == 0 && ctx.sf == 0 && ctx.of == 0 && ctx.pf == 0,
          "generated RMW write fault leaves later instruction, flags, and guest registers untouched");
    Check(mem::Protect(rmw_va & ~(mem::kPage - 1), mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "RMW target protection restored");

    uint32_t rmw_value = 0;
    Check(mem::Write(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk,
          "SUB input initialized");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    sub_0001104A(ctx, nullptr);
    Check(mem::Read(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk && rmw_value == UINT32_MAX &&
              ctx.cf == 1 && ctx.sf == 1 && ctx.zf == 0,
          "generated memory SUB commits result and flags after checked store");

    rmw_value = UINT32_MAX;
    Check(mem::Write(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk,
          "INC input initialized");
    ctx = X86Context{};
    ctx.edi = rmw_va;
    ctx.cf = 1;
    sub_0001104E(ctx, nullptr);
    Check(mem::Read(rmw_va, &rmw_value, sizeof(rmw_value)) == mem::AccessStatus::kOk && rmw_value == 0 &&
              ctx.cf == 1 && ctx.zf == 1,
          "generated memory INC commits result while preserving carry flag");

    ctx = X86Context{};
    ctx.edi = destination;
    Check(ThrowsTrap(sub_0001102D, ctx) && last_trap_pc == 0x1102D &&
              last_trap_reason.find("unimplemented: xchg") != std::string::npos,
          "unsupported atomic memory instruction reaches checked-safe trap path");
    Check(ThrowsTrap(sub_00011030, ctx) && last_trap_pc == 0x11030 &&
              last_trap_reason.find("unimplemented:") != std::string::npos,
          "16-byte SIMD memory operation remains blocked in checked mode");
    Check(ThrowsTrap(sub_00011034, ctx) && last_trap_pc == 0x11034 &&
              last_trap_reason.find("unimplemented:") != std::string::npos,
          "10-byte x87 memory operation remains blocked in checked mode");
    Check(ThrowsTrap(sub_00011037, ctx) && last_trap_pc == 0x11037 &&
              last_trap_reason.find("unimplemented:") != std::string::npos,
          "locked XADD remains blocked in checked mode");
    Check(ThrowsTrap(sub_0001103C, ctx) && last_trap_pc == 0x1103C &&
              last_trap_reason.find("unimplemented:") != std::string::npos,
          "REP MOVSD remains blocked in checked mode");

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE_CLI
    RunReleaseBenchmarks(source, destination);
#endif

    mem::Shutdown();
    return failures ? 1 : 0;
}

#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE_CLI
int main() { return RunCheckedRecompilerFixtureTests(); }
#endif
