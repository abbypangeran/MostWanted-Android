#include "cpu/dispatch_table.h"

#include <exception>

namespace dispatch {
namespace {

// shortcut: 8 MiB allows roughly 2x the current 3.75 MiB game range; raise it if generated code approaches the cap.
constexpr uint32_t kMaximumCodeSpan = 8u << 20;

}  // namespace

bool Table::Initialize(const Metadata& metadata, std::string& error) {
    error.clear();
    if (metadata.code_base >= metadata.code_end || metadata.entry_point < metadata.code_base ||
        metadata.entry_point >= metadata.code_end) {
        error = "dispatch code bounds or entry point are invalid";
        return false;
    }

    const uint32_t span = metadata.code_end - metadata.code_base;
    if (span > kMaximumCodeSpan) {
        error = "dispatch code range exceeds the 8 MiB limit";
        return false;
    }
    std::vector<X86Func*> functions;
    std::vector<bool> continuations;
    try {
        functions.assign(span, nullptr);
        continuations.assign(span, false);
    } catch (const std::exception&) {
        error = "dispatch code range is too large to allocate";
        return false;
    }
    for (const X86FuncMapping& function : metadata.functions) {
        if (!function.host || function.guest < metadata.code_base || function.guest >= metadata.code_end) {
            error = "dispatch function mapping is invalid";
            return false;
        }
        X86Func*& slot = functions[function.guest - metadata.code_base];
        if (slot) {
            error = "dispatch function mapping is duplicated";
            return false;
        }
        slot = function.host;
    }
    if (!functions[metadata.entry_point - metadata.code_base]) {
        error = "dispatch entry point has no function mapping";
        return false;
    }

    for (uint32_t continuation : metadata.continuations) {
        if (continuation < metadata.code_base || continuation >= metadata.code_end) {
            error = "dispatch continuation is outside code bounds";
            return false;
        }
        const std::size_t index = continuation - metadata.code_base;
        if (continuations[index]) {
            error = "dispatch continuation is duplicated";
            return false;
        }
        continuations[index] = true;
    }

    code_base_ = metadata.code_base;
    code_end_ = metadata.code_end;
    functions_.swap(functions);
    continuations_.swap(continuations);
    return true;
}

void Table::Reset() {
    code_base_ = 0;
    code_end_ = 0;
    functions_.clear();
    continuations_.clear();
}

X86Func* Table::Find(uint32_t guest) const {
    if (guest < code_base_ || guest >= code_end_) return nullptr;
    return functions_[guest - code_base_];
}

bool Table::IsContinuation(uint32_t guest) const {
    return guest >= code_base_ && guest < code_end_ && continuations_[guest - code_base_];
}

}  // namespace dispatch
