#include "cpu/dispatch_table.h"

#include <array>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

void First(X86Context&, uint8_t*) {}
void Second(X86Context&, uint8_t*) {}

}  // namespace

extern "C" int RunDispatchTableTests() {
    dispatch::Table table;
    std::string error;
    const std::array<X86FuncMapping, 2> functions = {{{0x1000, First}, {0x1008, Second}}};
    const std::array<uint32_t, 2> continuations = {0x1004, 0x100C};
    const dispatch::Metadata valid{0x1000, 0x1010, 0x1000, functions, continuations};
    Check(table.Initialize(valid, error) && table.Find(0x1000) == First && table.Find(0x1008) == Second &&
              table.Find(0x1004) == nullptr && table.IsContinuation(0x1004) && table.CodeBase() == 0x1000 &&
              table.CodeEnd() == 0x1010,
          "synthetic dispatch metadata builds bounded function and continuation lookups");

    const std::array<X86FuncMapping, 1> replacement = {{{0x2000, Second}}};
    const dispatch::Metadata next{0x2000, 0x2010, 0x2000, replacement, {}};
    Check(table.Initialize(next, error) && table.Find(0x1000) == nullptr && table.Find(0x2000) == Second &&
              !table.IsContinuation(0x1004),
          "dispatch reinitialization replaces prior mappings and continuations");

    const std::array<X86FuncMapping, 2> duplicate = {{{0x2000, First}, {0x2000, Second}}};
    Check(!table.Initialize({0x2000, 0x2010, 0x2000, duplicate, {}}, error) && table.Find(0x2000) == Second,
          "duplicate mappings fail without corrupting the published table");
    Check(!table.Initialize({0x2000, 0x2010, 0x2004, replacement, {}}, error) && table.Find(0x2000) == Second,
          "missing entry mapping fails without replacing valid state");
    const std::array<X86FuncMapping, 1> out_of_range = {{{0x2010, First}}};
    Check(!table.Initialize({0x2000, 0x2010, 0x2000, out_of_range, {}}, error),
          "out-of-range function address is rejected");
    const std::array<uint32_t, 1> bad_continuation = {0x2010};
    Check(!table.Initialize({0x2000, 0x2010, 0x2000, replacement, bad_continuation}, error),
          "out-of-range continuation address is rejected");
    const std::array<uint32_t, 2> duplicate_continuations = {0x2004, 0x2004};
    Check(!table.Initialize({0x2000, 0x2010, 0x2000, replacement, duplicate_continuations}, error),
          "duplicate continuation address is rejected");
    Check(!table.Initialize({0x2010, 0x2000, 0x2010, replacement, {}}, error),
          "reversed dispatch address bounds are rejected");
    Check(!table.Initialize({0x2000, 0x2010, 0x2010, replacement, {}}, error),
          "entry outside dispatch address range is rejected");
    const std::array<X86FuncMapping, 1> huge_mapping = {{{0x1000, First}}};
    Check(!table.Initialize({0x1000, 0x10000000, 0x1000, huge_mapping, {}}, error),
          "untrusted dispatch code span is capped before allocation");

    table.Reset();
    Check(table.Find(0x2000) == nullptr && !table.IsContinuation(0x2004) && table.CodeBase() == 0 && table.CodeEnd() == 0,
          "dispatch reset clears mappings and bounds");
    return failures ? 1 : 0;
}
