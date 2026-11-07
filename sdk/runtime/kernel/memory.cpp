#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/clock_worker.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mem {

static uint8_t* g_base = nullptr;
static bool g_poisoned = false;
// Page mutations mark and wait without g_lock so leased users can publish through checked memory access.
static std::recursive_mutex g_lock;
static std::recursive_mutex g_virtual_ranges_lock;
constexpr uint32_t kGuestPageCount = 1u << 20;
static std::mutex g_page_lease_lock;
static std::condition_variable g_page_lease_changed;
static std::vector<uint32_t> g_page_leases(kGuestPageCount, 0);
static std::vector<uint8_t> g_page_mutating(kGuestPageCount, 0);
#ifdef MOSTWANTED_GUEST_PAGE_LEASE_TESTS
static std::atomic<PageMutationWaitHook> g_page_mutation_wait_hook{nullptr};
#endif

namespace {

class ScopedPageMutation {
public:
    ScopedPageMutation(uint32_t first, uint32_t count) : first_(first), count_(count) {
        if (!count_) return;
        std::unique_lock lock(g_page_lease_lock);
        g_page_lease_changed.wait(lock, [&] {
            for (uint32_t p = first_; p < first_ + count_; ++p)
                if (g_page_mutating[p]) return false;
            return true;
        });
        for (uint32_t p = first_; p < first_ + count_; ++p) g_page_mutating[p] = 1;
#ifdef MOSTWANTED_GUEST_PAGE_LEASE_TESTS
        auto pinned = [&] {
            for (uint32_t p = first_; p < first_ + count_; ++p)
                if (g_page_leases[p]) return true;
            return false;
        };
        if (pinned())
            if (auto hook = g_page_mutation_wait_hook.load()) hook();
        g_page_lease_changed.wait(lock, [&] { return !pinned(); });
#else
        g_page_lease_changed.wait(lock, [&] {
            for (uint32_t p = first_; p < first_ + count_; ++p)
                if (g_page_leases[p]) return false;
            return true;
        });
#endif
        active_ = true;
    }

    ~ScopedPageMutation() {
        if (!active_) return;
        {
            std::lock_guard lock(g_page_lease_lock);
            for (uint32_t p = first_; p < first_ + count_; ++p) g_page_mutating[p] = 0;
        }
        g_page_lease_changed.notify_all();
    }

private:
    uint32_t first_;
    uint32_t count_;
    bool active_ = false;
};

}  // namespace

ScopedLock::ScopedLock() : lock_(g_lock) {}
ScopedLock::~ScopedLock() = default;
void ScopedLock::Unlock() { if (lock_.owns_lock()) lock_.unlock(); }
void ScopedLock::Lock() { if (!lock_.owns_lock()) lock_.lock(); }
ScopedVirtualRangeLock::ScopedVirtualRangeLock() : lock_(g_virtual_ranges_lock) {}
ScopedVirtualRangeLock::~ScopedVirtualRangeLock() = default;
#ifdef _WIN32
static HANDLE g_ram_mapping = nullptr;
#endif

uint8_t* Base() {
#ifdef __APPLE__
    return nullptr;  // raw Darwin access remains blocked until all callers use checked operations
#else
    return g_base;
#endif
}

bool IsInitialized() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_base != nullptr && !g_poisoned;
}

// physical memory shows at 0x80000000 and again at 0xF0000000 (write combined view)
static bool InPhysical(uint32_t va) {
    return (va >= kContiguousBase && va < kContiguousBase + kPhysicalSize) ||
           (va >= kWriteCombineBase && va < kWriteCombineBase + kPhysicalSize);
}

static bool OverlapsPhysical(uint64_t start, uint64_t end) {
    return (start < uint64_t(kContiguousBase) + kPhysicalSize && end > kContiguousBase) ||
           (start < uint64_t(kWriteCombineBase) + kPhysicalSize && end > kWriteCombineBase);
}

static bool IsPhysicalRange(uint64_t start, uint64_t end) {
    return (start >= kContiguousBase && end <= uint64_t(kContiguousBase) + kPhysicalSize) ||
           (start >= kWriteCombineBase && end <= uint64_t(kWriteCombineBase) + kPhysicalSize);
}

bool Init() {
    kernel::KernelMemoryLifecycle lifecycle;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_base) {
        return lifecycle.SetReady(!g_poisoned);
    }
    const uint64_t size = 1ull << 32;
#ifdef _WIN32
    HANDLE ram = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPhysicalSize, nullptr);
    // a fixed base keeps crash dumps comparable between runs
    for (uint64_t hint : {0x100000000ull, 0x200000000ull, 0x300000000ull, 0ull}) {
        uint8_t* b = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), size, MEM_RESERVE, PAGE_NOACCESS));
        if (!b) continue;
        VirtualFree(b, 0, MEM_RELEASE);
        // reserve around the two physical windows, then map the same memory into both
        const uint64_t cuts[][2] = {{0, kContiguousBase},
                                    {kContiguousBase + kPhysicalSize, kWriteCombineBase},
                                    {kWriteCombineBase + kPhysicalSize, size}};
        bool ok = ram != nullptr;
        for (auto& c : cuts)
            ok = ok && VirtualAlloc(b + c[0], size_t(c[1] - c[0]), MEM_RESERVE, PAGE_NOACCESS) == b + c[0];
        ok = ok && MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, b + kContiguousBase) == b + kContiguousBase;
        ok = ok && MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, b + kWriteCombineBase) == b + kWriteCombineBase;
        if (ok) {
            g_base = b;
            g_ram_mapping = ram;
            break;
        }
        for (auto& c : cuts) VirtualFree(b + c[0], 0, MEM_RELEASE);
        UnmapViewOfFile(b + kContiguousBase);
        UnmapViewOfFile(b + kWriteCombineBase);
    }
    if (!g_base && ram) CloseHandle(ram);
#else
    auto create_backing = []() {
#ifdef __APPLE__
        const char* tmp = std::getenv("TMPDIR");
        if (!tmp || !*tmp) {
            errno = ENOENT;
            return -1;
        }
        std::string path(tmp);
        if (path.back() != '/') path.push_back('/');
        path += "mostwanted-ram-XXXXXX";
        std::vector<char> name(path.begin(), path.end());
        name.push_back('\0');
        int file = mkstemp(name.data());
        if (file >= 0 && unlink(name.data())) {
            int error = errno;
            close(file);
            errno = error;
            return -1;
        }
        return file;
#else
        return memfd_create("ram", 0);
#endif
    };
    void* reservation = mmap(reinterpret_cast<void*>(0x100000000ull), size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation != MAP_FAILED) {
        uint8_t* base = static_cast<uint8_t*>(reservation);
        int fd = create_backing();
        if (fd >= 0 && !ftruncate(fd, kPhysicalSize)) {
            bool ok = mmap(base + kContiguousBase, kPhysicalSize, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED;
            if (ok)
                ok = mmap(base + kWriteCombineBase, kPhysicalSize, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED;
            if (ok) g_base = base;
        }
        if (fd >= 0) close(fd);
        if (!g_base) munmap(reservation, size);
    }
#endif
    if (!g_base) {
        LOGE("memory: could not reserve the 4 GB guest space");
        return false;
    }
    if (!lifecycle.SetReady(true)) return false;
    LOGI("memory: guest space at %p", static_cast<void*>(g_base));
    return true;
}

// page commit map, one byte per 4 KB page
static std::vector<uint8_t> g_committed(0x100000, 0);
static std::vector<uint8_t> g_permissions(0x100000, kNoAccess);

bool Commit(uint32_t va, uint32_t size) {
    if (!size) return true;
    uint64_t requested_end = uint64_t(va) + size;
    if (requested_end > (1ull << 32)) return false;
    if (OverlapsPhysical(va, requested_end)) return IsPhysicalRange(va, requested_end);
    std::unique_lock<std::recursive_mutex> ranges_lock(g_virtual_ranges_lock, std::defer_lock);
    if (va < kUserHigh && requested_end > kUserLow) ranges_lock.lock();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_base || g_poisoned) return false;
    uint32_t first = va & ~(kPage - 1);
    uint64_t end = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
#ifdef __APPLE__
    static const uint64_t host_page = uint64_t(sysconf(_SC_PAGESIZE));
    if (host_page < kPage || (host_page & (host_page - 1))) return false;
    uint64_t host_first = first & ~(host_page - 1);
    uint64_t host_end = (end + host_page - 1) & ~(host_page - 1);
#endif
#ifdef _WIN32
    if (!VirtualAlloc(g_base + first, size_t(end - first), MEM_COMMIT, PAGE_READWRITE)) {
        LOGE("memory: commit %08X+%X failed", first, uint32_t(end - first));
        return false;
    }
#else
    if (mprotect(g_base +
#ifdef __APPLE__
                     host_first, size_t(host_end - host_first),
#else
                     first, size_t(end - first),
#endif
                 PROT_READ | PROT_WRITE)) {
        LOGE("memory: commit %08X+%X failed", first, uint32_t(end - first));
        return false;
    }
#endif
    for (uint64_t p = first; p < end; p += kPage) {
#ifdef __APPLE__
        if (!g_committed[size_t(p >> 12)]) std::memset(g_base + p, 0, kPage);
#endif
        if (!g_committed[size_t(p >> 12)]) g_permissions[size_t(p >> 12)] = kReadWrite;
        g_committed[size_t(p >> 12)] = 1;
    }
    return true;
}

bool Decommit(uint32_t va, uint32_t size) {
    if (!size) return true;
    uint64_t requested_end = uint64_t(va) + size;
    if (requested_end > (1ull << 32)) return false;
    if (OverlapsPhysical(va, requested_end)) return IsPhysicalRange(va, requested_end);
    uint32_t first = va & ~(kPage - 1);
    uint64_t end = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
    ScopedPageMutation mutation(first >> 12, uint32_t((end - first) >> 12));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_base || g_poisoned) return false;
#ifdef __APPLE__
    static const uint64_t host_page = uint64_t(sysconf(_SC_PAGESIZE));
    if (host_page < kPage || (host_page & (host_page - 1))) return false;
    uint64_t host_first = first & ~(host_page - 1);
    uint64_t host_end = (end + host_page - 1) & ~(host_page - 1);
#endif
#ifdef _WIN32
    if (!VirtualFree(g_base + first, size_t(end - first), MEM_DECOMMIT)) return false;
#elif defined(__APPLE__)
    auto empty_after = [&](uint64_t h) {
        for (uint64_t p = h; p < h + host_page; p += kPage)
            if ((p < first || p >= end) && g_committed[size_t(p >> 12)]) return false;
        return true;
    };
    for (uint64_t h = host_first; h < host_end; h += host_page) {
        if (!empty_after(h)) continue;
        if (mprotect(g_base + h, size_t(host_page), PROT_NONE)) {
            int error = errno;
            for (uint64_t undo = host_first; undo < h; undo += host_page) {
                if (!empty_after(undo)) continue;
                bool was_committed = false;
                for (uint64_t p = undo; p < undo + host_page; p += kPage)
                    was_committed |= g_committed[size_t(p >> 12)] != 0;
                if (mprotect(g_base + undo, size_t(host_page), was_committed ? PROT_READ | PROT_WRITE : PROT_NONE)) {
                    g_poisoned = true;
                    LOGE("memory: could not restore protection at %08X", uint32_t(undo));
                }
            }
            LOGE("memory: decommit protection %08X failed: %s", uint32_t(h), std::strerror(error));
            return false;
        }
    }
    for (uint64_t p = first; p < end; p += kPage) {
        g_committed[size_t(p >> 12)] = 0;
        g_permissions[size_t(p >> 12)] = kNoAccess;
    }
    for (uint64_t h = host_first; h < host_end; h += host_page)
        if (empty_after(h) && madvise(g_base + h, size_t(host_page), MADV_DONTNEED))
            LOGW("memory: decommit discard %08X failed: %s", uint32_t(h), std::strerror(errno));
#else
    if (madvise(g_base + first, size_t(end - first), MADV_DONTNEED) ||
        mprotect(g_base + first, size_t(end - first), PROT_NONE)) {
        LOGE("memory: decommit %08X+%X failed: %s", first, uint32_t(end - first), std::strerror(errno));
        return false;
    }
#endif
#ifndef __APPLE__
    for (uint64_t p = first; p < end; p += kPage) {
        g_committed[size_t(p >> 12)] = 0;
        g_permissions[size_t(p >> 12)] = kNoAccess;
    }
#endif
    return true;
}

bool IsCommitted(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_base && !g_poisoned && (InPhysical(va) || g_committed[va >> 12] != 0);
}

static AccessStatus CheckAccessLocked(uint32_t va, std::size_t size, uint8_t permissions) {
    if (!g_base) return AccessStatus::kNotInitialized;
    if (g_poisoned) return AccessStatus::kHostError;
    if (!size) return AccessStatus::kOk;
    if (size > (1ull << 32) - va) return AccessStatus::kInvalidAddress;
    const uint64_t end = uint64_t(va) + size;
    if (OverlapsPhysical(va, end)) {
        if (!IsPhysicalRange(va, end)) return AccessStatus::kInvalidAddress;
        return permissions & kExecute ? AccessStatus::kPermissionDenied : AccessStatus::kOk;
    }
    for (uint64_t p = va >> 12; p <= (end - 1) >> 12; ++p) {
        if (!g_committed[size_t(p)]) return AccessStatus::kUncommitted;
        if ((g_permissions[size_t(p)] & permissions) != permissions) return AccessStatus::kPermissionDenied;
    }
    return AccessStatus::kOk;
}

AccessStatus CheckAccess(uint32_t va, std::size_t size, uint8_t permissions) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (permissions & ~(kRead | kWrite | kExecute)) return AccessStatus::kPermissionDenied;
    return CheckAccessLocked(va, size, permissions);
}

ScopedPageLease::~ScopedPageLease() { Release(); }

ScopedPageLease::ScopedPageLease(ScopedPageLease&& other) noexcept
    : first_page_(std::exchange(other.first_page_, 0)),
      page_count_(std::exchange(other.page_count_, 0)),
      active_(std::exchange(other.active_, false)) {}

ScopedPageLease& ScopedPageLease::operator=(ScopedPageLease&& other) noexcept {
    if (this == &other) return *this;
    Release();
    first_page_ = std::exchange(other.first_page_, 0);
    page_count_ = std::exchange(other.page_count_, 0);
    active_ = std::exchange(other.active_, false);
    return *this;
}

AccessStatus ScopedPageLease::Acquire(uint32_t va, std::size_t size, uint8_t permissions) {
    Release();
    if (!size) return CheckAccess(va, 0, permissions);
    if (size > (1ull << 32) - va) return AccessStatus::kInvalidAddress;
    const uint32_t first = va >> 12;
    const uint64_t end = uint64_t(va) + size;
    const uint32_t count = uint32_t(((end - 1) >> 12) - first + 1);
    {
        std::unique_lock lock(g_page_lease_lock);
        g_page_lease_changed.wait(lock, [&] {
            for (uint32_t p = first; p < first + count; ++p)
                if (g_page_mutating[p]) return false;
            return true;
        });
        for (uint32_t p = first; p < first + count; ++p) ++g_page_leases[p];
    }
    first_page_ = first;
    page_count_ = count;
    active_ = true;
    const AccessStatus status = CheckAccess(va, size, permissions);
    if (status != AccessStatus::kOk) Release();
    return status;
}

void ScopedPageLease::Release() {
    if (!active_) return;
    {
        std::lock_guard lock(g_page_lease_lock);
        for (uint32_t p = first_page_; p < first_page_ + page_count_; ++p)
            if (g_page_leases[p]) --g_page_leases[p];
    }
    active_ = false;
    first_page_ = page_count_ = 0;
    g_page_lease_changed.notify_all();
}

#ifdef MOSTWANTED_GUEST_PAGE_LEASE_TESTS
void SetPageMutationWaitHookForTest(PageMutationWaitHook hook) { g_page_mutation_wait_hook.store(hook); }
#endif

const char* StatusName(AccessStatus status) {
    switch (status) {
        case AccessStatus::kOk: return "ok";
        case AccessStatus::kNotInitialized: return "not-initialized";
        case AccessStatus::kInvalidAddress: return "invalid-address";
        case AccessStatus::kUncommitted: return "uncommitted";
        case AccessStatus::kPermissionDenied: return "permission-denied";
        case AccessStatus::kHostError: return "host-error";
    }
    return "unknown";
}

AccessStatus Protect(uint32_t va, uint32_t size, uint8_t permissions) {
    if (permissions & ~(kRead | kWrite | kExecute)) return AccessStatus::kPermissionDenied;
    if (!size || uint64_t(va) + size > (1ull << 32)) return AccessStatus::kInvalidAddress;
    const uint64_t end = uint64_t(va) + size;
    if (OverlapsPhysical(va, end)) return AccessStatus::kInvalidAddress;
    const uint64_t first = va & ~(uint64_t(kPage) - 1);
    const uint64_t last = (end + kPage - 1) & ~(uint64_t(kPage) - 1);
    ScopedPageMutation mutation(uint32_t(first >> 12), uint32_t((last - first) >> 12));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    AccessStatus status = CheckAccessLocked(uint32_t(first), size_t(last - first), kNoAccess);
    if (status != AccessStatus::kOk) return status;
    for (uint64_t p = first; p < last; p += kPage) g_permissions[size_t(p >> 12)] = permissions;
    return AccessStatus::kOk;
}

AccessStatus Read(uint32_t va, void* destination, std::size_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (size && !destination) return AccessStatus::kInvalidAddress;
    AccessStatus status = CheckAccessLocked(va, size, kRead);
    if (status == AccessStatus::kOk && size) std::memcpy(destination, g_base + va, size);
    return status;
}

AccessStatus Write(uint32_t va, const void* source, std::size_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (size && !source) return AccessStatus::kInvalidAddress;
    AccessStatus status = CheckAccessLocked(va, size, kWrite);
    if (status == AccessStatus::kOk && size) std::memcpy(g_base + va, source, size);
    return status;
}

AccessStatus Copy(uint32_t destination, uint32_t source, std::size_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    AccessStatus status = CheckAccessLocked(source, size, kRead);
    if (status != AccessStatus::kOk) return status;
    status = CheckAccessLocked(destination, size, kWrite);
    if (status != AccessStatus::kOk || !size) return status;
    if (InPhysical(source) && InPhysical(destination) &&
        (source >= kWriteCombineBase) != (destination >= kWriteCombineBase)) {
        const uint32_t src_offset = source & (kPhysicalSize - 1);
        const uint32_t dst_offset = destination & (kPhysicalSize - 1);
        const uint64_t src_end = uint64_t(src_offset) + size;
        const uint64_t dst_end = uint64_t(dst_offset) + size;
        if (dst_offset == src_offset) {
            return AccessStatus::kOk;
        } else if (dst_offset < src_end && src_offset < dst_end) {
            // Virtual aliases share backing storage; volatile preserves physical overlap order under optimization.
            volatile uint8_t* dst = g_base + destination;
            const volatile uint8_t* src = g_base + source;
            if (dst_offset > src_offset) {
                for (std::size_t i = size; i; --i) dst[i - 1] = src[i - 1];
            } else {
                for (std::size_t i = 0; i < size; ++i) dst[i] = src[i];
            }
        } else {
            std::memcpy(g_base + destination, g_base + source, size);
        }
    } else {
        std::memmove(g_base + destination, g_base + source, size);
    }
    return AccessStatus::kOk;
}

AccessStatus Move(uint32_t destination, uint32_t source, std::size_t size) {
    return Copy(destination, source, size);
}

AccessStatus Fill(uint32_t va, uint8_t value, std::size_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    AccessStatus status = CheckAccessLocked(va, size, kWrite);
    if (status == AccessStatus::kOk && size) std::memset(g_base + va, value, size);
    return status;
}

AccessStatus FillPattern32(uint32_t va, uint32_t value, std::size_t count) {
    if (count > SIZE_MAX / sizeof(value)) return AccessStatus::kInvalidAddress;
    const size_t size = count * sizeof(value);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const AccessStatus status = CheckAccessLocked(va, size, kWrite);
    if (status != AccessStatus::kOk) return status;
    for (size_t offset = 0; offset < size; offset += sizeof(value))
        std::memcpy(g_base + uint64_t(va) + offset, &value, sizeof(value));
    return AccessStatus::kOk;
}

AccessStatus AtomicFetchAdd32(uint32_t va, uint32_t value, uint32_t& previous) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    AccessStatus status = CheckAccessLocked(va, sizeof(value), kReadWrite);
    if (status != AccessStatus::kOk) return status;
    std::memcpy(&previous, g_base + va, sizeof(previous));
    uint32_t updated = previous + value;
    std::memcpy(g_base + va, &updated, sizeof(updated));
    return AccessStatus::kOk;
}

// first fit allocator over a guest range
class Ranges {
public:
    void Clear() { free_.clear(); used_.clear(); }
    void Init(uint32_t start, uint32_t size) { free_[start] = size; }

    void Remove(uint32_t start, uint32_t size) {
        // carve a fixed range out of the free list
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            uint32_t fs = it->first, fz = it->second;
            if (start >= fs && uint64_t(start) + size <= uint64_t(fs) + fz) {
                free_.erase(it);
                if (start > fs) free_[fs] = start - fs;
                if (uint64_t(start) + size < uint64_t(fs) + fz) free_[start + size] = uint32_t(uint64_t(fs) + fz - start - size);
                return;
            }
        }
    }

    uint32_t Alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down) {
        if (!align) align = 1;
        if (top_down) {
            for (auto it = free_.rbegin(); it != free_.rend(); ++it) {
                uint64_t fs = it->first, fe = fs + it->second;
                uint64_t e = std::min<uint64_t>(fe, uint64_t(hi) + 1);
                if (e < size) continue;
                uint64_t a = (e - size) & ~uint64_t(align - 1);
                if (a < fs || a < lo) continue;
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
            return 0;
        }
        for (auto& [fs, fz] : free_) {
            uint64_t a = std::max<uint64_t>(fs, lo);
            a = (a + align - 1) & ~uint64_t(align - 1);
            if (a + size <= uint64_t(fs) + fz && a + size - 1 <= hi) {
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
        }
        return 0;
    }

    bool IsFree(uint32_t start, uint32_t size) const {
        auto it = free_.upper_bound(start);
        if (it == free_.begin()) return false;
        --it;
        return uint64_t(start) + size <= uint64_t(it->first) + it->second;
    }

    bool Free(uint32_t a, uint32_t* size_out = nullptr) {
        auto it = used_.find(a);
        if (it == used_.end()) return false;
        uint32_t size = it->second;
        used_.erase(it);
        if (size_out) *size_out = size;
        Release(a, size);
        return true;
    }

    void Release(uint32_t a, uint32_t size) {
        auto n = free_.emplace(a, size).first;
        auto next = std::next(n);
        if (next != free_.end() && n->first + n->second == next->first) {
            n->second += next->second;
            free_.erase(next);
        }
        if (n != free_.begin()) {
            auto prev = std::prev(n);
            if (prev->first + prev->second == n->first) {
                prev->second += n->second;
                free_.erase(n);
            }
        }
    }

    void Forget(uint32_t a) { used_.erase(a); }

    uint32_t SizeOf(uint32_t a) const {
        auto it = used_.find(a);
        return it == used_.end() ? 0 : it->second;
    }

    uint64_t Used() const {
        uint64_t n = 0;
        for (auto& u : used_) n += u.second;
        return n;
    }

private:
    void Take(uint32_t a, uint32_t size) {
        Remove(a, size);
        used_[a] = size;
    }
    std::map<uint32_t, uint32_t> free_;
    std::map<uint32_t, uint32_t> used_;
};

static Ranges g_contig, g_system, g_stacks;
static std::map<uint32_t, uint32_t> g_stack_tops;
static bool g_ranges_ready = false;

struct Reservation {
    uint32_t size;
    uint32_t protect;
    std::vector<uint8_t> commit;  // per page
};
static std::map<uint32_t, Reservation> g_reserved;
static Ranges g_user;

static void EnsureRanges() {
    if (g_ranges_ready) return;
    g_ranges_ready = true;
    g_contig.Init(kContiguousBase + kKernelDataSize, kPhysicalSize - kKernelDataSize);
    g_system.Init(kSystemBase, kSystemSize);
    g_stacks.Init(kStackBase, kStackSize);
    g_user.Init(kUserLow, kUserHigh - kUserLow);
    Commit(kKernelDataBase, kKernelDataSize);
}

void Shutdown() {
    kernel::KernelMemoryLifecycle lifecycle;
    if (!lifecycle.StopWorker()) {
        LOGE("memory: kernel workers could not be joined; guest memory remains mapped");
        return;
    }
    std::lock_guard<std::recursive_mutex> ranges_lock(g_virtual_ranges_lock);
    ScopedPageMutation mutation(0, kGuestPageCount);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_base) return;
    const uint64_t size = 1ull << 32;
#ifdef _WIN32
    const uint64_t cuts[][2] = {{0, kContiguousBase},
                                {kContiguousBase + kPhysicalSize, kWriteCombineBase},
                                {kWriteCombineBase + kPhysicalSize, size}};
    bool released = UnmapViewOfFile(g_base + kContiguousBase) != 0;
    released = (UnmapViewOfFile(g_base + kWriteCombineBase) != 0) && released;
    for (const auto& cut : cuts)
        released = (VirtualFree(g_base + cut[0], 0, MEM_RELEASE) != 0) && released;
    if (!released) {
        LOGE("memory: could not release the guest address space");
        return;
    }
    if (g_ram_mapping) CloseHandle(g_ram_mapping);
    g_ram_mapping = nullptr;
#else
    if (munmap(g_base, size)) {
        LOGE("memory: could not release the guest address space: %s", std::strerror(errno));
        return;
    }
#endif
    g_base = nullptr;
    g_poisoned = false;
    std::fill(g_committed.begin(), g_committed.end(), 0);
    std::fill(g_permissions.begin(), g_permissions.end(), kNoAccess);
    g_contig.Clear();
    g_system.Clear();
    g_stacks.Clear();
    g_stack_tops.clear();
    g_reserved.clear();
    g_user.Clear();
    g_ranges_ready = false;
}

bool ReserveImage(uint32_t base, uint32_t size) {
    std::lock_guard<std::recursive_mutex> ranges_lock(g_virtual_ranges_lock);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    uint64_t requested_end = uint64_t(base) + size;
    if (!size || base < kUserLow || requested_end > kUserHigh) return false;
    uint32_t b = base & ~0xFFFFu;
    uint64_t rounded_end = (requested_end + 0xFFFF) & ~0xFFFFull;
    if (rounded_end > kUserHigh) return false;
    uint32_t e = uint32_t(rounded_end);
    if (!g_user.IsFree(b, e - b)) return false;
    g_user.Remove(b, e - b);
    Reservation r{e - b, 4, std::vector<uint8_t>((e - b) / kPage, 1)};  // 4 is PAGE_READWRITE
    g_reserved[b] = std::move(r);
    if (!Commit(b, e - b)) {
        g_reserved.erase(b);
        g_user.Release(b, e - b);
        return false;
    }
    return true;
}

static Reservation* FindReservation(uint32_t addr, uint32_t* start) {
    auto it = g_reserved.upper_bound(addr);
    if (it == g_reserved.begin()) return nullptr;
    --it;
    if (addr >= it->first + it->second.size) return nullptr;
    *start = it->first;
    return &it->second;
}

uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect) {
    std::lock_guard<std::recursive_mutex> ranges_lock(g_virtual_ranges_lock);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    if (!size) return 0xC000000D;  // STATUS_INVALID_PARAMETER
    const uint32_t requested_base = base, requested_size = size;
    uint64_t requested_end = uint64_t(requested_base) + requested_size;
    if ((requested_base && (requested_base < kUserLow || requested_end > kUserHigh)) ||
        (!requested_base && requested_size > kUserHigh - kUserLow))
        return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
    uint32_t reserved_base = 0, reserved_size = 0;
    bool reserve = type & kMemReserve;
    bool commit = type & kMemCommit;
    if (!base && commit) reserve = true;
    if (reserve) {
        uint64_t rounded_size = (uint64_t(requested_size) + 0xFFFF) & ~0xFFFFull;
        if (rounded_size > UINT32_MAX) return 0xC0000017;  // STATUS_NO_MEMORY
        uint32_t sz = uint32_t(rounded_size);
        uint32_t a;
        if (requested_base) {
            a = requested_base & ~0xFFFFu;
            uint64_t rounded_end = (requested_end + 0xFFFF) & ~0xFFFFull;
            if (rounded_end > kUserHigh) return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
            sz = uint32_t(rounded_end - a);
            if (!g_user.IsFree(a, sz)) return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
            g_user.Remove(a, sz);
        } else {
            a = g_user.Alloc(sz, 0x10000, kUserLow, kUserHigh - 1, type & kMemTopDown);
            if (!a) return 0xC0000017;  // STATUS_NO_MEMORY
            g_user.Forget(a);
        }
        g_reserved[a] = Reservation{sz, protect, std::vector<uint8_t>(sz / kPage, 0)};
        reserved_base = a;
        reserved_size = sz;
    }
    if (commit) {
        uint32_t start;
        uint32_t requested_commit_base = requested_base ? requested_base : reserved_base;
        Reservation* r = FindReservation(requested_commit_base, &start);
        if (!r) {
            if (reserve) {
                g_reserved.erase(reserved_base);
                g_user.Release(reserved_base, reserved_size);
            }
            return 0xC0000045;  // STATUS_INVALID_PAGE_PROTECTION stands in for not reserved
        }
        uint32_t first = requested_commit_base & ~(kPage - 1);
        uint64_t end64 = (uint64_t(requested_commit_base) + requested_size + kPage - 1) & ~uint64_t(kPage - 1);
        if (end64 > uint64_t(start) + r->size) {
            if (reserve) {
                g_reserved.erase(reserved_base);
                g_user.Release(reserved_base, reserved_size);
            }
            return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
        }
        uint32_t end = uint32_t(end64);
        if (!Commit(first, uint32_t(end - first))) {
            if (reserve) {
                g_reserved.erase(reserved_base);
                g_user.Release(reserved_base, reserved_size);
            }
            return 0xC0000017;  // STATUS_NO_MEMORY
        }
        for (uint32_t p = first; p < end; p += kPage) r->commit[(p - start) / kPage] = 1;
        base = first;
        size = end - first;
    } else if (reserve) {
        base = reserved_base;
        size = reserved_size;
    }
    return 0;
}

uint32_t FreeVirtual(uint32_t& base, uint32_t& size, uint32_t type) {
    std::lock_guard ranges_lock(g_virtual_ranges_lock);
    uint32_t start, first, end, reserved_size;
    const bool release = type & kMemRelease;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        Reservation* r = FindReservation(base, &start);
        if (!r) return 0xC000009F;  // STATUS_MEMORY_NOT_ALLOCATED
        reserved_size = r->size;
        first = release || !size ? start : base & ~(kPage - 1);
        uint64_t end64 = release || !size ? uint64_t(start) + r->size
                                         : (uint64_t(base) + size + kPage - 1) & ~uint64_t(kPage - 1);
        if (end64 > uint64_t(start) + r->size || end64 < first) return 0xC0000018;
        end = uint32_t(end64);
    }
    if (!Decommit(first, end - first)) return 0xC0000017;  // STATUS_NO_MEMORY
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        Reservation* r = FindReservation(start, &start);
        if (!r) return 0xC000009F;
        if (release) {
            g_reserved.erase(start);
            g_user.Release(start, reserved_size);
            base = start;
            size = reserved_size;
        } else {
            for (uint32_t p = first; p < end; p += kPage) r->commit[(p - start) / kPage] = 0;
            base = first;
            size = end - first;
        }
    }
    return 0;
}

bool QueryVirtual(uint32_t addr, RegionInfo& out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t start;
    Reservation* r = FindReservation(addr, &start);
    std::memset(&out, 0, sizeof(out));
    if (!r) {
        out.base = addr & ~(kPage - 1);
        auto it = g_reserved.upper_bound(addr);
        uint32_t next = it == g_reserved.end() ? kUserHigh : it->first;
        out.size = next > out.base ? next - out.base : kPage;
        out.state = 0x10000;  // MEM_FREE
        out.protect = 1;      // PAGE_NOACCESS
        return addr < kUserHigh;
    }
    uint32_t page = (addr - start) / kPage;
    uint8_t c = r->commit[page];
    uint32_t n = page;
    while (n < r->commit.size() && r->commit[n] == c) n++;
    out.base = start + page * kPage;
    out.alloc_base = start;
    out.alloc_protect = r->protect;
    out.size = (n - page) * kPage;
    out.state = c ? kMemCommit : kMemReserve;
    out.protect = c ? r->protect : 0;
    out.type = 0x20000;  // MEM_PRIVATE
    return true;
}

uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    size = (size + kPage - 1) & ~(kPage - 1);
    if (!size) size = kPage;
    align = std::max<uint32_t>(align, kPage);
    uint32_t lo = kContiguousBase | (lowest & (kPhysicalSize - 1));
    uint32_t hi = highest >= kPhysicalSize - 1 ? kContiguousBase + kPhysicalSize - 1 : kContiguousBase | highest;
    // highest fit first: low memory stays free for requests limited to it (D3D report pages, first 16 MB)
    uint32_t a = g_contig.Alloc(size, align, lo, hi, true);
    if (!a) {
        LOGE("memory: contiguous allocation of %X bytes failed (%X..%X align %X), %llu KB in use", size, lowest, highest, align,
             (unsigned long long)(g_contig.Used() >> 10));
        return 0;
    }
    if (!Commit(a, size)) {
        g_contig.Free(a);
        return 0;
    }
    if (Fill(a, 0, size) != AccessStatus::kOk) {
        g_contig.Free(a);
        return 0;
    }
    return a;
}

void FreeContiguous(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t size = g_contig.SizeOf(va);
    if (size && Decommit(va, size)) g_contig.Free(va);
}

uint32_t AllocSystem(uint32_t size, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    uint32_t a = g_system.Alloc(size, std::max<uint32_t>(align, 16), kSystemBase, kSystemBase + kSystemSize - 1, false);
    if (!a) return 0;
    if (!Commit(a, size)) {
        g_system.Free(a);
        return 0;
    }
    if (Fill(a, 0, size) != AccessStatus::kOk) {
        g_system.Free(a);
        return 0;
    }
    return a;
}

void FreeSystem(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_system.Free(va);
}

uint32_t AllocStack(uint32_t size) {
    std::lock_guard<std::recursive_mutex> ranges_lock(g_virtual_ranges_lock);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    size = (size + 0xFFFF) & ~0xFFFFu;
    // one uncommitted guard page below the stack
    uint32_t a = g_stacks.Alloc(size + 0x10000, 0x10000, kStackBase, kStackBase + kStackSize - 1, false);
    if (!a) return 0;
    if (!Commit(a + 0x10000, size)) {
        g_stacks.Free(a);
        return 0;
    }
    g_stack_tops[a + 0x10000 + size] = a;
    return a + 0x10000 + size;
}

void FreeStack(uint32_t top) {
    std::lock_guard<std::recursive_mutex> ranges_lock(g_virtual_ranges_lock);
    uint32_t start, size;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        auto it = g_stack_tops.find(top);
        if (it == g_stack_tops.end()) return;
        start = it->second;
        size = g_stacks.SizeOf(start);
    }
    if (!size || !Decommit(start, size)) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_stack_tops.find(top);
    if (it != g_stack_tops.end() && it->second == start && g_stacks.Free(start)) g_stack_tops.erase(it);
}

uint32_t AllocationSize(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (uint32_t s = g_contig.SizeOf(va)) return s;
    if (uint32_t s = g_system.SizeOf(va)) return s;
    uint32_t start;
    if (Reservation* r = FindReservation(va, &start)) return r->size - (va - start);
    return 0;
}

uint32_t PhysicalFromVirtual(uint32_t va) {
    if (InPhysical(va)) return va & (kPhysicalSize - 1);
    return va;
}

uint32_t VirtualFromPhysical(uint32_t pa) {
    if (pa < kPhysicalSize) return kContiguousBase | pa;
    return pa;
}

Stats Query() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Stats s{};
    s.total_pages = kPhysicalSize / kPage;
    uint64_t vc = 0;
    for (auto& [a, r] : g_reserved)
        for (uint8_t c : r.commit) vc += c;
    s.virtual_committed = uint32_t(vc);
    s.contiguous_used = uint32_t(g_contig.Used() / kPage);
    s.system_used = uint32_t(g_system.Used() / kPage);
    uint64_t used = vc + s.contiguous_used + s.system_used;
    s.available_pages = used >= s.total_pages ? 0 : uint32_t(s.total_pages - used);
    return s;
}

}  // namespace mem
