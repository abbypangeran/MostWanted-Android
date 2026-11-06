#include "kernel/kernel_thunks.h"

#include <algorithm>
#include <utility>

namespace kernel {
namespace {

constexpr std::size_t kMaximumThunkEntries = 65536;

struct Patch {
    uint32_t address;
    uint32_t original;
    uint32_t replacement;
    bool implemented;
};

}  // namespace

bool PatchKernelThunks(const xbe::Image& image, ResolveThunk resolve, void* context, std::vector<uint32_t>& missing,
                       std::string& error, ThunkMemory memory) {
    error.clear();
    missing.clear();
    if (!resolve) {
        error = "kernel thunk resolver is missing";
        return false;
    }

    const int64_t file_offset = image.Offset(image.kernel_thunk);
    const std::size_t span = image.FileSpan(image.kernel_thunk);
    if ((image.kernel_thunk & 3u) || file_offset < 0 || !span || uint64_t(file_offset) + span > image.file.size()) {
        error = "kernel thunk table is outside file-backed XBE data";
        return false;
    }
    const std::size_t capacity = std::min(span / sizeof(uint32_t), kMaximumThunkEntries);
    if (!capacity) {
        error = "kernel thunk table has no complete entry";
        return false;
    }

    const auto read = memory.read ? memory.read : &mem::Read;
    const auto write = memory.write ? memory.write : &mem::Write;
    std::vector<Patch> patches;
    patches.reserve(std::min<std::size_t>(capacity, 512));
    std::vector<uint32_t> unresolved;
    bool terminated = false;

    mem::ScopedLock memory_lock;
    for (std::size_t i = 0; i < capacity; ++i) {
        const uint64_t slot64 = uint64_t(image.kernel_thunk) + i * sizeof(uint32_t);
        if (slot64 + sizeof(uint32_t) > uint64_t(UINT32_MAX) + 1) {
            error = "kernel thunk guest address overflows";
            return false;
        }
        const uint32_t slot = uint32_t(slot64);
        uint32_t encoded = 0;
        const mem::AccessStatus read_status = read(slot, &encoded, sizeof(encoded));
        if (read_status != mem::AccessStatus::kOk) {
            error = "kernel thunk read failed: " + std::string(mem::StatusName(read_status));
            return false;
        }
        if (!encoded) {
            terminated = true;
            break;
        }

        const uint32_t ordinal = encoded & 0x7FFFFFFFu;
        if (ordinal >= 400) {
            error = "kernel thunk ordinal is outside the export table";
            return false;
        }
        uint32_t target = 0;
        bool implemented = false;
        if (!resolve(ordinal, target, implemented, context) || !target) {
            error = "kernel thunk ordinal could not be resolved";
            return false;
        }
        patches.push_back({slot, encoded, target, implemented});
        if (!implemented) unresolved.push_back(ordinal);
    }
    if (!terminated) {
        error = "kernel thunk table is unterminated within its bounded file span";
        return false;
    }

    for (std::size_t i = 0; i < patches.size(); ++i) {
        const Patch& patch = patches[i];
        const mem::AccessStatus status = write(patch.address, &patch.replacement, sizeof(patch.replacement));
        if (status == mem::AccessStatus::kOk) continue;
        error = "kernel thunk write failed: " + std::string(mem::StatusName(status));
        for (std::size_t undo = i + 1; undo > 0; --undo) {
            const Patch& previous = patches[undo - 1];
            const mem::AccessStatus rollback = write(previous.address, &previous.original, sizeof(previous.original));
            if (rollback != mem::AccessStatus::kOk)
                error += "; rollback failed: " + std::string(mem::StatusName(rollback));
        }
        return false;
    }

    missing.swap(unresolved);
    return true;
}

}  // namespace kernel
