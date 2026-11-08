#pragma once

#include <cstdint>
#include <span>

#include "cpu/x86_context.h"

namespace kernel {

struct GuestCallbackTicket {
    uint32_t target = 0;
    uint64_t dispatch_generation = 0;
};

enum class GuestCallbackStatus : uint8_t {
    kReady,
    kInvoked,
    kExecutionDisabled,
    kCancelled,
    kNullTarget,
    kRejectedTarget,
    kTargetChanged,
    kStaleDispatch,
    kCodePageDenied,
    kInvalidStack,
    kStackWriteDenied,
    kTooManyArguments,
};

GuestCallbackStatus PrepareGuestCallback(uint32_t target, GuestCallbackTicket& ticket);
GuestCallbackStatus CallGuestCallback(X86Context& ctx, const GuestCallbackTicket& ticket, uint32_t current_target,
                                      std::span<const uint32_t> arguments, uint32_t stack_base, uint32_t stack_size,
                                      uint32_t& result);

#ifdef MOSTWANTED_GUEST_CALLBACK_TESTS
GuestCallbackStatus CallGuestCallbackForTest(X86Context& ctx, const GuestCallbackTicket& ticket,
                                             uint32_t current_target, std::span<const uint32_t> arguments,
                                             uint32_t stack_base, uint32_t stack_size, uint32_t& result);
#endif

}  // namespace kernel
