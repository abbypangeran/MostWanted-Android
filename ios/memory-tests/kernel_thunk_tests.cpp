#include "kernel/kernel_thunks.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int write_calls = 0;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

bool Resolve(uint32_t ordinal, uint32_t& address, bool& implemented, void*) {
    if (ordinal >= 400) return false;
    address = ordinal == 2 ? 0x81234000 : 0xFE000000 + ordinal * 4;
    implemented = ordinal != 3;
    return true;
}

mem::AccessStatus FailSecondWrite(uint32_t address, const void* source, std::size_t size) {
    if (++write_calls == 2) {
        mem::Write(address, source, size);
        return mem::AccessStatus::kHostError;
    }
    return mem::Write(address, source, size);
}

xbe::Image SyntheticImage() {
    xbe::Image image;
    image.base = 0x00100000;
    image.header_size = 0x110;
    image.kernel_thunk = 0x00100100;
    image.file.resize(image.header_size);
    return image;
}

bool WriteEntries(const std::array<uint32_t, 4>& entries) {
    return mem::Write(0x00100100, entries.data(), sizeof(entries)) == mem::AccessStatus::kOk;
}

}  // namespace

extern "C" int RunKernelThunkTests() {
    const xbe::Image image = SyntheticImage();
    const std::array<uint32_t, 4> original = {0x80000002, 0x80000003, 0, 0};
    Check(mem::Init() && mem::Commit(0x00100000, mem::kPage), "synthetic kernel thunk page commits");

    std::string error;
    std::vector<uint32_t> missing;
    Check(WriteEntries(original) && kernel::PatchKernelThunks(image, Resolve, nullptr, missing, error),
          "kernel thunk table resolves synthetic ordinals");
    std::array<uint32_t, 4> patched{};
    Check(mem::Read(0x00100100, patched.data(), sizeof(patched)) == mem::AccessStatus::kOk &&
              patched == std::array<uint32_t, 4>{0x81234000, 0xFE00000C, 0, 0} && missing == std::vector<uint32_t>{3},
          "kernel data and function ordinals patch to bounded targets and report missing exports");

    const std::array<uint32_t, 4> invalid_ordinal = {0x80000190, 0, 0, 0};
    WriteEntries(invalid_ordinal);
    Check(!kernel::PatchKernelThunks(image, Resolve, nullptr, missing, error) && !error.empty() &&
              mem::Read(0x00100100, patched.data(), sizeof(patched)) == mem::AccessStatus::kOk && patched == invalid_ordinal,
          "out-of-range kernel ordinal rejects the entire table without writes");

    const std::array<uint32_t, 4> unterminated = {0x80000002, 0x80000002, 0x80000002, 0x80000002};
    WriteEntries(unterminated);
    Check(!kernel::PatchKernelThunks(image, Resolve, nullptr, missing, error) && !error.empty() &&
              mem::Read(0x00100100, patched.data(), sizeof(patched)) == mem::AccessStatus::kOk && patched == unterminated,
          "unterminated bounded table is rejected before any writes");

    WriteEntries(original);
    write_calls = 0;
    const kernel::ThunkMemory injected{nullptr, FailSecondWrite};
    Check(!kernel::PatchKernelThunks(image, Resolve, nullptr, missing, error, injected) && !error.empty() &&
              mem::Read(0x00100100, patched.data(), sizeof(patched)) == mem::AccessStatus::kOk && patched == original,
          "failed thunk write rolls back earlier slots");

    auto outside = image;
    outside.kernel_thunk = 0x00200000;
    Check(!kernel::PatchKernelThunks(outside, Resolve, nullptr, missing, error) && !error.empty(),
          "thunk table outside file-backed image is rejected");

    auto unaligned = image;
    unaligned.kernel_thunk++;
    Check(!kernel::PatchKernelThunks(unaligned, Resolve, nullptr, missing, error) && !error.empty(),
          "unaligned thunk table address is rejected");

    mem::Shutdown();
    return failures ? 1 : 0;
}
