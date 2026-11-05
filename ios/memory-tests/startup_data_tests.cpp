#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kernel/startup_data.h"

namespace {

namespace fs = std::filesystem;
using kernel::startup_data::DisplayState;
using kernel::startup_data::Operations;
using kernel::startup_data::ReadResult;
using kernel::startup_data::Result;

int failures = 0;
uint32_t allocated = 0;
int releases = 0;
int writes = 0;
int fail_write_at = 0;
bool fail_allocation = false;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

class TestDirectory {
public:
    TestDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 32; ++i) {
            path_ = fs::temp_directory_path() / ("mostwanted-startup-import-" + std::to_string(stamp) + "-" +
                                                  std::to_string(i));
            std::error_code error;
            if (fs::create_directory(path_, error)) return;
        }
        path_.clear();
    }
    ~TestDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

uint32_t SeedAllocate(uint32_t) {
    if (fail_allocation) return 0;
    allocated = mem::AllocContiguous(2 * mem::kPage, 0, UINT32_MAX, mem::kPage);
    if (!allocated) return 0;
    std::vector<uint8_t> seed(2 * mem::kPage, 0xA5);
    if (mem::Write(allocated, seed.data(), seed.size()) != mem::AccessStatus::kOk) {
        mem::FreeContiguous(allocated);
        allocated = 0;
    }
    return allocated;
}

void CountRelease(uint32_t address) {
    ++releases;
    mem::FreeContiguous(address);
}

mem::AccessStatus InjectWrite(uint32_t address, const void* source, size_t size) {
    if (++writes == fail_write_at) return mem::AccessStatus::kHostError;
    return mem::Write(address, source, size);
}

ReadResult InjectReadFailure(const fs::path&, size_t, std::vector<uint8_t>&, std::string& error) {
    error = "injected read failure";
    return ReadResult::kFailed;
}

Operations TestOperations() {
    return {SeedAllocate, CountRelease, InjectWrite, nullptr};
}

Operations FailReadOperations() {
    Operations operations;
    operations.read_file = InjectReadFailure;
    return operations;
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(file);
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (i * 8));
}

std::vector<uint8_t> LaunchFile() {
    std::vector<uint8_t> bytes(0x1000);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 17 + 3);
    return bytes;
}

std::vector<uint8_t> DisplayFile(uint32_t pixel_bytes = 4, std::string picture = std::string("pic\0", 4), uint32_t marked = 1) {
    std::vector<uint8_t> bytes(28 + pixel_bytes + picture.size());
    Put32(bytes, 0, 0x50535044);
    Put32(bytes, 4, 0xABCD0002);
    Put32(bytes, 8, 0x11223344);
    Put32(bytes, 12, 0x55667788);
    Put32(bytes, 16, pixel_bytes);
    Put32(bytes, 20, marked);
    Put32(bytes, 24, uint32_t(picture.size()));
    for (uint32_t i = 0; i < pixel_bytes; ++i) bytes[28 + i] = uint8_t(0x20 + i);
    std::copy(picture.begin(), picture.end(), bytes.begin() + 28 + pixel_bytes);
    return bytes;
}

bool SourceExists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

bool CheckSeededTail(uint32_t address, size_t written) {
    std::vector<uint8_t> bytes(2 * mem::kPage);
    if (mem::Read(address, bytes.data(), bytes.size()) != mem::AccessStatus::kOk) return false;
    return std::all_of(bytes.begin() + written, bytes.end(), [](uint8_t value) { return value == 0xA5; });
}

bool SameState(const DisplayState& left, const DisplayState& right) {
    return left.saved_data == right.saved_data && left.allocation == right.allocation && left.marked == right.marked &&
           left.picture == right.picture;
}

void CheckLaunchImports(const fs::path& directory) {
    const fs::path launch = directory / "launch.bin";
    constexpr uint32_t pointer = 0x10000;
    const uint32_t previous = 0x87654321;
    Check(mem::Commit(pointer, mem::kPage), "launch pointer slot commits");
    Check(mem::Write(pointer, &previous, sizeof(previous)) == mem::AccessStatus::kOk, "launch pointer sentinel is set");

    auto bytes = LaunchFile();
    Check(WriteFile(launch, bytes), "launch fixture is written under dedicated test directory");
    writes = releases = fail_write_at = 0;
    std::string error;
    Result result = kernel::startup_data::ImportLaunchData(launch, pointer, error, TestOperations());
    uint32_t page = 0;
    std::array<uint8_t, 0x1000> imported{};
    Check(result == Result::kImported && mem::Read(pointer, &page, sizeof(page)) == mem::AccessStatus::kOk &&
              page == allocated && mem::Read(page, imported.data(), imported.size()) == mem::AccessStatus::kOk &&
              std::equal(imported.begin(), imported.end(), bytes.begin()) &&
              CheckSeededTail(page, 0x1000) && !SourceExists(launch),
          "launch import checks full page, publishes pointer last, and removes source after success");
    uint32_t previous_block = page;
    Check(kernel::startup_data::ImportLaunchData(launch, pointer, error, TestOperations()) == Result::kMissing &&
              mem::Read(pointer, &page, sizeof(page)) == mem::AccessStatus::kOk && page == previous_block,
          "repeated launch import sees missing source without allocating or changing published state");
    mem::FreeContiguous(previous_block);
    mem::Write(pointer, &previous, sizeof(previous));

    bytes.resize(0xFFF);
    Check(WriteFile(launch, bytes), "short launch fixture is written");
    Check(kernel::startup_data::ImportLaunchData(launch, pointer, error) == Result::kFailed && SourceExists(launch) &&
              mem::Read(pointer, &page, sizeof(page)) == mem::AccessStatus::kOk && page == previous,
          "short launch read fails without deleting source or changing pointer");

    bytes = LaunchFile();
    bytes.push_back(0xFF);
    Check(WriteFile(launch, bytes), "oversized launch fixture is written");
    Check(kernel::startup_data::ImportLaunchData(launch, pointer, error) == Result::kFailed && SourceExists(launch),
          "oversized launch data is rejected and preserved");

    bytes = LaunchFile();
    Check(WriteFile(launch, bytes), "valid launch fixture is restored for injected failures");
    Check(kernel::startup_data::ImportLaunchData(launch, pointer, error, FailReadOperations()) == Result::kFailed &&
              error == "injected read failure" && SourceExists(launch),
          "injected launch read failure preserves source and guest pointer");

    fail_allocation = true;
    writes = 0;
    Check(kernel::startup_data::ImportLaunchData(launch, pointer, error, TestOperations()) == Result::kFailed &&
              writes == 0 && SourceExists(launch) && mem::Read(pointer, &page, sizeof(page)) == mem::AccessStatus::kOk && page == previous,
          "injected launch allocation failure preserves source and pointer");
    fail_allocation = false;

    for (int fail_at : {1, 2}) {
        writes = releases = 0;
        fail_write_at = fail_at;
        Check(kernel::startup_data::ImportLaunchData(launch, pointer, error, TestOperations()) == Result::kFailed &&
                  releases == 1 && mem::AllocationSize(allocated) == 0 && SourceExists(launch) &&
                  mem::Read(pointer, &page, sizeof(page)) == mem::AccessStatus::kOk && page == previous,
              fail_at == 1 ? "injected launch page write rolls allocation back" :
                             "injected launch pointer write rolls allocation back and leaves pointer unchanged");
    }
    fail_write_at = 0;
    mem::Decommit(pointer, mem::kPage);
}

void CheckDisplayImports(const fs::path& directory) {
    const fs::path display = directory / "display.bin";
    DisplayState state{0x12345678, 0, false, "old picture"};
    const DisplayState original = state;
    std::string error;

    Check(kernel::startup_data::ImportPersistedDisplay(display, state, error) == Result::kMissing && SameState(state, original),
          "missing persisted display leaves global state unchanged");

    auto bytes = DisplayFile();
    Check(WriteFile(display, bytes), "display fixture is written under dedicated test directory");
    writes = releases = fail_write_at = 0;
    Check(kernel::startup_data::ImportPersistedDisplay(display, state, error, TestOperations()) == Result::kImported &&
              state.saved_data == allocated && state.allocation == allocated && state.marked && state.picture == std::string("pic\0", 4) &&
              !SourceExists(display),
          "persisted-display import publishes fully checked surface and removes source after success");
    const uint32_t first_block = state.allocation;
    std::array<uint32_t, 16> surface{};
    std::array<uint8_t, 4> pixels{};
    Check(mem::Read(first_block, surface.data(), sizeof(surface)) == mem::AccessStatus::kOk &&
              surface[0] == 0xABCD0001 && surface[1] == mem::PhysicalFromVirtual(first_block + 0x40) && surface[2] == 0 &&
              surface[3] == 0x11223344 && surface[4] == 0x55667788 &&
              mem::Read(first_block + 0x40, pixels.data(), pixels.size()) == mem::AccessStatus::kOk &&
              pixels == std::array<uint8_t, 4>{0x20, 0x21, 0x22, 0x23} && CheckSeededTail(first_block, 0x44),
          "display metadata and pixels match source while neighboring page contents remain intact");
    Check(kernel::startup_data::ImportPersistedDisplay(display, state, error) == Result::kMissing &&
              state.allocation == first_block && mem::AllocationSize(first_block) == 2 * mem::kPage,
          "repeated display import does not duplicate allocation or change state");

    auto invalid = DisplayFile();
    invalid.resize(27);
    Check(WriteFile(display, invalid) && kernel::startup_data::ImportPersistedDisplay(display, state, error) == Result::kFailed &&
              SameState(state, DisplayState{first_block, first_block, true, std::string("pic\0", 4)}) && SourceExists(display),
          "truncated display header fails with old global state and source preserved");

    const auto expect_invalid = [&](std::vector<uint8_t> bad, const char* message) {
        Check(WriteFile(display, bad) && kernel::startup_data::ImportPersistedDisplay(display, state, error) == Result::kFailed &&
                  state.allocation == first_block && state.saved_data == first_block && SourceExists(display),
              message);
    };
    invalid = DisplayFile(); Put32(invalid, 0, 0); expect_invalid(invalid, "bad display magic is rejected");
    invalid = DisplayFile(); Put32(invalid, 16, 0); expect_invalid(invalid, "zero display pixel size is rejected");
    invalid = DisplayFile(); Put32(invalid, 16, 0x800001); expect_invalid(invalid, "oversized display pixel metadata is rejected");
    invalid = DisplayFile(); Put32(invalid, 16, UINT32_MAX); expect_invalid(invalid, "overflowing display allocation size is rejected");
    invalid = DisplayFile(); Put32(invalid, 20, 2); expect_invalid(invalid, "malformed display marker is rejected");
    invalid = DisplayFile(); Put32(invalid, 24, 1041); invalid.resize(28 + 4 + 1041); expect_invalid(invalid, "oversized display picture metadata is rejected");
    invalid = DisplayFile(); invalid.push_back(0xEE); expect_invalid(invalid, "trailing display data is rejected");
    invalid = DisplayFile(); invalid.resize(28 + 2); expect_invalid(invalid, "short display pixel read is rejected");

    bytes = DisplayFile();
    Check(WriteFile(display, bytes) && kernel::startup_data::ImportPersistedDisplay(display, state, error, FailReadOperations()) ==
                                            Result::kFailed && SameState(state, DisplayState{first_block, first_block, true, std::string("pic\0", 4)}) &&
              SourceExists(display),
          "injected display read failure preserves state and source");

    fail_allocation = true;
    Check(kernel::startup_data::ImportPersistedDisplay(display, state, error, TestOperations()) == Result::kFailed &&
              state.allocation == first_block && SourceExists(display),
          "injected display allocation failure preserves state and source");
    fail_allocation = false;

    for (int fail_at : {1, 2}) {
        writes = releases = 0;
        fail_write_at = fail_at;
        Check(kernel::startup_data::ImportPersistedDisplay(display, state, error, TestOperations()) == Result::kFailed &&
                  releases == 1 && mem::AllocationSize(allocated) == 0 && state.allocation == first_block &&
                  state.saved_data == first_block && SourceExists(display),
              fail_at == 1 ? "injected display pixel write rolls back allocation" :
                             "injected display metadata write rolls back unpublished surface");
    }
    fail_write_at = 0;

    bytes = DisplayFile(8, std::string("new\0", 4), 0);
    Check(WriteFile(display, bytes) && kernel::startup_data::ImportPersistedDisplay(display, state, error, TestOperations()) ==
                                            Result::kImported && state.allocation == allocated && !state.marked &&
              state.picture == std::string("new\0", 4) && mem::AllocationSize(first_block) == 0,
          "replacement display import atomically replaces state and frees prior owned allocation");
    mem::FreeContiguous(state.allocation);
    state = {};
}

}  // namespace

extern "C" int RunStartupDataTests() {
    TestDirectory directory;
    Check(!directory.path().empty(), "dedicated temporary directory is created for import tests");
    if (directory.path().empty() || !mem::Init()) return 1;
    CheckLaunchImports(directory.path());
    CheckDisplayImports(directory.path());
    mem::Shutdown();
    return failures ? 1 : 0;
}
