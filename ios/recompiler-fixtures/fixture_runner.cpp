#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <bit>
#include <exception>
#include <string>

#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "x86_recomp_shared.h"

namespace kernel {
uint8_t* g_base = nullptr;
}

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

struct SubFlags {
    uint32_t cf, pf, af, zf, sf, of;
};

uint64_t WidthMask(unsigned width) { return width == 32 ? UINT32_MAX : (uint64_t(1) << width) - 1; }

int64_t SignedValue(uint32_t value, unsigned width) {
    const uint64_t mask = WidthMask(width), sign = uint64_t(1) << (width - 1);
    value &= uint32_t(mask);
    return value & sign ? int64_t(value) - int64_t(uint64_t(1) << width) : value;
}

SubFlags ReferenceSubFlags(unsigned width, uint32_t left, uint32_t right) {
    const uint64_t mask = WidthMask(width);
    const uint64_t a = left & mask, b = right & mask, result = (a - b) & mask;
    const int64_t difference = SignedValue(uint32_t(a), width) - SignedValue(uint32_t(b), width);
    const int64_t minimum = -(int64_t(1) << (width - 1));
    const int64_t maximum = (int64_t(1) << (width - 1)) - 1;
    return {uint32_t(a < b), uint32_t(std::popcount(uint8_t(result)) % 2 == 0), uint32_t((a & 0xF) < (b & 0xF)),
            uint32_t(result == 0), uint32_t((result >> (width - 1)) & 1), uint32_t(difference < minimum || difference > maximum)};
}

bool Runs(X86Func* function, X86Context& ctx) {
    try {
        function(ctx, nullptr);
        return true;
    } catch (...) {
        return false;
    }
}

bool MatchesFlags(const X86Context& ctx, const SubFlags& expected) {
    return ctx.cf == expected.cf && ctx.pf == expected.pf && ctx.af == expected.af && ctx.zf == expected.zf &&
           ctx.sf == expected.sf && ctx.of == expected.of;
}

bool SeedCmpFlags(X86Context& ctx) {
    ctx.cf = 1;
    ctx.pf = 0;
    ctx.af = 1;
    ctx.zf = 0;
    ctx.sf = 1;
    ctx.of = 0;
    return true;
}

void CheckCmpTests(uint32_t destination) {
    struct Case {
        X86Func* function;
        unsigned width;
        uint32_t left, right;
        const char* name;
    };
    const Case cases[] = {
        {sub_00011054, 8, 0x80, 1, "8-bit negative CMP flags match independent subtraction reference"},
        {sub_00011054, 8, 0x7F, 0xFF, "8-bit signed overflow and parity flags match reference"},
        {sub_00011054, 8, 0, 1, "8-bit carry and half-carry flags match reference"},
        {sub_00011054, 8, 0x55, 0x55, "8-bit zero and parity flags match reference"},
        {sub_00011057, 16, 0x8000, 1, "16-bit negative CMP flags match independent subtraction reference"},
        {sub_00011057, 16, 0x7FFF, 0xFFFF, "16-bit signed overflow flags match reference"},
        {sub_00011057, 16, 0x10, 1, "16-bit half-carry flags match reference"},
        {sub_00011051, 32, 0x80000000, 1, "32-bit negative CMP flags match independent subtraction reference"},
        {sub_00011051, 32, 0x7FFFFFFF, 0xFFFFFFFF, "32-bit signed overflow flags match reference"},
        {sub_00011051, 32, 0, 1, "32-bit carry and half-carry flags match reference"},
        {sub_00011051, 32, 0x101, 1, "32-bit parity uses low result byte"},
        {sub_00011051, 32, 9, 9, "32-bit zero comparison flags match reference"},
    };
    for (const Case& test : cases) {
        X86Context ctx{};
        ctx.eax = test.width == 32 ? test.left : test.left | 0xA5A50000;
        ctx.ebx = test.width == 32 ? test.right : test.right | 0x5A5A0000;
        const uint32_t eax_before = ctx.eax, ebx_before = ctx.ebx;
        SeedCmpFlags(ctx);
        const SubFlags expected = ReferenceSubFlags(test.width, test.left, test.right);
        Check(Runs(test.function, ctx) && ctx.eax == eax_before && ctx.ebx == ebx_before && MatchesFlags(ctx, expected), test.name);
    }

    X86Context ctx{};
    ctx.eax = 0x7FFFFFFF;
    const SubFlags immediate32 = ReferenceSubFlags(32, ctx.eax, 0x80000000);
    SeedCmpFlags(ctx);
    Check(Runs(sub_0001105B, ctx) && ctx.eax == 0x7FFFFFFF && MatchesFlags(ctx, immediate32),
          "32-bit immediate CMP uses destination operand width");
    ctx = X86Context{};
    ctx.eax = UINT32_MAX;
    const SubFlags signed_imm8 = ReferenceSubFlags(32, ctx.eax, UINT32_MAX);
    Check(Runs(sub_00011061, ctx) && ctx.eax == UINT32_MAX && MatchesFlags(ctx, signed_imm8),
          "sign-extended imm8 CMP compares against -1 at 32-bit width");

    constexpr uint32_t unaligned = 0x30001;
    uint32_t memory_value = 0x44332280;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "CMP unaligned memory fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0x4433227F;
    ctx.esi = unaligned;
    const SubFlags memory_flags = ReferenceSubFlags(32, ctx.eax, memory_value);
    Check(Runs(sub_00011068, ctx) && ctx.eax == 0x4433227F && MatchesFlags(ctx, memory_flags),
          "CMP register-memory uses checked unaligned dword read and exact arithmetic flags");

    const uint32_t memory_dest = destination + 0x100;
    memory_value = 0xABCDEF01;
    Check(mem::Write(memory_dest, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "CMP memory destination fixture initializes");
    ctx = X86Context{};
    ctx.eax = memory_value - 1;
    ctx.esi = memory_dest;
    const SubFlags destination_flags = ReferenceSubFlags(32, memory_value, ctx.eax);
    uint32_t register_left = ctx.eax;
    Check(Runs(sub_00011065, ctx) && ctx.eax == register_left &&
              mem::Read(memory_dest, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk &&
              memory_value == 0xABCDEF01 && MatchesFlags(ctx, destination_flags),
          "CMP memory-register performs read-only operand access without destination write");

    uint8_t byte_value = 0x7F;
    Check(mem::Write(unaligned, &byte_value, sizeof(byte_value)) == mem::AccessStatus::kOk,
          "byte CMP fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0xA5A50080;
    ctx.esi = unaligned;
    Check(Runs(sub_0001106B, ctx) && ctx.eax == 0xA5A50080 && MatchesFlags(ctx, ReferenceSubFlags(8, 0x7F, 0x80)),
          "CMP byte memory operand uses 8-bit flags and preserves upper register bits");

    uint16_t word_value = 0x8001;
    Check(mem::Write(unaligned, &word_value, sizeof(word_value)) == mem::AccessStatus::kOk,
          "word CMP fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0x12348000;
    ctx.esi = unaligned;
    Check(Runs(sub_0001106E, ctx) && ctx.eax == 0x12348000 && MatchesFlags(ctx, ReferenceSubFlags(16, 0x8001, 0x8000)),
          "CMP word memory operand uses 16-bit flags and preserves upper register bits");

    const uint32_t scaled = 0x30204;
    memory_value = 0x11223344;
    Check(mem::Write(scaled, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "scaled CMP address fixture initializes");
    ctx = X86Context{};
    ctx.ebx = 0x30200;
    ctx.esi = 2;
    ctx.eax = memory_value + 1;
    Check(Runs(sub_00011072, ctx) && MatchesFlags(ctx, ReferenceSubFlags(32, memory_value, memory_value + 1)),
          "CMP SIB address applies scale, base, and signed displacement");

    memory_value = UINT32_MAX;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "memory-immediate CMP fixture initializes");
    ctx = X86Context{};
    ctx.esi = unaligned;
    Check(Runs(sub_00011077, ctx) && MatchesFlags(ctx, ReferenceSubFlags(32, UINT32_MAX, UINT32_MAX)),
          "memory CMP sign-extends imm8 without writing operand");

    memory_value = 3;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "CMP branch fixture initializes");
    ctx = X86Context{};
    ctx.eax = 4;
    ctx.esi = unaligned;
    Check(Runs(sub_0001107B, ctx) && ctx.eax == 2,
          "CMP feeds correct unsigned not-below conditional branch");
    ctx = X86Context{};
    ctx.eax = 2;
    ctx.esi = unaligned;
    Check(Runs(sub_0001107B, ctx) && ctx.eax == 1,
          "CMP carry feeds opposite unsigned conditional branch");
    memory_value = 0;
    mem::Write(unaligned, &memory_value, sizeof(memory_value));
    ctx = X86Context{};
    ctx.esi = unaligned;
    Check(Runs(sub_0001108B, ctx) && ctx.eax == 2 && ctx.pf == 1,
          "CMP parity flag drives subsequent JP branch");
    ctx = X86Context{};
    ctx.eax = 1;
    ctx.esi = unaligned;
    Check(Runs(sub_0001108B, ctx) && ctx.eax == 1 && ctx.pf == 0,
          "CMP odd-result parity skips subsequent JP branch");

    memory_value = 2;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "signed CMP branch fixture initializes");
    ctx = X86Context{};
    ctx.eax = 1;
    ctx.esi = unaligned;
    Check(Runs(sub_0001109B, ctx) && ctx.eax == 2,
          "CMP signed less-than branch uses SF and OF for negative result");
    memory_value = 0x80000000;
    Check(mem::Write(unaligned, &memory_value, sizeof(memory_value)) == mem::AccessStatus::kOk,
          "signed-overflow CMP branch fixture initializes");
    ctx = X86Context{};
    ctx.eax = 0x7FFFFFFF;
    ctx.esi = unaligned;
    Check(Runs(sub_0001109B, ctx) && ctx.eax == 1,
          "CMP signed less-than branch rejects negative wrapped result when OF is set");

    const auto check_fault_unchanged = [&](uint32_t base, uint32_t index, uint32_t address, uint32_t pc,
                                           mem::AccessStatus status, const char* name, X86Func* function) {
        X86Context fault_ctx{};
        fault_ctx.eax = 0xDEADBEEF;
        fault_ctx.ebx = base;
        fault_ctx.esi = index;
        fault_ctx.esp = 0x12345678;
        fault_ctx.cf = 1; fault_ctx.pf = 0; fault_ctx.af = 1; fault_ctx.zf = 0; fault_ctx.sf = 1; fault_ctx.of = 0;
        const X86Context before = fault_ctx;
        Check(ThrowsMemoryFault(function, fault_ctx) && last_fault.instruction == pc && last_fault.address == address &&
                  last_fault.status == status && !last_fault.write && fault_ctx.eax == before.eax &&
                  fault_ctx.ebx == before.ebx && fault_ctx.esi == before.esi && fault_ctx.esp == before.esp &&
                  fault_ctx.cf == before.cf && fault_ctx.pf == before.pf && fault_ctx.af == before.af &&
                  fault_ctx.zf == before.zf && fault_ctx.sf == before.sf && fault_ctx.of == before.of,
              name);
    };
    check_fault_unchanged(0, 0x24000, 0x24000, 0x11068, mem::AccessStatus::kUncommitted,
                          "CMP uncommitted read preserves all registers and arithmetic flags", sub_00011068);
    check_fault_unchanged(0, 0x21FFF, 0x21FFF, 0x11068, mem::AccessStatus::kUncommitted,
                          "CMP cross-page read reports starting guest address and fault PC", sub_00011068);
    Check(mem::Commit(0x40000, mem::kPage) && mem::Protect(0x40000, mem::kPage, mem::kWrite) == mem::AccessStatus::kOk,
          "CMP denied-read fixture page becomes write-only");
    check_fault_unchanged(0, 0x40000, 0x40000, 0x11068, mem::AccessStatus::kPermissionDenied,
                          "CMP permission denial preserves registers and flags", sub_00011068);
    check_fault_unchanged(3, 0, UINT32_MAX, 0x11072, mem::AccessStatus::kInvalidAddress,
                          "CMP SIB address overflow reports wrapped guest address", sub_00011072);

    X86Context denied{};
    denied.esi = 0x40000;
    Check(ThrowsMemoryFault(sub_00011068, denied) && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.instruction == 0x11068 && last_fault.address == 0x40000 && !last_fault.write,
          "CMP reports exact memory-read permission fault");
    Check(mem::Protect(0x40000, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "CMP permission test restores guest page");
}

struct AndFlags {
    uint32_t cf, zf, sf, of, pf;
};

AndFlags ReferenceAndFlags(uint32_t left, uint32_t right, unsigned width) {
    const uint32_t mask = width == 32 ? UINT32_MAX : (1u << width) - 1;
    const uint32_t result = (left & right) & mask;
    return {0, uint32_t(result == 0), (result >> (width - 1)) & 1, 0,
            uint32_t(std::popcount(result & 0xFF) % 2 == 0)};
}

bool MatchesAndFlags(const X86Context& ctx, const AndFlags& flags) {
    return ctx.cf == flags.cf && ctx.zf == flags.zf && ctx.sf == flags.sf && ctx.of == flags.of &&
           ctx.pf == flags.pf;
}

bool SameCoreState(const X86Context& left, const X86Context& right) {
    return left.eax == right.eax && left.ecx == right.ecx && left.edx == right.edx && left.ebx == right.ebx &&
           left.esp == right.esp && left.ebp == right.ebp && left.esi == right.esi && left.edi == right.edi &&
           left.cf == right.cf && left.zf == right.zf && left.sf == right.sf && left.of == right.of &&
           left.pf == right.pf && left.af == right.af && left.df == right.df && left.xflags == right.xflags;
}

void SeedFlags(X86Context& ctx);

void CheckCallTests(uint32_t destination) {
    constexpr uint32_t stack = 0x74000, top = stack + mem::kPage;
    Check(mem::Commit(stack, mem::kPage), "CALL fixture stack page commits");

    X86Context ctx{};
    ctx.esp = top;
    SeedFlags(ctx);
    Check(Runs(sub_000110F5, ctx) && ctx.eax == 0x12345678 && ctx.ebx == 0x55667788 && ctx.esp == top + 4 &&
              ctx.last_icall == 0x110EF && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
              ctx.af == 1,
          "direct CALL dispatches generated target and returns to guest continuation");
    uint32_t return_address = 0;
    Check(mem::Read(top - 4, &return_address, sizeof(return_address)) == mem::AccessStatus::kOk &&
              return_address == 0x110FA,
          "direct CALL stores exact guest return address at ESP minus four");

    ctx = X86Context{};
    ctx.esp = top;
    Check(Runs(sub_0001110D, ctx) && ctx.eax == 3 && ctx.ebx == 0x44332211 && ctx.esp == top + 4 && ctx.cf == 0 &&
              ctx.zf == 0 && ctx.sf == 0 && ctx.of == 0 && ctx.pf == 1 && ctx.af == 0,
          "nested direct CALLs return in LIFO order through generated continuations");
    uint32_t returns[2]{};
    Check(mem::Read(top - 8, returns, sizeof(returns)) == mem::AccessStatus::kOk && returns[0] == 0x11109 &&
              returns[1] == 0x11117,
          "nested CALLs preserve inner and outer guest return addresses");

    ctx = X86Context{};
    ctx.esp = top;
    ctx.edi = destination;
    SeedFlags(ctx);
    Check(Runs(sub_00011120, ctx) && ctx.esp == top + 12,
          "checked RET imm16 applies guest callee stack cleanup");
    uint32_t post_call_esp = 0;
    Check(mem::Read(destination, &post_call_esp, sizeof(post_call_esp)) == mem::AccessStatus::kOk &&
              post_call_esp == top + 8 && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
              ctx.af == 1,
          "RET imm16 continuation observes ESP after return address and immediate cleanup");

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_00011130, ctx) && last_trap_pc == 0x1112F &&
              last_trap_reason.find("return address") != std::string::npos && ctx.esp == top - 4,
          "checked RET traps corrupted guest return address before changing ESP");

    ctx = X86Context{};
    ctx.esp = 0x78000;
    const X86Context before_write_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110F5, ctx) && last_fault.instruction == 0x110F5 &&
              last_fault.address == 0x77FFC && last_fault.status == mem::AccessStatus::kUncommitted && last_fault.write &&
              SameCoreState(ctx, before_write_fault),
          "CALL stack write fault reports CALL PC and leaves ESP unchanged");

    constexpr uint32_t read_only_stack = 0x76000;
    Check(mem::Commit(read_only_stack, mem::kPage) &&
              mem::Protect(read_only_stack, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "CALL denied-write fixture page becomes read-only");
    ctx = X86Context{};
    ctx.esp = read_only_stack + mem::kPage;
    const X86Context before_denied_write = ctx;
    Check(ThrowsMemoryFault(sub_000110F5, ctx) && last_fault.instruction == 0x110F5 &&
              last_fault.address == ctx.esp - 4 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.write && SameCoreState(ctx, before_denied_write),
          "CALL denied stack write preserves ESP and reports permission failure");
    Check(mem::Protect(read_only_stack, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "CALL denied-write fixture page becomes writable again");

    ctx = X86Context{};
    ctx.esp = top;
    ctx.ebx = 0x78000;
    Check(ThrowsMemoryFault(sub_0001114E, ctx) && last_fault.instruction == 0x1114D &&
              last_fault.address == 0x78000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && ctx.esp == 0x78000 && ctx.checked_call == nullptr,
          "RET stack read fault reports RET PC and address without advancing ESP");

    ctx = X86Context{};
    ctx.esp = top;
    const uint32_t no_args = 0;
    const uint32_t host_result = kernel::CallGuest(ctx, 0x110EF, &no_args, 0);
    Check(host_result == 0x12345678 && ctx.esp == top && ctx.checked_call == nullptr &&
              mem::Read(top - 4, &return_address, sizeof(return_address)) == mem::AccessStatus::kOk &&
              return_address == 0xFFFFFFF0,
          "host CallGuest validates its explicit sentinel return without a guest continuation");

    X86CheckedCallFrame outer_frame{nullptr, 0xCAFEBABE, false};
    ctx = X86Context{};
    ctx.esp = top;
    ctx.checked_call = &outer_frame;
    const uint32_t nested_host_result = kernel::CallGuest(ctx, 0x110EF, &no_args, 0);
    Check(nested_host_result == 0x12345678 && ctx.esp == top && ctx.checked_call == &outer_frame &&
              !outer_frame.returned,
          "host CallGuest sentinel frame shadows and restores an enclosing checked CALL frame");

    ctx = X86Context{};
    ctx.esp = top;
    ctx.checked_call = &outer_frame;
    traps = 0;
    try {
        kernel::CallGuest(ctx, 0x11128, &no_args, 0);
    } catch (const FixtureStop&) {
    }
    Check(traps == 1 && last_trap_pc == 0x1112F &&
              last_trap_reason.find("expected FFFFFFF0, found DEADBEEF") != std::string::npos &&
              ctx.esp == top - 4 && ctx.checked_call == &outer_frame && !outer_frame.returned,
          "host CallGuest rejects a corrupted sentinel and restores the enclosing checked frame");
    ctx.checked_call = nullptr;

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_0001113C, ctx) && last_trap_pc == 0x1113C &&
              last_trap_reason.find("unimplemented: call") != std::string::npos && ctx.esp == top,
          "indirect CALL remains trapped without changing guest stack");

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_0001113F, ctx) && last_trap_pc == 0x1113F &&
              last_trap_reason.find("unimplemented: call") != std::string::npos && ctx.esp == top,
          "direct CALL to a target absent from generated metadata remains trapped");

    ctx = X86Context{};
    ctx.esp = top;
    Check(ThrowsTrap(sub_00011145, ctx) && last_trap_pc == 0x11145 &&
              last_trap_reason.find("unimplemented: call") != std::string::npos && ctx.esp == top,
          "CALL $+5 remains trapped because fallthrough is not an audited target");

    ctx = X86Context{};
    ctx.esp = top;
    traps = 0;
    try {
        x86_checked_call(ctx, nullptr, 0x11200, 0x22222222, 0x1113F);
    } catch (const FixtureStop&) {
    }
    Check(traps == 1 && last_trap_pc == 0x1113F && ctx.esp == top &&
              last_trap_reason.find("not mapped") != std::string::npos,
          "runtime checked CALL rejects a target missing from the dispatch table before stack write");

    const uint32_t continuation = 0x11136;
    const dispatch::Metadata metadata{X86CodeBase, X86CodeEnd, X86EntryPoint,
                                      std::span<const X86FuncMapping>(X86FuncMappings, X86FuncMappingCount),
                                      std::span<const uint32_t>(&continuation, 1)};
    std::string error;
    Check(dispatch::Init(metadata, error), "dispatch table accepts generated fixture continuation metadata");

    X86CheckedCallFrame resumed_frame{nullptr, 0x22222222, false};
    ctx = X86Context{};
    ctx.esp = top - 4;
    ctx.checked_call = &resumed_frame;
    return_address = resumed_frame.return_address;
    Check(mem::Write(ctx.esp, &return_address, sizeof(return_address)) == mem::AccessStatus::kOk,
          "valid-resume stack return address initializes");
    x86_resume(ctx, nullptr, X86Resume{continuation, ctx.esp});
    Check(ctx.ecx == 0x12345678 && ctx.esp == top && ctx.checked_call == nullptr && resumed_frame.returned,
          "valid X86Resume continuation dispatches generated code and validates its RET");

    ctx = X86Context{};
    ctx.esp = top;
    traps = 0;
    bool continuation_rejected = false;
    try {
        x86_checked_call(ctx, nullptr, continuation, 0x22222222, 0x11145);
    } catch (const FixtureStop&) {
        continuation_rejected = true;
    } catch (...) {
    }
    Check(continuation_rejected && traps == 1 && ctx.esp == top && ctx.checked_call == nullptr &&
              last_trap_reason.find("continuation") != std::string::npos,
          "direct CALL rejects a generated continuation before changing the guest stack");

    Check(dispatch::Init(error), "dispatch table restores generated fixture metadata after continuation test");
}

void SeedFlags(X86Context& ctx) {
    ctx.cf = 1;
    ctx.zf = 0;
    ctx.sf = 1;
    ctx.of = 1;
    ctx.pf = 0;
    ctx.af = 1;
}

void CheckEntryChainTests() {
    constexpr uint32_t stack = 0x7A000;
    Check(mem::Commit(stack, mem::kPage), "entry-chain fixture stack commits");

    X86Context ctx{};
    ctx.esp = stack + 0x100;
    ctx.eax = 0x11223344;
    ctx.ecx = 0x55667788;
    ctx.esi = 0x99AABBCC;
    SeedFlags(ctx);
    const uint32_t popped = 0xA1B2C3D4;
    Check(mem::Write(ctx.esp, &popped, sizeof(popped)) == mem::AccessStatus::kOk,
          "POP ECX stack input initializes");
    Check(Runs(sub_00011154, ctx) && ctx.ecx == popped && ctx.esp == stack + 0x108 && ctx.eax == 0x11223344 &&
              ctx.esi == 0x99AABBCC && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
              ctx.af == 1,
          "checked POP ECX reads dword, advances ESP after success, preserves other registers and flags");

    constexpr uint32_t pop_guard = 0x7C000;
    Check(mem::Commit(pop_guard + mem::kPage, mem::kPage), "POP fixture leaves lower guard page uncommitted");
    ctx = X86Context{};
    ctx.esp = pop_guard;
    ctx.ecx = 0xCAFEBABE;
    SeedFlags(ctx);
    const X86Context before_pop_fault = ctx;
    Check(ThrowsMemoryFault(sub_00011154, ctx) && last_fault.instruction == 0x11154 &&
              last_fault.address == pop_guard && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_pop_fault),
          "POP guard-page read fault reports guest PC and preserves ECX, ESP, and flags");

    constexpr uint32_t pop_cross = 0x7E000;
    Check(mem::Commit(pop_cross, mem::kPage), "POP fixture commits page below unreadable boundary");
    ctx = X86Context{};
    ctx.esp = pop_cross + mem::kPage - 2;
    ctx.ecx = 0x13579BDF;
    SeedFlags(ctx);
    const X86Context before_pop_cross_fault = ctx;
    Check(ThrowsMemoryFault(sub_00011154, ctx) && last_fault.instruction == 0x11154 &&
              last_fault.address == ctx.esp && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && SameCoreState(ctx, before_pop_cross_fault),
          "POP cross-page read fault validates full dword before changing guest state");

    ctx = X86Context{};
    ctx.esp = stack + 0x180;
    ctx.eax = 0x12345678;
    Check(ThrowsTrap(sub_00011156, ctx) && last_trap_pc == 0x11156 && ctx.esp == stack + 0x180 &&
              ctx.eax == 0x12345678,
          "unsupported POP EAX stays blocked before stack access");

    struct CdqCase { uint32_t eax, edx; };
    for (const CdqCase test : {CdqCase{0, 0}, CdqCase{0x7FFFFFFF, 0}, CdqCase{0x80000000, UINT32_MAX}}) {
        ctx = X86Context{};
        ctx.eax = test.eax;
        ctx.edx = 0x12345678;
        SeedFlags(ctx);
        Check(Runs(sub_00011158, ctx) && ctx.eax == test.eax && ctx.edx == test.edx && ctx.cf == 1 && ctx.zf == 0 &&
                  ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 && ctx.af == 1,
              "checked CDQ sign-extends EAX into EDX without changing flags");
    }
    ctx = X86Context{};
    Check(ThrowsTrap(sub_00011160, ctx) && last_trap_pc == 0x11160,
          "CWD remains unsupported while checked CDQ is enabled");

    const auto set_dividend = [](X86Context& state, int64_t dividend) {
        const uint64_t bits = std::bit_cast<uint64_t>(dividend);
        state.eax = uint32_t(bits);
        state.edx = uint32_t(bits >> 32);
    };
    const auto divide = [&](int64_t dividend, int32_t divisor, uint32_t quotient, uint32_t remainder,
                            const char* message) {
        ctx = X86Context{};
        ctx.esp = stack + 0x200;
        set_dividend(ctx, dividend);
        ctx.ecx = std::bit_cast<uint32_t>(divisor);
        SeedFlags(ctx);
        Check(Runs(sub_0001115A, ctx) && ctx.eax == quotient && ctx.edx == remainder && ctx.ecx == std::bit_cast<uint32_t>(divisor) &&
                  ctx.esp == stack + 0x204 && ctx.cf == 1 && ctx.zf == 0 && ctx.sf == 1 && ctx.of == 1 && ctx.pf == 0 &&
                  ctx.af == 1,
              message);
    };
    divide(17, 5, 3, 2, "checked IDIV computes positive quotient and remainder");
    divide(-17, 5, std::bit_cast<uint32_t>(int32_t(-3)), std::bit_cast<uint32_t>(int32_t(-2)),
           "checked IDIV truncates negative dividend toward zero with signed remainder");
    divide(17, -5, std::bit_cast<uint32_t>(int32_t(-3)), 2,
           "checked IDIV handles negative divisor with signed quotient");
    divide(INT32_MIN, 1, std::bit_cast<uint32_t>(INT32_MIN), 0,
           "checked IDIV accepts exact INT32_MIN quotient");

    const auto divide_trap = [&](int64_t dividend, int32_t divisor, const char* reason_fragment, const char* message) {
        ctx = X86Context{};
        ctx.esp = stack + 0x280;
        set_dividend(ctx, dividend);
        ctx.ecx = std::bit_cast<uint32_t>(divisor);
        SeedFlags(ctx);
        const X86Context before = ctx;
        Check(ThrowsTrap(sub_0001115A, ctx) && last_trap_pc == 0x1115A &&
                  last_trap_reason.find(reason_fragment) != std::string::npos && SameCoreState(ctx, before),
              message);
    };
    divide_trap(1, 0, "division by zero", "checked IDIV zero divisor stops at guest PC and preserves guest state");
    divide_trap(INT32_MIN, -1, "quotient overflow", "checked IDIV detects INT32_MIN divided by -1 overflow");
    divide_trap(int64_t(INT32_MIN) - 1, 1, "quotient overflow", "checked IDIV rejects negative quotient outside int32 range");
    divide_trap(int64_t(INT32_MAX) + 1, 1, "quotient overflow", "checked IDIV rejects positive quotient outside int32 range");
    divide_trap(INT64_MIN, -1, "quotient overflow", "checked IDIV guards signed 64-bit host division overflow");
    Check(ThrowsTrap(sub_00011163, ctx) && last_trap_pc == 0x11163,
          "memory-source IDIV remains rejected in checked mode");

    ctx = X86Context{};
    ctx.esp = stack + 0x300;
    ctx.eax = 0x12345678;
    ctx.esi = 0xCAFEBABE;
    SeedFlags(ctx);
    Check(Runs(sub_0001115D, ctx) && ctx.esi == 0 && ctx.eax == 0x12345678 && ctx.esp == stack + 0x304 && ctx.cf == 0 &&
              ctx.of == 0 && ctx.zf == 1 && ctx.sf == 0 && ctx.pf == 1 && ctx.af == 1,
          "checked XOR ESI,ESI sets defined flags and preserves undefined AF");
}

void CheckPushLeaAndTests() {
    constexpr uint32_t stack = 0x60000;
    Check(mem::Commit(stack, mem::kPage), "checked PUSH fixture stack page commits");
    X86Context ctx{};
    ctx.esp = stack + 0x104;
    ctx.esi = 0xAABBCCDD;
    SeedFlags(ctx);
    sub_000110AB(ctx, nullptr);
    uint32_t word = 0;
    Check(ctx.esp == stack + 0x104 && mem::Read(stack + 0x100, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == 0xAABBCCDD && ctx.cf == 1 && ctx.af == 1,
          "PUSH register decrements 32-bit ESP, writes little-endian dword, and RET restores ESP");

    ctx = X86Context{};
    ctx.esp = stack + 0x114;
    Check(Runs(sub_000110AD, ctx) && mem::Read(stack + 0x110, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == UINT32_MAX - 3,
          "PUSH imm8 sign-extends -4 to a 32-bit stack value");
    ctx = X86Context{};
    ctx.esp = stack + 0x124;
    Check(Runs(sub_000110B0, ctx) && mem::Read(stack + 0x120, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == 0x12345678,
          "PUSH imm32 writes full little-endian value");
    ctx = X86Context{};
    ctx.esp = stack + 0x134;
    const uint32_t esp_before = ctx.esp;
    Check(Runs(sub_000110B6, ctx) && mem::Read(stack + 0x130, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == esp_before,
          "PUSH ESP stores pre-decrement ESP value");
    ctx = X86Context{};
    ctx.esp = stack + 0x150;
    ctx.esi = 0x10203040;
    Check(Runs(sub_000110BB, ctx) && ctx.esp == stack + 0x14C &&
              mem::Read(stack + 0x148, &word, sizeof(word)) == mem::AccessStatus::kOk && word == UINT32_MAX - 3 &&
              mem::Read(stack + 0x14C, &word, sizeof(word)) == mem::AccessStatus::kOk && word == 0x10203040,
          "repeated PUSH operations preserve LIFO stack contents");

    constexpr uint32_t guard = 0x70000;
    Check(mem::Commit(guard, mem::kPage), "PUSH guard fixture commits only upper stack page");
    ctx = X86Context{};
    ctx.esp = guard + 4;
    ctx.esi = 0x55667788;
    Check(Runs(sub_000110AB, ctx) && mem::Read(guard, &word, sizeof(word)) == mem::AccessStatus::kOk &&
              word == 0x55667788,
          "PUSH succeeds at committed stack boundary");

    const auto check_push_fault = [&](uint32_t esp, uint32_t address, mem::AccessStatus status, const char* message) {
        X86Context failed{};
        failed.esp = esp;
        failed.esi = 0xCAFEBABE;
        SeedFlags(failed);
        const X86Context before = failed;
        Check(ThrowsMemoryFault(sub_000110AB, failed) && last_fault.instruction == 0x110AB &&
                  last_fault.address == address && last_fault.status == status && last_fault.write &&
                  SameCoreState(failed, before), message);
    };
    check_push_fault(guard, guard - 4, mem::AccessStatus::kUncommitted,
                     "PUSH guard-page fault leaves ESP, registers, and flags unchanged");
    check_push_fault(guard + 2, guard - 2, mem::AccessStatus::kUncommitted,
                     "PUSH cross-page fault validates full dword before changing ESP");
    Check(mem::Protect(guard, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "PUSH read-only stack fixture protection set");
    check_push_fault(guard + 4, guard, mem::AccessStatus::kPermissionDenied,
                     "PUSH denied stack write preserves architectural state");
    Check(mem::Protect(guard, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "PUSH stack protection restored");

    ctx = X86Context{};
    ctx.esp = guard + 4;
    Check(ThrowsTrap(sub_000110B8, ctx) && last_trap_pc == 0x110B8 && ctx.esp == guard + 4,
          "PUSH memory operand remains blocked until read/write ordering is supported");

    ctx = X86Context{};
    ctx.eax = 0xFFFFFFF0;
    ctx.ecx = 0x30;
    SeedFlags(ctx);
    const uint32_t old_flags = ctx.cf | ctx.zf << 1 | ctx.sf << 2 | ctx.of << 3 | ctx.pf << 4 | ctx.af << 5;
    Check(Runs(sub_000110BF, ctx) && ctx.eax == 0x2F &&
              (ctx.cf | ctx.zf << 1 | ctx.sf << 2 | ctx.of << 3 | ctx.pf << 4 | ctx.af << 5) == old_flags,
          "LEA computes base/index/displacement with 32-bit wrap and preserves flags without reading memory");
    ctx = X86Context{};
    ctx.ebx = 0x1000;
    ctx.esi = 3;
    Check(Runs(sub_000110C4, ctx) && ctx.eax == 0x1008,
          "LEA applies SIB scale and signed displacement");
    ctx = X86Context{};
    ctx.ebx = 0x1234FFFF;
    Check(Runs(sub_000110C9, ctx) && ctx.eax == 0,
          "LEA address-size override wraps effective address to 16 bits");

    ctx = X86Context{};
    ctx.eax = 0xF0F0F0F0;
    ctx.ebx = 0x0F0F0F0F;
    SeedFlags(ctx);
    AndFlags flags = ReferenceAndFlags(ctx.eax, ctx.ebx, 32);
    const uint32_t old_af = ctx.af;
    Check(Runs(sub_000110CE, ctx) && ctx.eax == 0 && MatchesAndFlags(ctx, flags) && ctx.af == old_af,
          "AND register form updates defined flags and preserves undefined AF");
    ctx = X86Context{};
    ctx.eax = UINT32_MAX;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(UINT32_MAX, UINT32_MAX - 127, 32);
    Check(Runs(sub_000110D1, ctx) && ctx.eax == UINT32_MAX - 127 && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND sign-extends imm8 -128 to dword result and flags");
    ctx = X86Context{};
    ctx.eax = 0xABCD00A5;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(0xA5, 0xF0, 8);
    Check(Runs(sub_000110DB, ctx) && ctx.eax == 0xABCD00A0 && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND byte immediate preserves upper register bits and undefined AF");
    ctx = X86Context{};
    ctx.eax = 0xAAAAFFFF;
    ctx.ebx = 0xBBBB000F;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(0xFFFF, 0x000F, 16);
    Check(Runs(sub_000110DE, ctx) && ctx.eax == 0xAAAA000F && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND word register form preserves upper register bits and undefined AF");

    constexpr uint32_t and_memory = 0x62000;
    Check(mem::Commit(and_memory, mem::kPage), "AND memory fixture page commits");
    uint32_t memory = 0xF0F0F0F0;
    Check(mem::Write(and_memory, &memory, sizeof(memory)) == mem::AccessStatus::kOk,
          "AND memory destination initializes");
    ctx = X86Context{};
    ctx.esi = and_memory;
    ctx.eax = 0x0FF00FF0;
    SeedFlags(ctx);
    const uint32_t expected_memory = memory & ctx.eax;
    flags = ReferenceAndFlags(memory, ctx.eax, 32);
    Check(Runs(sub_000110D5, ctx) && mem::Read(and_memory, &memory, sizeof(memory)) == mem::AccessStatus::kOk &&
              memory == expected_memory && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND register to memory writes result and updates defined flags");

    memory = 0x800000F0;
    Check(mem::Write(and_memory + 4, &memory, sizeof(memory)) == mem::AccessStatus::kOk,
          "AND memory source initializes");
    ctx = X86Context{};
    ctx.esi = and_memory + 4;
    ctx.eax = UINT32_MAX;
    SeedFlags(ctx);
    flags = ReferenceAndFlags(ctx.eax, memory, 32);
    Check(Runs(sub_000110D8, ctx) && ctx.eax == memory && MatchesAndFlags(ctx, flags) && ctx.af == 1,
          "AND memory to register checks source read and sets flags");

    uint32_t byte_memory = UINT32_MAX;
    Check(mem::Write(and_memory + 8, &byte_memory, sizeof(byte_memory)) == mem::AccessStatus::kOk,
          "AND memory immediate initializes");
    ctx = X86Context{};
    ctx.esi = and_memory + 8;
    flags = ReferenceAndFlags(UINT32_MAX, 0x0F, 32);
    Check(Runs(sub_000110E2, ctx) && mem::Read(and_memory + 8, &memory, sizeof(memory)) == mem::AccessStatus::kOk &&
              memory == 0x0F && MatchesAndFlags(ctx, flags),
          "AND memory immediate uses checked read-modify-write");

    memory = 0x13579BDF;
    Check(mem::Write(and_memory + 0x10, &memory, sizeof(memory)) == mem::AccessStatus::kOk,
          "AND denied-write sentinel initializes");
    Check(mem::Protect(and_memory, mem::kPage, mem::kRead) == mem::AccessStatus::kOk,
          "AND destination page made read-only");
    ctx = X86Context{};
    ctx.esi = and_memory + 0x10;
    ctx.eax = 0xFFFFFFFF;
    SeedFlags(ctx);
    const X86Context before_and_write_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110D5, ctx) && last_fault.instruction == 0x110D5 &&
              last_fault.address == and_memory + 0x10 && last_fault.status == mem::AccessStatus::kPermissionDenied &&
              last_fault.write && SameCoreState(ctx, before_and_write_fault) &&
              mem::Read(and_memory + 0x10, &memory, sizeof(memory)) == mem::AccessStatus::kOk &&
              memory == 0x13579BDF,
          "AND denied write preserves memory, destination registers, and prior flags");
    Check(mem::Protect(and_memory, mem::kPage, mem::kReadWrite) == mem::AccessStatus::kOk,
          "AND destination protection restored");

    ctx = X86Context{};
    ctx.eax = 0xF0;
    ctx.ebx = 0x0F;
    ctx.ecx = 0xAABBCCDD;
    ctx.esi = 0x24000;
    SeedFlags(ctx);
    Check(ThrowsMemoryFault(sub_000110E6, ctx) && last_fault.instruction == 0x110E8 &&
              last_fault.address == 0x24000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              !last_fault.write && ctx.eax == 0 && ctx.ecx == 0xAABBCCDD && ctx.cf == 0 && ctx.of == 0 &&
              ctx.zf == 1 && ctx.sf == 0 && ctx.pf == 1 && ctx.af == 1,
          "AND memory fault retains prior instruction results and flags in context");

    ctx = X86Context{};
    ctx.eax = 0xF0;
    ctx.ebx = 0x0F;
    ctx.esp = 0x24004;
    ctx.esi = 0xCAFEBABE;
    SeedFlags(ctx);
    Check(ThrowsMemoryFault(sub_000110EB, ctx) && last_fault.instruction == 0x110ED &&
              last_fault.address == 0x24000 && last_fault.status == mem::AccessStatus::kUncommitted &&
              last_fault.write && ctx.eax == 0 && ctx.esp == 0x24004 && ctx.cf == 0 && ctx.zf == 1 &&
              ctx.sf == 0 && ctx.of == 0 && ctx.pf == 1 && ctx.af == 1,
          "PUSH fault preserves stack pointer and preceding AND flags");

    ctx = X86Context{};
    ctx.eax = 0x12345678;
    ctx.esi = UINT32_MAX;
    SeedFlags(ctx);
    const X86Context before_and_read_fault = ctx;
    Check(ThrowsMemoryFault(sub_000110D8, ctx) && last_fault.instruction == 0x110D8 &&
              last_fault.address == UINT32_MAX && last_fault.status == mem::AccessStatus::kInvalidAddress &&
              !last_fault.write && SameCoreState(ctx, before_and_read_fault),
          "AND invalid source span preserves guest state");
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
    std::string dispatch_error;
    Check(dispatch::Init(dispatch_error), "generated function map initializes production dispatch table");

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

    CheckCmpTests(destination);
    CheckPushLeaAndTests();
    CheckCallTests(destination);
    CheckEntryChainTests();

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
