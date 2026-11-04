#include "guest_memory.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace guest_memory_prototype {
namespace {

bool ValidPermissions(uint8_t permissions) {
    return (permissions & ~(kRead | kWrite | kExecute)) == 0;
}

}  // namespace

GuestMemory::~GuestMemory() { Shutdown(); }

Status GuestMemory::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aliases_[0] && aliases_[1] && backing_fd_ >= 0) return Status::kOk;
    if (aliases_[0] || aliases_[1] || backing_fd_ >= 0) return Status::kHostError;

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size != kHostPageSize) return Status::kHostError;
    host_page_size_ = uint32_t(page_size);

    const char* temp_dir = std::getenv("TMPDIR");
    if (!temp_dir || !*temp_dir) return Status::kHostError;
    char path[PATH_MAX];
    const size_t directory_size = std::strlen(temp_dir);
    const int path_size = std::snprintf(path, sizeof(path), "%s%sguest-memory-prototype-XXXXXX", temp_dir,
                                        temp_dir[directory_size - 1] == '/' ? "" : "/");
    if (path_size < 0 || size_t(path_size) >= sizeof(path)) return Status::kHostError;

    int fd = mkstemp(path);
    if (fd < 0) return Status::kHostError;
    if (unlink(path) || ftruncate(fd, off_t(kWindowSize))) {
        close(fd);
        unlink(path);
        return Status::kHostError;
    }

    void* first = mmap(nullptr, kWindowSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (first == MAP_FAILED) {
        close(fd);
        return Status::kHostError;
    }
    void* second = mmap(nullptr, kWindowSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (second == MAP_FAILED) {
        munmap(first, kWindowSize);
        close(fd);
        return Status::kHostError;
    }

    aliases_[0] = static_cast<uint8_t*>(first);
    aliases_[1] = static_cast<uint8_t*>(second);
    backing_fd_ = fd;
    ResetPages();
    return Status::kOk;
}

Status GuestMemory::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool failed = false;
    for (uint8_t*& alias : aliases_) {
        if (!alias) continue;
        if (munmap(alias, kWindowSize)) failed = true;
        else alias = nullptr;
    }
    if (backing_fd_ >= 0) {
        if (close(backing_fd_)) failed = true;
        backing_fd_ = -1;
    }
    if (!aliases_[0] && !aliases_[1] && backing_fd_ < 0) {
        ResetPages();
        host_page_size_ = 0;
    }
    return failed ? Status::kHostError : Status::kOk;
}

bool GuestMemory::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aliases_[0] && aliases_[1] && backing_fd_ >= 0;
}

Status GuestMemory::Locate(uint32_t address, std::size_t size, View& view, std::size_t& offset) const {
    if (!size || size > kWindowSize) return Status::kInvalidAddress;
    const uint64_t end = uint64_t(address) + size;
    const uint32_t bases[] = {kGuestBase, kPhysicalAliasBase, kWriteCombineAliasBase};
    for (uint8_t i = 0; i < std::size(bases); ++i) {
        const uint64_t base = bases[i];
        if (address >= base && end <= base + kWindowSize) {
            view = View(i);
            offset = std::size_t(uint64_t(address) - base);
            return Status::kOk;
        }
    }
    return Status::kInvalidAddress;
}

Status GuestMemory::ValidateLocked(std::size_t offset, std::size_t size, uint8_t permissions) const {
    if (!size) return Status::kOk;
    const std::size_t first = offset / kGuestPageSize;
    const std::size_t last = (offset + size - 1) / kGuestPageSize;
    for (std::size_t page = first; page <= last; ++page) {
        const PageEntry& entry = pages_[page];
        if (!entry.committed) return Status::kUncommitted;
        if ((entry.permissions & permissions) != permissions) return Status::kPermissionDenied;
    }
    return Status::kOk;
}

uint8_t* GuestMemory::ViewBase(View view) const {
    return view == View::kWriteCombine ? aliases_[1] : aliases_[0];
}

void GuestMemory::TransferLocked(View view, std::size_t offset, void* bytes, std::size_t size, bool write) const {
    auto* buffer = static_cast<uint8_t*>(bytes);
    while (size) {
        const std::size_t page_index = offset / kGuestPageSize;
        const std::size_t in_page = offset % kGuestPageSize;
        const std::size_t count = std::min(size, std::size_t(kGuestPageSize) - in_page);
        uint8_t* guest = ViewBase(view) + std::size_t(pages_[page_index].frame) * kGuestPageSize + in_page;
        if (write) std::memcpy(guest, buffer, count);
        else std::memcpy(buffer, guest, count);
        offset += count;
        buffer += count;
        size -= count;
    }
}

void GuestMemory::ResetPages() {
    for (std::size_t i = 0; i < pages_.size(); ++i)
        pages_[i] = PageEntry{false, kNoAccess, uint16_t((i & ~std::size_t(3)) + (3 - (i & 3)))};
}

Status GuestMemory::Commit(uint32_t address, std::size_t size, uint8_t permissions) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    if (!ValidPermissions(permissions)) return Status::kPermissionDenied;
    View view;
    std::size_t offset;
    Status status = Locate(address, size, view, offset);
    if (status != Status::kOk) return status;
    const std::size_t first = offset & ~(std::size_t(kGuestPageSize) - 1);
    const std::size_t end = (offset + size + kGuestPageSize - 1) & ~(std::size_t(kGuestPageSize) - 1);
    if (end > kWindowSize) return Status::kInvalidAddress;
    for (std::size_t page = first / kGuestPageSize; page < end / kGuestPageSize; ++page) {
        PageEntry& entry = pages_[page];
        if (entry.committed) continue;
        std::memset(aliases_[0] + std::size_t(entry.frame) * kGuestPageSize, 0, kGuestPageSize);
        entry.committed = true;
        entry.permissions = permissions;
    }
    return Status::kOk;
}

Status GuestMemory::Decommit(uint32_t address, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    View view;
    std::size_t offset;
    Status status = Locate(address, size, view, offset);
    if (status != Status::kOk) return status;
    const std::size_t first = offset & ~(std::size_t(kGuestPageSize) - 1);
    const std::size_t end = (offset + size + kGuestPageSize - 1) & ~(std::size_t(kGuestPageSize) - 1);
    if (end > kWindowSize) return Status::kInvalidAddress;
    for (std::size_t page = first / kGuestPageSize; page < end / kGuestPageSize; ++page) {
        PageEntry& entry = pages_[page];
        std::memset(aliases_[0] + std::size_t(entry.frame) * kGuestPageSize, 0, kGuestPageSize);
        entry.committed = false;
        entry.permissions = kNoAccess;
    }
    return Status::kOk;
}

Status GuestMemory::Protect(uint32_t address, std::size_t size, uint8_t permissions) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    if (!ValidPermissions(permissions)) return Status::kPermissionDenied;
    View view;
    std::size_t offset;
    Status status = Locate(address, size, view, offset);
    if (status != Status::kOk) return status;
    const std::size_t first = offset & ~(std::size_t(kGuestPageSize) - 1);
    const std::size_t end = (offset + size + kGuestPageSize - 1) & ~(std::size_t(kGuestPageSize) - 1);
    if (end > kWindowSize) return Status::kInvalidAddress;
    status = ValidateLocked(first, end - first, kNoAccess);
    if (status != Status::kOk) return status;
    for (std::size_t page = first / kGuestPageSize; page < end / kGuestPageSize; ++page)
        pages_[page].permissions = permissions;
    return Status::kOk;
}

Status GuestMemory::Read(uint32_t address, void* destination, std::size_t size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    if (!size) return Status::kOk;
    if (!destination) return Status::kInvalidAddress;
    View view;
    std::size_t offset;
    Status status = Locate(address, size, view, offset);
    if (status != Status::kOk) return status;
    status = ValidateLocked(offset, size, kRead);
    if (status == Status::kOk) TransferLocked(view, offset, destination, size, false);
    return status;
}

Status GuestMemory::Write(uint32_t address, const void* source, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    if (!size) return Status::kOk;
    if (!source) return Status::kInvalidAddress;
    View view;
    std::size_t offset;
    Status status = Locate(address, size, view, offset);
    if (status != Status::kOk) return status;
    status = ValidateLocked(offset, size, kWrite);
    if (status == Status::kOk) TransferLocked(view, offset, const_cast<void*>(source), size, true);
    return status;
}

Status GuestMemory::Copy(uint32_t destination, uint32_t source, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    if (!size) return Status::kOk;
    View source_view, destination_view;
    std::size_t source_offset, destination_offset;
    Status status = Locate(source, size, source_view, source_offset);
    if (status != Status::kOk) return status;
    status = Locate(destination, size, destination_view, destination_offset);
    if (status != Status::kOk) return status;
    status = ValidateLocked(source_offset, size, kRead);
    if (status != Status::kOk) return status;
    status = ValidateLocked(destination_offset, size, kWrite);
    if (status != Status::kOk) return status;
    try {
        std::vector<uint8_t> temporary(size);
        TransferLocked(source_view, source_offset, temporary.data(), size, false);
        TransferLocked(destination_view, destination_offset, temporary.data(), size, true);
    } catch (...) {
        return Status::kHostError;
    }
    return Status::kOk;
}

Status GuestMemory::Fill(uint32_t address, uint8_t value, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    if (!size) return Status::kOk;
    View view;
    std::size_t offset;
    Status status = Locate(address, size, view, offset);
    if (status != Status::kOk) return status;
    status = ValidateLocked(offset, size, kWrite);
    if (status != Status::kOk) return status;
    while (size) {
        const std::size_t page_index = offset / kGuestPageSize;
        const std::size_t in_page = offset % kGuestPageSize;
        const std::size_t count = std::min(size, std::size_t(kGuestPageSize) - in_page);
        uint8_t* guest = ViewBase(view) + std::size_t(pages_[page_index].frame) * kGuestPageSize + in_page;
        std::memset(guest, value, count);
        offset += count;
        size -= count;
    }
    return Status::kOk;
}

Status GuestMemory::FetchAdd32(uint32_t address, uint32_t value, uint32_t& previous) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    View view;
    std::size_t offset;
    Status status = Locate(address, sizeof(uint32_t), view, offset);
    if (status != Status::kOk) return status;
    status = ValidateLocked(offset, sizeof(uint32_t), kRead | kWrite);
    if (status != Status::kOk) return status;
    uint32_t current = 0;
    TransferLocked(view, offset, &current, sizeof(current), false);
    previous = current;
    current += value;
    TransferLocked(view, offset, &current, sizeof(current), true);
    return Status::kOk;
}

Status GuestMemory::CheckExecute(uint32_t address, std::size_t size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_[0] || !aliases_[1]) return Status::kNotInitialized;
    View view;
    std::size_t offset;
    Status status = Locate(address, size, view, offset);
    if (status != Status::kOk) return status;
    return ValidateLocked(offset, size, kExecute);
}

}  // namespace guest_memory_prototype
