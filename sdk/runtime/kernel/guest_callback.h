#pragma once

#include <cstdint>
#include <span>

#include "cpu/x86_context.h"

namespace kernel {

enum class GuestCallbackClass : uint8_t {
    kThreadStart,
    kSystemRoutine,
    kApc,
    kFileApc,
    kDpc,
    kTimerApc,
    kTimerDpc,
    kVBlank,
    kSynchronizeExecution,
    kFixture,
};

enum class GuestCallbackArgumentKind : uint8_t {
    kScalar,
    kOpaqueDword,
    kNullableUnresolvedPointer,
    kGuestFunction,
    kBoundedRead,
    kBoundedReadWrite,
};

struct GuestCallbackArgumentContract {
    GuestCallbackArgumentKind kind;
    uint32_t size = 0;
};

struct GuestCallbackContract {
    GuestCallbackClass callback_class;
    std::span<const GuestCallbackArgumentContract> arguments;
    bool variadic = false;
};

const GuestCallbackContract& CallbackContract(GuestCallbackClass callback_class);

struct GuestCallbackTicket {
    uint32_t target = 0;
    uint64_t dispatch_generation = 0;
    GuestCallbackClass callback_class = GuestCallbackClass::kFixture;
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
    kArgumentRejected,
};

GuestCallbackStatus PrepareGuestCallback(uint32_t target, GuestCallbackClass callback_class,
                                         GuestCallbackTicket& ticket);
GuestCallbackStatus CallGuestCallback(X86Context& ctx, const GuestCallbackTicket& ticket, uint32_t current_target,
                                      std::span<const uint32_t> arguments, uint32_t stack_base, uint32_t stack_size,
                                      uint32_t& result);

#ifdef MOSTWANTED_GUEST_CALLBACK_TESTS
GuestCallbackStatus CallGuestCallbackForTest(X86Context& ctx, const GuestCallbackTicket& ticket,
                                             uint32_t current_target, std::span<const uint32_t> arguments,
                                             uint32_t stack_base, uint32_t stack_size, uint32_t& result,
                                             uint32_t* final_esp = nullptr);
#endif

}  // namespace kernel
