#include "kernel/checked_file_transfer.h"

#include <algorithm>
#include <new>
#include <vector>

namespace kernel::checked_file_transfer {

namespace {

Result MemoryFailure(mem::AccessStatus status) {
    return {Status::kGuestMemory, status, 0};
}

bool AllocateStaging(uint32_t length, std::vector<uint8_t>& staging) {
    try {
        staging.resize(length);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

}  // namespace

Result ReadFromHost(uint32_t guest_buffer, uint32_t length, void* context, HostRead read) {
    if (length > kMaxStagedBytes) return {Status::kRequestTooLarge, mem::AccessStatus::kOk, 0};
    if (!length) return {};

    const mem::AccessStatus access = mem::CheckAccess(guest_buffer, length, mem::kWrite);
    if (access != mem::AccessStatus::kOk) return MemoryFailure(access);
    if (!read) return {Status::kHostContract, mem::AccessStatus::kOk, 0};

    std::vector<uint8_t> staging;
    if (!AllocateStaging(length, staging)) return {Status::kNoMemory, mem::AccessStatus::kOk, 0};

    const std::size_t transferred = read(context, staging.data(), staging.size());
    if (transferred > staging.size()) return {Status::kHostContract, mem::AccessStatus::kOk, 0};
    if (transferred) {
        const mem::AccessStatus publish = mem::Write(guest_buffer, staging.data(), transferred);
        if (publish != mem::AccessStatus::kOk) return MemoryFailure(publish);
    }
    return {Status::kOk, mem::AccessStatus::kOk, uint32_t(transferred)};
}

Result WriteToHost(uint32_t guest_buffer, uint32_t length, void* context, HostWrite write) {
    if (length > kMaxStagedBytes) return {Status::kRequestTooLarge, mem::AccessStatus::kOk, 0};
    if (!length) return {};
    if (!write) return {Status::kHostContract, mem::AccessStatus::kOk, 0};
    const mem::AccessStatus access = mem::CheckAccess(guest_buffer, length, mem::kRead);
    if (access != mem::AccessStatus::kOk) return MemoryFailure(access);

    std::vector<uint8_t> staging;
    if (!AllocateStaging(length, staging)) return {Status::kNoMemory, mem::AccessStatus::kOk, 0};

    const mem::AccessStatus snapshot = mem::Read(guest_buffer, staging.data(), staging.size());
    if (snapshot != mem::AccessStatus::kOk) return MemoryFailure(snapshot);

    const std::size_t transferred = write(context, staging.data(), staging.size());
    if (transferred > staging.size()) return {Status::kHostContract, mem::AccessStatus::kOk, 0};
    return {Status::kOk, mem::AccessStatus::kOk, uint32_t(transferred)};
}

}  // namespace kernel::checked_file_transfer
