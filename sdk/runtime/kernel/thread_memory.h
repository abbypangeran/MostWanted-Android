#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "kernel/memory.h"

namespace kernel {

inline mem::AccessStatus InitializeThreadGuestMemory(uint32_t kpcr, uint32_t ethread, uint32_t stack_top,
                                                      uint32_t stack_size, uint32_t tls, uint32_t thread_id) {
    constexpr uint32_t kKpcrSize = 0x400;
    constexpr uint32_t kEthreadSize = 0x200;
    if (!kpcr || !ethread || !stack_top || !stack_size || stack_size > stack_top || tls > stack_top)
        return mem::AccessStatus::kInvalidAddress;

    std::array<uint8_t, kKpcrSize> kpcr_bytes{};
    std::array<uint8_t, kEthreadSize> ethread_bytes{};
    auto put32 = [](auto& bytes, size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    put32(kpcr_bytes, 0x00, 0xFFFFFFFF);
    put32(kpcr_bytes, 0x04, stack_top);
    put32(kpcr_bytes, 0x08, stack_top - stack_size);
    put32(kpcr_bytes, 0x18, kpcr);
    put32(kpcr_bytes, 0x1C, kpcr);
    put32(kpcr_bytes, 0x20, kpcr + 0x28);
    put32(kpcr_bytes, 0x28, ethread);
    ethread_bytes[0] = 6;
    ethread_bytes[2] = uint8_t(kEthreadSize / 4);
    put32(ethread_bytes, 0x1C, stack_top);
    put32(ethread_bytes, 0x20, stack_top - stack_size);
    put32(ethread_bytes, 0x28, tls);
    put32(ethread_bytes, 0x12C, thread_id);

    auto status = mem::Write(kpcr, kpcr_bytes.data(), kpcr_bytes.size());
    if (status != mem::AccessStatus::kOk) return status;
    return mem::Write(ethread, ethread_bytes.data(), ethread_bytes.size());
}

}  // namespace kernel
