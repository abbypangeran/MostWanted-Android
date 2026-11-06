#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "kernel/memory.h"

namespace kernel::startup_data {

inline constexpr uint32_t kPersistMagic = 0x50535044;  // "DPSP"

enum class Result : uint8_t { kMissing, kImported, kFailed };
enum class ReadResult : uint8_t { kOk, kMissing, kFailed };

struct DisplayState {
    uint32_t saved_data = 0;
    uint32_t allocation = 0;
    bool marked = false;
    std::string picture;
};

struct Operations {
    uint32_t (*allocate)(uint32_t) = nullptr;
    void (*release)(uint32_t) = nullptr;
    mem::AccessStatus (*write)(uint32_t, const void*, size_t) = nullptr;
    ReadResult (*read_file)(const std::filesystem::path&, size_t, std::vector<uint8_t>&, std::string&) = nullptr;
    mem::AccessStatus (*read_guest)(uint32_t, void*, size_t) = nullptr;
    std::vector<uint8_t> (*allocate_display_file)(size_t) = nullptr;
};

Result ImportLaunchData(const std::filesystem::path& path, uint32_t launch_data_pointer, std::string& error,
                        Operations operations = {});
Result ImportPersistedDisplay(const std::filesystem::path& path, DisplayState& state, std::string& error,
                              Operations operations = {});
bool ExportPersistedDisplay(const std::filesystem::path& path, uint32_t common, uint32_t format, uint32_t size,
                            uint32_t guest_pixels, uint32_t pixel_bytes, bool marked, const std::string& picture,
                            std::string& error, Operations operations = {});

}  // namespace kernel::startup_data
