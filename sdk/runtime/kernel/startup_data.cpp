#include "kernel/startup_data.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace kernel::startup_data {
namespace {

constexpr size_t kLaunchBytes = 0x1000;
constexpr size_t kDisplayHeaderBytes = 7 * sizeof(uint32_t);

ReadResult ReadFile(const std::filesystem::path& path, size_t limit, std::vector<uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) return ReadResult::kMissing;
        error = "cannot inspect " + path.string() + ": " + ec.message();
        return ReadResult::kFailed;
    }
    if (size > limit || size > size_t(-1)) {
        error = "file exceeds import bound: " + path.string();
        return ReadResult::kFailed;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return ReadResult::kFailed;
    }
    bytes.resize(size_t(size));
    if (size && !file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) {
        error = "short read from " + path.string();
        return ReadResult::kFailed;
    }
    if (file.bad() || file.peek() != std::char_traits<char>::eof() || file.bad()) {
        error = "file changed during import: " + path.string();
        return ReadResult::kFailed;
    }
    return ReadResult::kOk;
}

ReadResult Read(const std::filesystem::path& path, size_t limit, std::vector<uint8_t>& bytes, std::string& error,
                Operations operations) {
    auto read = operations.read_file ? operations.read_file : &ReadFile;
    return read(path, limit, bytes, error);
}

uint32_t Allocate(uint32_t bytes, Operations operations) {
    return operations.allocate ? operations.allocate(bytes) : mem::AllocContiguous(bytes, 0, UINT32_MAX, mem::kPage);
}

void Release(uint32_t address, Operations operations) {
    auto release = operations.release ? operations.release : &mem::FreeContiguous;
    release(address);
}

mem::AccessStatus Write(uint32_t address, const void* bytes, size_t size, Operations operations) {
    auto write = operations.write ? operations.write : &mem::Write;
    return write(address, bytes, size);
}

bool RemoveSource(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        error = "import succeeded but source could not be removed: " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool AllocationIsLargeEnough(uint32_t address, uint32_t requested) {
    const uint32_t allocation = mem::AllocationSize(address);
    return address && allocation >= requested;
}

}  // namespace

Result ImportLaunchData(const std::filesystem::path& path, uint32_t launch_data_pointer, std::string& error,
                        Operations operations) {
    error.clear();
    std::vector<uint8_t> bytes;
    const ReadResult read = Read(path, kLaunchBytes, bytes, error, operations);
    if (read == ReadResult::kMissing) return Result::kMissing;
    if (read == ReadResult::kFailed) return Result::kFailed;
    if (bytes.size() != kLaunchBytes) {
        error = "launch data must be exactly 4096 bytes";
        return Result::kFailed;
    }

    const uint32_t page = Allocate(uint32_t(kLaunchBytes), operations);
    if (!AllocationIsLargeEnough(page, uint32_t(kLaunchBytes))) {
        if (page) Release(page, operations);
        error = "could not allocate launch-data page";
        return Result::kFailed;
    }
    auto fail = [&](const char* operation, mem::AccessStatus status) {
        error = std::string(operation) + " failed: " + mem::StatusName(status);
        Release(page, operations);
        return Result::kFailed;
    };

    mem::AccessStatus status = Write(page, bytes.data(), bytes.size(), operations);
    if (status != mem::AccessStatus::kOk) return fail("launch-data write", status);
    status = Write(launch_data_pointer, &page, sizeof(page), operations);
    if (status != mem::AccessStatus::kOk) return fail("launch-data pointer write", status);
    RemoveSource(path, error);
    return Result::kImported;
}

Result ImportPersistedDisplay(const std::filesystem::path& path, DisplayState& state, std::string& error,
                              Operations operations) {
    error.clear();
    constexpr size_t kMaxPixels = 0x800000;
    constexpr size_t kMaxPicture = 1040;
    constexpr size_t kMaxFile = kDisplayHeaderBytes + kMaxPixels + kMaxPicture;
    std::vector<uint8_t> bytes;
    const ReadResult read = Read(path, kMaxFile, bytes, error, operations);
    if (read == ReadResult::kMissing) return Result::kMissing;
    if (read == ReadResult::kFailed) return Result::kFailed;
    if (bytes.size() < kDisplayHeaderBytes) {
        error = "persisted-display header is truncated";
        return Result::kFailed;
    }

    std::array<uint32_t, 7> header{};
    std::memcpy(header.data(), bytes.data(), kDisplayHeaderBytes);
    const uint32_t pixel_bytes = header[4], picture_bytes = header[6];
    const uint64_t allocation_bytes = uint64_t(0x40) + pixel_bytes;
    const uint64_t file_bytes = uint64_t(kDisplayHeaderBytes) + pixel_bytes + picture_bytes;
    if (header[0] != kPersistMagic || !pixel_bytes || pixel_bytes > kMaxPixels || picture_bytes > kMaxPicture ||
        header[5] > 1 || allocation_bytes > UINT32_MAX || file_bytes != bytes.size()) {
        error = "persisted-display metadata is invalid";
        return Result::kFailed;
    }

    std::string picture(reinterpret_cast<const char*>(bytes.data() + kDisplayHeaderBytes + pixel_bytes), picture_bytes);
    const uint32_t block = Allocate(uint32_t(allocation_bytes), operations);
    if (!AllocationIsLargeEnough(block, uint32_t(allocation_bytes))) {
        if (block) Release(block, operations);
        error = "could not allocate persisted-display surface";
        return Result::kFailed;
    }
    const uint64_t pixel_address = uint64_t(block) + 0x40;
    if (pixel_address > UINT32_MAX) {
        Release(block, operations);
        error = "persisted-display guest address overflow";
        return Result::kFailed;
    }
    auto fail = [&](const char* operation, mem::AccessStatus status) {
        error = std::string(operation) + " failed: " + mem::StatusName(status);
        Release(block, operations);
        return Result::kFailed;
    };

    mem::AccessStatus status = Write(uint32_t(pixel_address), bytes.data() + kDisplayHeaderBytes, pixel_bytes, operations);
    if (status != mem::AccessStatus::kOk) return fail("persisted-display pixel write", status);
    std::array<uint32_t, 16> surface{};
    surface[0] = (header[1] & 0xFFFF0000) | 1;
    surface[1] = mem::PhysicalFromVirtual(uint32_t(pixel_address));
    surface[2] = 0;
    surface[3] = header[2];
    surface[4] = header[3];
    status = Write(block, surface.data(), sizeof(surface), operations);
    if (status != mem::AccessStatus::kOk) return fail("persisted-display metadata write", status);

    const uint32_t previous_allocation = state.allocation;
    state.saved_data = block;
    state.allocation = block;
    state.marked = header[5] != 0;
    state.picture.swap(picture);
    if (previous_allocation && previous_allocation != block) Release(previous_allocation, operations);
    RemoveSource(path, error);
    return Result::kImported;
}

}  // namespace kernel::startup_data
