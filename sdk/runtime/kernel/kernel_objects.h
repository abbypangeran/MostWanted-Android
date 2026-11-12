#pragma once

#include <cstddef>
#include <cstdint>

#include "kernel/memory.h"

namespace kernel {

enum class KernelObjectStatus : uint8_t {
    kOk,
    kNotInitialized,
    kInvalidAddress,
    kUncommitted,
    kPermissionDenied,
    kBusy,
    kInvalidType,
    kHostError,
};

const char* KernelObjectStatusName(KernelObjectStatus status);

// Xbox KDPC: Type 19 at +0, Inserted/Padding zero, empty DpcListEntry at
// +4/+8, DeferredRoutine at +0x0C, DeferredContext at +0x10, SystemArgument1/2
// at +0x14/+0x18. The complete 0x1C span is validated and pinned before the
// object is published. The callback is stored only; it is never invoked.
KernelObjectStatus InitializeDpcChecked(uint32_t dpc, uint32_t routine, uint32_t context);

// Xbox KTIMER: dispatcher type 8 (TimerNotificationObject) or 9
// (TimerSynchronizationObject), size 10 dwords, signal state 0, empty wait
// list at +8/+12 and zero DueTime/TimerListEntry/Dpc/Period. The complete
// 0x28 span is validated and pinned. No timer is scheduled.
KernelObjectStatus InitializeTimerExChecked(uint32_t timer, uint32_t type);

}  // namespace kernel
