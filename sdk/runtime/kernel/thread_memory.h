#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "kernel/memory.h"

namespace kernel {

// Guest thread blocks. The offsets follow the Xbox KPCR/KTHREAD layouts
// (Cxbx-Reloaded src/core/kernel/common/types.h, itself based on OpenXDK) and
// cover only the fields authentic game code reads or writes through FS.
constexpr uint32_t kKpcrSize = 0x400;
constexpr uint32_t kEthreadSize = 0x200;

namespace layout {

// NT_TIB is the first 0x1C bytes of the KPCR; FS:0x20 is the KPRCB pointer.
constexpr uint32_t kKpcrExceptionList = 0x00;  // 0xFFFFFFFF while no SEH record is registered
constexpr uint32_t kKpcrStackBase = 0x04;
constexpr uint32_t kKpcrStackLimit = 0x08;
constexpr uint32_t kKpcrSelf = 0x18;
constexpr uint32_t kKpcrSelfPcr = 0x1C;
constexpr uint32_t kKpcrPrcb = 0x20;
constexpr uint32_t kKpcrIrql = 0x24;
constexpr uint32_t kKpcrPrcbData = 0x28;

// KTHREAD starts the ETHREAD, and KPRCB.CurrentThread (KPCR+0x28) points at it.
constexpr uint32_t kPrcbCurrentThread = 0x00;
constexpr uint32_t kThreadHeaderType = 0x00;  // KOBJECTS::ThreadObject
constexpr uint32_t kThreadHeaderSize = 0x02;  // dwords
constexpr uint32_t kThreadStackBase = 0x1C;
constexpr uint32_t kThreadStackLimit = 0x20;
constexpr uint32_t kThreadKernelStack = 0x24;  // unresolved, see below
constexpr uint32_t kThreadTlsData = 0x28;
constexpr uint32_t kThreadUniqueThread = 0x12C;

// 002B38B2 reads [FS:0x20+0x250] and, when non-null, treats [eax+0x24] as the
// base of the dword pair it fills with 1 and an offset. 002B5BD9, 002B596D,
// 002B83F6, 0037927B and 0039CD5F read the same slot as a non-null object
// pointer whose +0x14 is callable and whose +0x20 is a writable block. That is
// not a KTHREAD (KTHREAD+0x14 is MutantListHead), and no writer in the image
// sets it, so its owner stays unresolved: the runtime leaves the slot zero and
// authentic code takes its null branch. KTHREAD+0x24 is left zero for the same
// reason (Cxbx names it KernelStack; 002B38BC uses it as a TLS descriptor).
constexpr uint32_t kPrcbUnresolved250 = 0x250;

}  // namespace layout

inline mem::AccessStatus InitializeThreadGuestMemory(uint32_t kpcr, uint32_t ethread, uint32_t stack_top,
                                                     uint32_t stack_size, uint32_t tls, uint32_t thread_id) {
    if (!kpcr || !ethread || !stack_top || !stack_size || stack_size > stack_top || tls > stack_top)
        return mem::AccessStatus::kInvalidAddress;

    std::array<uint8_t, kKpcrSize> kpcr_bytes{};
    std::array<uint8_t, kEthreadSize> ethread_bytes{};
    auto put32 = [](auto& bytes, size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    put32(kpcr_bytes, layout::kKpcrExceptionList, 0xFFFFFFFF);
    put32(kpcr_bytes, layout::kKpcrStackBase, stack_top);
    put32(kpcr_bytes, layout::kKpcrStackLimit, stack_top - stack_size);
    put32(kpcr_bytes, layout::kKpcrSelf, kpcr);
    put32(kpcr_bytes, layout::kKpcrSelfPcr, kpcr);
    put32(kpcr_bytes, layout::kKpcrPrcb, kpcr + layout::kKpcrPrcbData);
    put32(kpcr_bytes, layout::kKpcrIrql, 0);
    put32(kpcr_bytes, layout::kKpcrPrcbData + layout::kPrcbCurrentThread, ethread);
    ethread_bytes[layout::kThreadHeaderType] = 6;
    ethread_bytes[layout::kThreadHeaderSize] = uint8_t(kEthreadSize / 4);
    put32(ethread_bytes, layout::kThreadStackBase, stack_top);
    put32(ethread_bytes, layout::kThreadStackLimit, stack_top - stack_size);
    put32(ethread_bytes, layout::kThreadTlsData, tls);
    put32(ethread_bytes, layout::kThreadUniqueThread, thread_id);

    auto status = mem::Write(kpcr, kpcr_bytes.data(), kpcr_bytes.size());
    if (status != mem::AccessStatus::kOk) return status;
    return mem::Write(ethread, ethread_bytes.data(), ethread_bytes.size());
}

// Reads the fields back so a partial write, a stale layout, or a guest that
// scribbled over its own thread blocks cannot pass as an initialized thread.
inline bool ValidateThreadGuestMemory(uint32_t kpcr, uint32_t ethread, uint32_t stack_top, uint32_t stack_size,
                                      uint32_t tls, uint32_t thread_id) {
    if (!kpcr || !ethread || !stack_top || !stack_size || stack_size > stack_top || tls > stack_top) return false;
    std::array<uint32_t, kKpcrSize / 4> k{};
    std::array<uint32_t, kEthreadSize / 4> e{};
    auto index = [](uint32_t byte_offset) { return byte_offset / 4; };
    if (mem::Read(kpcr, k.data(), sizeof(k)) != mem::AccessStatus::kOk ||
        mem::Read(ethread, e.data(), sizeof(e)) != mem::AccessStatus::kOk)
        return false;
    return k[index(layout::kKpcrExceptionList)] == 0xFFFFFFFF &&
           k[index(layout::kKpcrStackBase)] == stack_top &&
           k[index(layout::kKpcrStackLimit)] == stack_top - stack_size &&
           k[index(layout::kKpcrSelf)] == kpcr && k[index(layout::kKpcrSelfPcr)] == kpcr &&
           k[index(layout::kKpcrPrcb)] == kpcr + layout::kKpcrPrcbData &&
           k[index(layout::kKpcrPrcbData + layout::kPrcbCurrentThread)] == ethread &&
           (e[0] & 0xFF) == 6 && ((e[0] >> 16) & 0xFF) == kEthreadSize / 4 &&
           e[index(layout::kThreadStackBase)] == stack_top &&
           e[index(layout::kThreadStackLimit)] == stack_top - stack_size &&
           e[index(layout::kThreadTlsData)] == tls && e[index(layout::kThreadUniqueThread)] == thread_id;
}

}  // namespace kernel
