#include "kernel/kernel_data_init.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace kernel {
namespace {

struct Change {
    uint32_t address;
    std::vector<uint8_t> value;
    std::vector<uint8_t> original;
};

void Add(Change& change, uint32_t address, const void* value, size_t size) {
    change.address = address;
    change.value.resize(size);
    std::memcpy(change.value.data(), value, size);
}

Change Ansi(uint32_t address, const char* text) {
    const size_t length = std::strlen(text);
    std::vector<uint8_t> bytes(8 + length + 1, 0);
    const uint16_t length16 = uint16_t(length), maximum16 = uint16_t(length + 1);
    const uint32_t buffer = address + 8;
    std::memcpy(bytes.data(), &length16, sizeof(length16));
    std::memcpy(bytes.data() + 2, &maximum16, sizeof(maximum16));
    std::memcpy(bytes.data() + 4, &buffer, sizeof(buffer));
    std::memcpy(bytes.data() + 8, text, length + 1);
    return {address, std::move(bytes), {}};
}

}  // namespace

bool InitializeKernelData(const KernelDataLayout& layout, std::string& error, KernelDataMemory memory) {
    error.clear();
    const std::array<uint32_t, 11> addresses = {
        layout.launch_data_page, layout.hardware_info, layout.kernel_version, layout.image_file_name,
        layout.disk_model_number, layout.disk_serial_number, layout.disk_cache_partition_count,
        layout.boot_smc_video_mode, layout.ide_channel_object, layout.debugger_not_present, layout.time_increment};
    if (std::any_of(addresses.begin(), addresses.end(), [](uint32_t address) { return address == 0; }) ||
        layout.ide_channel_object > UINT32_MAX - 0x30) {
        error = "kernel-data address is invalid";
        return false;
    }

    std::vector<Change> changes;
    changes.reserve(11);
    const uint32_t zero = 0, cache_count = 3, video_mode = 0, increment = 10000;
    const uint8_t debugger_not_present = 1;
    const std::array<uint8_t, 6> hardware = {0x20, 0, 0, 0, 0xD3, 0xB1};
    std::array<uint16_t, 4> version = {1, 0, 5838, 1};
    const uint32_t queue_head = layout.ide_channel_object + 0x28;
    std::array<uint32_t, 2> queue_links = {queue_head, queue_head};
    Change change{};
    Add(change, layout.launch_data_page, &zero, sizeof(zero)); changes.push_back(std::move(change));
    Add(change, layout.hardware_info, hardware.data(), hardware.size()); changes.push_back(std::move(change));
    Add(change, layout.kernel_version, version.data(), sizeof(version)); changes.push_back(std::move(change));
    changes.push_back(Ansi(layout.image_file_name, "\\Device\\CdRom0\\default.xbe"));
    changes.push_back(Ansi(layout.disk_model_number, "RECOMP HARD DISK"));
    changes.push_back(Ansi(layout.disk_serial_number, "0000000000000000"));
    Add(change, layout.disk_cache_partition_count, &cache_count, sizeof(cache_count)); changes.push_back(std::move(change));
    Add(change, layout.boot_smc_video_mode, &video_mode, sizeof(video_mode)); changes.push_back(std::move(change));
    Add(change, queue_head, queue_links.data(), sizeof(queue_links)); changes.push_back(std::move(change));
    Add(change, layout.debugger_not_present, &debugger_not_present, sizeof(debugger_not_present)); changes.push_back(std::move(change));
    Add(change, layout.time_increment, &increment, sizeof(increment)); changes.push_back(std::move(change));

    auto read = memory.read ? memory.read : &mem::Read;
    auto write = memory.write ? memory.write : &mem::Write;
    for (Change& item : changes) {
        const uint64_t end = uint64_t(item.address) + item.value.size();
        if (!item.address || end > uint64_t(UINT32_MAX) + 1) {
            error = "kernel-data write address overflows";
            return false;
        }
        item.original.resize(item.value.size());
        const mem::AccessStatus status = read(item.address, item.original.data(), item.original.size());
        if (status != mem::AccessStatus::kOk) {
            error = "kernel-data snapshot failed: " + std::string(mem::StatusName(status));
            return false;
        }
    }

    for (size_t i = 0; i < changes.size(); ++i) {
        Change& item = changes[i];
        const mem::AccessStatus status = write(item.address, item.value.data(), item.value.size());
        if (status == mem::AccessStatus::kOk) continue;
        error = "kernel-data write failed: " + std::string(mem::StatusName(status));
        for (size_t undo = i + 1; undo > 0; --undo) {
            Change& previous = changes[undo - 1];
            const mem::AccessStatus rollback = write(previous.address, previous.original.data(), previous.original.size());
            if (rollback != mem::AccessStatus::kOk)
                error += "; rollback failed: " + std::string(mem::StatusName(rollback));
        }
        return false;
    }
    return true;
}

}  // namespace kernel
