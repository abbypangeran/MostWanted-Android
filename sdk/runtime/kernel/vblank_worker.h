#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

#include "cpu/x86_context.h"
#include "kernel/guest_callback.h"
#include "kernel/worker_lifecycle.h"

namespace kernel {

struct VBlankWorkerConfig {
    uint32_t device;
    uint32_t device_size;
    uint32_t count_offset;
    uint32_t swaps_offset;
    uint32_t event_offset;
    uint32_t callback_offset;
    std::recursive_mutex* device_lock;
    const std::atomic<bool>* focus_paused = nullptr;
};

using VBlankCallbackInvoker = GuestCallbackStatus (*)(X86Context&, const GuestCallbackTicket&, uint32_t,
                                                       std::span<const uint32_t>, uint32_t, uint32_t, uint32_t&);

bool StartVBlankWorker(const char* name, const VBlankWorkerConfig& config);

#ifdef MOSTWANTED_VBLANK_RUNTIME_TESTS
void SetVBlankCallbackInvokerForTest(VBlankCallbackInvoker invoker);
#endif

}  // namespace kernel
