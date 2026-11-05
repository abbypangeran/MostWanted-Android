#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel/kernel.h"
#include "cpu/x86_ops.h"
#include "x86_recomp_shared.h"

namespace test {
constexpr uint32_t kExportCount = 400;
constexpr uint32_t kGuestStack = 0x70000;
std::array<kernel::KernelFn, kExportCount> exports{};

uint32_t CallExport(uint32_t ordinal, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (ordinal >= exports.size() || !exports[ordinal] || !mem::Commit(kGuestStack, mem::kPage)) _exit(5);
    const std::array<uint32_t, 4> stack{0, a0, a1, a2};
    if (mem::Write(kGuestStack, stack.data(), sizeof(stack)) != mem::AccessStatus::kOk) _exit(6);
    X86Context ctx{};
    ctx.esp = kGuestStack;
    exports[ordinal](ctx, nullptr);
    return ctx.eax;
}
}  // namespace test

namespace kernel {
uint8_t* g_base = nullptr;
void RegisterExport(uint32_t ordinal, const char*, KernelFn fn) {
    if (ordinal < test::exports.size()) test::exports[ordinal] = fn;
}
}  // namespace kernel

namespace {

enum class Scenario {
    kReadFault,
    kCmpReadFault,
    kWriteFault,
    kStringDescriptorOverflow,
    kUlongFillOverflow,
    kCompareCursorOverflow,
    kUnsupportedInstruction
};

bool AbortsWithDiagnostic(Scenario scenario, const char* expected) {
    int output[2];
    if (pipe(output)) return false;
    pid_t child = fork();
    if (child < 0) return false;
    if (!child) {
        close(output[0]);
        dup2(output[1], STDERR_FILENO);
        close(output[1]);
        rlimit no_core{0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        X86Context ctx{};
        if (scenario == Scenario::kUnsupportedInstruction) {
            sub_0001102D(ctx, nullptr);
        } else if (!mem::Init()) {
            _exit(3);
        } else if (scenario == Scenario::kReadFault) {
            ctx.ebx = 0x24000;
            sub_00011000(ctx, nullptr);
        } else if (scenario == Scenario::kCmpReadFault) {
            ctx.eax = 0xDEADBEEF;
            ctx.esi = 0x24000;
            sub_00011068(ctx, nullptr);
        } else if (scenario == Scenario::kWriteFault) {
            if (!mem::Commit(0x20000, mem::kPage) || !mem::Commit(0x30000, mem::kPage) ||
                mem::Protect(0x30000, mem::kPage, mem::kNoAccess) != mem::AccessStatus::kOk)
                _exit(4);
            ctx.ebx = 0x20000;
            ctx.edi = 0x30000;
            sub_00011000(ctx, nullptr);
        } else if (scenario == Scenario::kStringDescriptorOverflow) {
            kernel::ReadAnsiString(UINT32_MAX - 3);
        } else if (scenario == Scenario::kUlongFillOverflow) {
            test::CallExport(285, UINT32_MAX - 1, 8, 0xAABBCCDD);
        } else if (scenario == Scenario::kCompareCursorOverflow) {
            constexpr uint32_t last_page = UINT32_MAX & ~(mem::kPage - 1);
            const uint8_t equal = 0x5A;
            if (!mem::Commit(last_page, mem::kPage) || !mem::Commit(0x20000, mem::kPage) ||
                mem::Write(UINT32_MAX, &equal, 1) != mem::AccessStatus::kOk ||
                mem::Write(0x20000, &equal, 1) != mem::AccessStatus::kOk)
                _exit(7);
            test::CallExport(268, UINT32_MAX, 0x20000, 2);
        } else {
            sub_0001102D(ctx, nullptr);
        }
        _exit(2);
    }
    close(output[1]);
    std::array<char, 512> bytes{};
    std::string diagnostic;
    ssize_t count;
    while ((count = read(output[0], bytes.data(), bytes.size())) > 0) diagnostic.append(bytes.data(), size_t(count));
    close(output[0]);
    int status = 0;
    if (waitpid(child, &status, 0) != child) return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && diagnostic.find(expected) != std::string::npos;
}

bool RtlSuccessPaths() {
    if (!mem::Init() || !mem::Commit(0x40000, mem::kPage)) return false;
    constexpr uint32_t pattern = 0xAABBCCDD;
    test::CallExport(285, 0x40000, 8, pattern);
    std::array<uint32_t, 2> filled{};
    if (mem::Read(0x40000, filled.data(), sizeof(filled)) != mem::AccessStatus::kOk ||
        filled != std::array<uint32_t, 2>{pattern, pattern})
        return false;

    const std::array<uint8_t, 3> left{1, 2, 3}, right{1, 2, 4};
    if (mem::Write(0x40010, left.data(), left.size()) != mem::AccessStatus::kOk ||
        mem::Write(0x40020, right.data(), right.size()) != mem::AccessStatus::kOk)
        return false;
    return test::CallExport(268, 0x40010, 0x40020, 3) == 2;
}

}  // namespace

int main() {
    bool read_fault = AbortsWithDiagnostic(Scenario::kReadFault,
                                           "checked guest read fault at 00011000 for 00024000: uncommitted");
    bool cmp_read_fault = AbortsWithDiagnostic(Scenario::kCmpReadFault,
                                               "checked guest read fault at 00011068 for 00024000: uncommitted");
    bool write_fault = AbortsWithDiagnostic(Scenario::kWriteFault,
                                            "checked guest write fault at 00011002 for 00030000: permission-denied");
    bool descriptor_overflow = AbortsWithDiagnostic(Scenario::kStringDescriptorOverflow,
                                                    "kernel guest read at FFFFFFFC failed: invalid-address");
    bool fill_overflow = AbortsWithDiagnostic(Scenario::kUlongFillOverflow,
                                              "kernel guest write at FFFFFFFE failed: invalid-address");
    bool compare_overflow = AbortsWithDiagnostic(Scenario::kCompareCursorOverflow,
                                                 "kernel guest read at FFFFFFFF failed: invalid-address");
    bool blocked_instruction = AbortsWithDiagnostic(Scenario::kUnsupportedInstruction,
                                                    "checked recompiler blocked instruction at 0001102D");
    bool rtl_success = RtlSuccessPaths();
    std::printf("%s: generated uncommitted read reaches production logger and abort handler\n", read_fault ? "PASS" : "FAIL");
    std::printf("%s: generated CMP read fault reports its PC and guest address before aborting\n",
                cmp_read_fault ? "PASS" : "FAIL");
    std::printf("%s: generated denied write logs direction, PC, status, and aborts\n", write_fault ? "PASS" : "FAIL");
    std::printf("%s: counted string descriptor overflow is rejected without address wrap\n",
                descriptor_overflow ? "PASS" : "FAIL");
    std::printf("%s: ULONG fill validates its span and avoids wrapping iteration\n", fill_overflow ? "PASS" : "FAIL");
    std::printf("%s: memory comparison stops safely before its cursor wraps\n", compare_overflow ? "PASS" : "FAIL");
    std::printf("%s: generated blocked instruction reaches production handler without stack scan\n",
                blocked_instruction ? "PASS" : "FAIL");
    std::printf("%s: checked RTL fill and compare success paths preserve guest data\n", rtl_success ? "PASS" : "FAIL");
    return read_fault && cmp_read_fault && write_fault && descriptor_overflow && fill_overflow && compare_overflow &&
                   blocked_instruction && rtl_success
               ? 0
               : 1;
}
