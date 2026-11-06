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
#include "cpu/dispatch_table.h"
#include "cpu/x86_ops.h"
#include "x86_recomp_shared.h"

namespace test {
constexpr uint32_t kExportCount = 400;
constexpr uint32_t kGuestStack = 0x70000;

uint32_t CallExport(uint32_t ordinal, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (ordinal >= kExportCount || !kernel::HasExport(ordinal) || !mem::Commit(kGuestStack, mem::kPage)) _exit(5);
    const std::array<uint32_t, 4> stack{0, a0, a1, a2};
    if (mem::Write(kGuestStack, stack.data(), sizeof(stack)) != mem::AccessStatus::kOk) _exit(6);
    X86Context ctx{};
    ctx.esp = kGuestStack;
    x86_kernel(ctx, nullptr, ordinal);
    return ctx.eax;
}
}  // namespace test

namespace kernel {
uint8_t* g_base = nullptr;
}  // namespace kernel

namespace {

enum class Scenario {
    kReadFault,
    kCmpReadFault,
    kPushWriteFault,
    kAndReadFault,
    kAndWriteFault,
    kWriteFault,
    kCallStackWriteFault,
    kRetStackReadFault,
    kCallMissingTarget,
    kInvalidContinuation,
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
        } else if (scenario == Scenario::kCallStackWriteFault) {
            std::string error;
            if (!dispatch::Init(error)) _exit(8);
            ctx.esp = 0x24000;
            sub_000110F5(ctx, nullptr);
        } else if (scenario == Scenario::kRetStackReadFault) {
            std::string error;
            if (!dispatch::Init(error) || !mem::Commit(0x70000, mem::kPage)) _exit(8);
            ctx.esp = 0x71000;
            ctx.ebx = 0x24000;
            sub_0001114E(ctx, nullptr);
        } else if (scenario == Scenario::kCallMissingTarget) {
            std::string error;
            if (!dispatch::Init(error)) _exit(8);
            ctx.esp = 0x70000;
            sub_0001113F(ctx, nullptr);
        } else if (scenario == Scenario::kInvalidContinuation) {
            std::string error;
            if (!dispatch::Init(error)) _exit(8);
            ctx.esp = 0x24000;
            x86_resume(ctx, nullptr, X86Resume{0x11136, ctx.esp});
        } else if (scenario == Scenario::kReadFault) {
            ctx.ebx = 0x24000;
            sub_00011000(ctx, nullptr);
        } else if (scenario == Scenario::kCmpReadFault) {
            ctx.eax = 0xDEADBEEF;
            ctx.esi = 0x24000;
            sub_00011068(ctx, nullptr);
        } else if (scenario == Scenario::kPushWriteFault) {
            ctx.esp = 0x24004;
            ctx.esi = 0xDEADBEEF;
            sub_000110AB(ctx, nullptr);
        } else if (scenario == Scenario::kAndReadFault) {
            ctx.eax = 0xDEADBEEF;
            ctx.esi = 0x24000;
            sub_000110D8(ctx, nullptr);
        } else if (scenario == Scenario::kAndWriteFault) {
            if (!mem::Commit(0x30000, mem::kPage) ||
                mem::Protect(0x30000, mem::kPage, mem::kRead) != mem::AccessStatus::kOk)
                _exit(4);
            ctx.eax = 0xFFFFFFFF;
            ctx.esi = 0x30000;
            sub_000110D5(ctx, nullptr);
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
    bool push_write_fault = AbortsWithDiagnostic(Scenario::kPushWriteFault,
                                                 "checked guest write fault at 000110AB for 00024000: uncommitted");
    bool and_read_fault = AbortsWithDiagnostic(Scenario::kAndReadFault,
                                               "checked guest read fault at 000110D8 for 00024000: uncommitted");
    bool and_write_fault = AbortsWithDiagnostic(Scenario::kAndWriteFault,
                                                "checked guest write fault at 000110D5 for 00030000: permission-denied");
    bool write_fault = AbortsWithDiagnostic(Scenario::kWriteFault,
                                            "checked guest write fault at 00011002 for 00030000: permission-denied");
    bool call_stack_write_fault = AbortsWithDiagnostic(
        Scenario::kCallStackWriteFault,
        "checked guest write fault at 000110F5 for 00023FFC: uncommitted");
    bool ret_stack_read_fault = AbortsWithDiagnostic(
        Scenario::kRetStackReadFault,
        "checked guest read fault at 0001114D for 00024000: uncommitted");
    bool call_missing_target = AbortsWithDiagnostic(
        Scenario::kCallMissingTarget,
        "checked recompiler blocked instruction at 0001113F: unimplemented: call 0x00011200");
    bool invalid_continuation = AbortsWithDiagnostic(Scenario::kInvalidContinuation,
                                                     "guest trap at 00011136: stale or invalid continuation");
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
    std::printf("%s: generated PUSH write fault reports its PC and guest address before aborting\n",
                push_write_fault ? "PASS" : "FAIL");
    std::printf("%s: generated AND read fault reports its PC and guest address before aborting\n",
                and_read_fault ? "PASS" : "FAIL");
    std::printf("%s: generated AND write fault reports direction, PC, and permission status before aborting\n",
                and_write_fault ? "PASS" : "FAIL");
    std::printf("%s: generated denied write logs direction, PC, status, and aborts\n", write_fault ? "PASS" : "FAIL");
    std::printf("%s: generated CALL stack fault reports CALL PC, stack address, and aborts\n",
                call_stack_write_fault ? "PASS" : "FAIL");
    std::printf("%s: generated RET stack fault reports RET PC, stack address, and aborts\n",
                ret_stack_read_fault ? "PASS" : "FAIL");
    std::printf("%s: production checked CALL blocks a target missing from generated metadata\n",
                call_missing_target ? "PASS" : "FAIL");
    std::printf("%s: production resume dispatch rejects stale continuation targets\n",
                invalid_continuation ? "PASS" : "FAIL");
    std::printf("%s: counted string descriptor overflow is rejected without address wrap\n",
                descriptor_overflow ? "PASS" : "FAIL");
    std::printf("%s: ULONG fill validates its span and avoids wrapping iteration\n", fill_overflow ? "PASS" : "FAIL");
    std::printf("%s: memory comparison stops safely before its cursor wraps\n", compare_overflow ? "PASS" : "FAIL");
    std::printf("%s: generated blocked instruction reaches production handler without stack scan\n",
                blocked_instruction ? "PASS" : "FAIL");
    std::printf("%s: checked RTL fill and compare success paths preserve guest data\n", rtl_success ? "PASS" : "FAIL");
    return read_fault && cmp_read_fault && push_write_fault && and_read_fault && and_write_fault && write_fault &&
                   call_stack_write_fault && ret_stack_read_fault && call_missing_target && invalid_continuation && descriptor_overflow && fill_overflow &&
                   compare_overflow && blocked_instruction && rtl_success
               ? 0
               : 1;
}
