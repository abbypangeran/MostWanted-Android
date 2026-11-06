#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "kernel/memory.h"

namespace kernel {

struct KernelDataLayout {
    uint32_t launch_data_page;
    uint32_t hardware_info;
    uint32_t kernel_version;
    uint32_t image_file_name;
    uint32_t disk_model_number;
    uint32_t disk_serial_number;
    uint32_t disk_cache_partition_count;
    uint32_t boot_smc_video_mode;
    uint32_t ide_channel_object;
    uint32_t debugger_not_present;
    uint32_t time_increment;
};

struct KernelDataMemory {
    mem::AccessStatus (*read)(uint32_t, void*, size_t) = nullptr;
    mem::AccessStatus (*write)(uint32_t, const void*, size_t) = nullptr;
};

bool InitializeKernelData(const KernelDataLayout& layout, std::string& error, KernelDataMemory memory = {});

}  // namespace kernel
