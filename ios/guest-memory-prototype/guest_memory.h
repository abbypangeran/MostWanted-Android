#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace guest_memory_prototype {

constexpr uint32_t kGuestPageSize = 0x1000;
constexpr uint32_t kHostPageSize = 0x4000;
constexpr uint32_t kGuestBase = 0x00010000;
constexpr uint32_t kPhysicalAliasBase = 0x80000000;
constexpr uint32_t kWriteCombineAliasBase = 0xF0000000;
constexpr std::size_t kWindowSize = 0x40000;
constexpr std::size_t kGuestPageCount = kWindowSize / kGuestPageSize;

enum Permission : uint8_t {
    kNoAccess = 0,
    kRead = 1,
    kWrite = 2,
    kExecute = 4,
    kReadWrite = kRead | kWrite,
};

enum class Status : uint8_t {
    kOk,
    kNotInitialized,
    kInvalidAddress,
    kUncommitted,
    kPermissionDenied,
    kHostError,
};

class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    Status Init();
    Status Shutdown();
    bool IsInitialized() const;
    uint32_t HostPageSize() const { return host_page_size_; }

    Status Commit(uint32_t address, std::size_t size, uint8_t permissions = kReadWrite);
    Status Decommit(uint32_t address, std::size_t size);
    Status Protect(uint32_t address, std::size_t size, uint8_t permissions);
    Status Read(uint32_t address, void* destination, std::size_t size) const;
    Status Write(uint32_t address, const void* source, std::size_t size);
    Status Copy(uint32_t destination, uint32_t source, std::size_t size);
    Status Fill(uint32_t address, uint8_t value, std::size_t size);
    Status FetchAdd32(uint32_t address, uint32_t value, uint32_t& previous);
    Status CheckExecute(uint32_t address, std::size_t size) const;

private:
    enum class View : uint8_t { kGuest, kPhysical, kWriteCombine };
    struct PageEntry {
        bool committed = false;
        uint8_t permissions = kNoAccess;
        uint16_t frame = 0;
    };

    Status Locate(uint32_t address, std::size_t size, View& view, std::size_t& offset) const;
    Status ValidateLocked(std::size_t offset, std::size_t size, uint8_t permissions) const;
    void TransferLocked(View view, std::size_t offset, void* bytes, std::size_t size, bool write) const;
    uint8_t* ViewBase(View view) const;
    void ResetPages();

    mutable std::mutex mutex_;
    std::array<PageEntry, kGuestPageCount> pages_{};
    uint8_t* aliases_[2]{};
    int backing_fd_ = -1;
    uint32_t host_page_size_ = 0;
};

}  // namespace guest_memory_prototype
