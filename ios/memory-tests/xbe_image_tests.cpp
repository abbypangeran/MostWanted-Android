#include "kernel/crypto.h"
#include "cpu/dispatch_table.h"
#include "kernel/clock_worker.h"
#include "kernel/filesystem_context.h"
#include "kernel/kernel_data_init.h"
#include "kernel/kernel_thunks.h"
#include "kernel/memory.h"
#include "kernel/thread_memory.h"
#include "xbe/image_loader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kBase = 0x10000;
constexpr uint32_t kEntry = 0x11000;
constexpr uint32_t kSecondSection = 0x12000;
constexpr char kSyntheticSha1[] = "2b96c952a21e35bd9611e9ec398d947f7dc735e2";
constexpr char kGameSha1[] = "f636a16af04171089f879a4bed646e185c734233";
constexpr uint32_t kGameEntry = 0x002B3916;

int failures = 0;
unsigned injected_write_count = 0;
unsigned fail_write_at = 0;

void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
    bytes[offset + 2] = uint8_t(value >> 16);
    bytes[offset + 3] = uint8_t(value >> 24);
}

std::vector<uint8_t> MakeSyntheticXbe() {
    std::vector<uint8_t> bytes(0x1008, 0);
    std::copy_n("XBEH", 4, bytes.begin());
    Put32(bytes, 0x104, kBase);
    Put32(bytes, 0x108, 0x1000);
    Put32(bytes, 0x10C, 0x3000);
    Put32(bytes, 0x11C, 2);
    Put32(bytes, 0x120, kBase + 0x180);
    Put32(bytes, 0x128, kEntry ^ xbe::kRetailEntryKey);
    Put32(bytes, 0x158, (kBase + 0x280) ^ xbe::kRetailThunkKey);
    Put32(bytes, 0x180, xbe::kExecutable | xbe::kPreload);
    Put32(bytes, 0x184, kEntry);
    Put32(bytes, 0x188, 0x1000);
    Put32(bytes, 0x18C, 0x1000);
    Put32(bytes, 0x190, 4);
    Put32(bytes, 0x194, kBase + 0x240);
    std::copy_n(".text", 6, bytes.begin() + 0x240);
    const size_t second = 0x180 + 56;
    Put32(bytes, second, 0);
    Put32(bytes, second + 4, kSecondSection);
    Put32(bytes, second + 8, 0x1000);
    Put32(bytes, second + 12, 0x1004);
    Put32(bytes, second + 16, 4);
    Put32(bytes, second + 20, kBase + 0x248);
    std::copy_n(".data", 6, bytes.begin() + 0x248);
    bytes[0x1000] = 0x90;
    bytes[0x1001] = 0xC3;
    bytes[0x1002] = 0xCC;
    bytes[0x1003] = 0x90;
    bytes[0x1004] = 0x11;
    bytes[0x1005] = 0x22;
    bytes[0x1006] = 0x33;
    bytes[0x1007] = 0x44;
    return bytes;
}

bool Parse(std::vector<uint8_t> bytes, xbe::Image& image) {
    std::string error;
    bool parsed = image.Parse(std::move(bytes), error);
    if (!parsed) std::printf("expected parser failure: %s\n", error.c_str());
    return parsed;
}

mem::AccessStatus FailInjectedWrite(uint32_t va, const void* source, size_t size) {
    if (++injected_write_count == fail_write_at) return mem::AccessStatus::kHostError;
    return mem::Write(va, source, size);
}

int kernel_data_write_count = 0;
int fail_kernel_data_write_at = 0;

mem::AccessStatus FailKernelDataWrite(uint32_t va, const void* source, size_t size) {
    const mem::AccessStatus status = mem::Write(va, source, size);
    return status == mem::AccessStatus::kOk && ++kernel_data_write_count == fail_kernel_data_write_at
               ? mem::AccessStatus::kHostError
               : status;
}

int thunk_write_count = 0;
int fail_thunk_write_at = 0;

mem::AccessStatus FailPreparationThunkWrite(uint32_t va, const void* source, size_t size) {
    const mem::AccessStatus status = mem::Write(va, source, size);
    return status == mem::AccessStatus::kOk && ++thunk_write_count == fail_thunk_write_at
               ? mem::AccessStatus::kHostError
               : status;
}

std::string Sha1Hex(const std::vector<uint8_t>& bytes) {
    alignas(8) uint8_t state[128];
    uint8_t digest[20];
    crypto::ShaInit(state);
    crypto::ShaUpdate(state, bytes.data(), bytes.size());
    crypto::ShaFinal(state, digest);
    std::string result;
    result.reserve(40);
    for (uint8_t byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}

bool ResolvePreparationThunk(uint32_t ordinal, uint32_t& address, bool& implemented, void*) {
    if (ordinal == 2) {
        address = 0x81234000;
        implemented = true;
        return true;
    }
    if (ordinal == 3) {
        address = 0xFE00000C;
        implemented = false;
        return true;
    }
    return false;
}

void PreparationGeneratedStub(X86Context&, uint8_t*) {}

struct PreparationTempRoot {
    std::filesystem::path path;
    ~PreparationTempRoot() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    bool Create() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("mostwanted-kernel-prep-" + std::to_string(stamp));
        std::error_code error;
        return std::filesystem::create_directories(path / "game", error) && !error;
    }
};

void CheckParserFailures(const std::vector<uint8_t>& valid) {
    xbe::Image image;
    auto bytes = valid;
    bytes.resize(0x100);
    Check(!Parse(std::move(bytes), image), "truncated XBE header rejected");

    bytes = valid;
    Put32(bytes, 0x11C, 0xFFFFFFFF);
    Check(!Parse(std::move(bytes), image), "out-of-bounds section table rejected");

    bytes = valid;
    Put32(bytes, 0x18C, uint32_t(bytes.size() - 1));
    Put32(bytes, 0x190, 2);
    Check(!Parse(std::move(bytes), image), "truncated raw section rejected");

    bytes = valid;
    Put32(bytes, 0x18C, 0xFFFFFFF0);
    Put32(bytes, 0x190, 8);
    Check(!Parse(std::move(bytes), image), "invalid overflowing file offset rejected");

    bytes = valid;
    Put32(bytes, 0x10C, 0xFFFFFFFF);
    Check(!Parse(std::move(bytes), image), "XBE image address overflow rejected");

    bytes = valid;
    Put32(bytes, 0x10C, 0xFFFF0000);
    Put32(bytes, 0x184, 0xFFFFFFF0);
    Put32(bytes, 0x188, 0x40);
    Check(!Parse(std::move(bytes), image), "XBE section address overflow rejected");

    bytes = valid;
    Put32(bytes, 0x11C, 2);
    const size_t second = 0x180 + 56;
    Put32(bytes, second + 4, 0x11800);
    Put32(bytes, second + 8, 0x800);
    Put32(bytes, second + 12, uint32_t(bytes.size()));
    Put32(bytes, second + 16, 0);
    Check(!Parse(std::move(bytes), image), "overlapping virtual sections rejected");

    bytes = valid;
    std::fill(bytes.begin() + 0x280, bytes.begin() + 0x1000, 0xFF);
    Check(!Parse(std::move(bytes), image), "unterminated kernel thunk table rejected");

    bytes = valid;
    Put32(bytes, 0x128, kEntry ^ xbe::kRetailEntryKey);
    Put32(bytes, 0x12C, kBase + 0x300);
    Put32(bytes, 0x300, kEntry);
    Put32(bytes, 0x304, kEntry);
    Put32(bytes, 0x308, 0);
    Put32(bytes, 0x310, UINT32_MAX);
    Check(!Parse(std::move(bytes), image), "XBE TLS size overflow rejected");
}

void CheckPreparedImage(const xbe::Image& image, const std::vector<uint8_t>& source) {
    std::string error;
    std::vector<uint8_t> header(image.header_size);
    Check(mem::Read(image.base, header.data(), header.size()) == mem::AccessStatus::kOk,
          "prepared XBE header readable through checked memory");
    const uint32_t table = image.U32(0x120);
    for (size_t i = 0; i < image.header_size; ++i) {
        bool refcount = false;
        for (size_t n = 0; n < image.sections.size(); ++n)
            if ((image.sections[n].flags & xbe::kPreload) && i >= table - image.base + n * 56 + 0x18 &&
                i < table - image.base + n * 56 + 0x1C)
                refcount = true;
        if (!refcount && header[i] != source[i]) {
            Check(false, "prepared header matches source outside loaded reference counts");
            return;
        }
    }
    Check(true, "prepared header matches source outside loaded reference counts");

    for (size_t i = 0; i < image.sections.size(); ++i) {
        const auto& section = image.sections[i];
        if (!(section.flags & xbe::kPreload)) continue;
        std::vector<uint8_t> bytes(section.vsize);
        const uint32_t ref_va = table + uint32_t(i * 56) + 0x18;
        uint32_t refcount = 0;
        bool okay = mem::Read(section.va, bytes.data(), bytes.size()) == mem::AccessStatus::kOk &&
                    mem::Read(ref_va, &refcount, sizeof(refcount)) == mem::AccessStatus::kOk && refcount == 1 &&
                    std::equal(bytes.begin(), bytes.begin() + section.rsize, source.begin() + section.raw) &&
                    std::all_of(bytes.begin() + section.rsize, bytes.end(), [](uint8_t value) { return value == 0; });
        Check(okay, "preloaded section bytes, virtual zero tail, and reference count verified");
    }
}

void CheckSectionLoadRollback(const xbe::Image& image, const std::vector<uint8_t>& source) {
    const uint32_t table = image.U32(0x120);
    const uint32_t section_header = table + 56;
    std::string error;
    uint32_t refs = UINT32_MAX;
    auto fails_without_leak = [&](unsigned fail_on, const char* operation, const char* test_name) {
        bool decommitted = mem::Decommit(kSecondSection, 0x1000) && !mem::IsCommitted(kSecondSection);
        injected_write_count = 0;
        fail_write_at = fail_on;
        const xbe::SectionStatus status = xbe::LoadSection(image, section_header, error, FailInjectedWrite);
        fail_write_at = 0;
        Check(decommitted && status == xbe::SectionStatus::kAccessFailure && error.find(operation) != std::string::npos,
              test_name);
        const bool clean = !mem::IsCommitted(kSecondSection) && mem::IsCommitted(kEntry) &&
                           mem::Read(section_header + 0x18, &refs, sizeof(refs)) == mem::AccessStatus::kOk && refs == 0;
        Check(clean, "failed section load rolls back new guest page and leaves reference count zero");
        std::array<uint8_t, 4> preserved{};
        Check(mem::Read(kEntry, preserved.data(), preserved.size()) == mem::AccessStatus::kOk &&
                  std::equal(preserved.begin(), preserved.end(), source.begin() + 0x1000),
              "section rollback preserves neighboring preloaded bytes");
    };

    fails_without_leak(1, "section write", "failed section data write reports error and rolls back");
    fails_without_leak(2, "section reference-count write", "failed section reference-count write reports and rolls back");

    Check(xbe::LoadSection(image, section_header, error) == xbe::SectionStatus::kOk,
          "valid non-preloaded section loads through checked memory");
    std::vector<uint8_t> loaded(0x1000, 0xFF);
    bool valid_loaded = mem::Read(kSecondSection, loaded.data(), loaded.size()) == mem::AccessStatus::kOk &&
                        std::equal(loaded.begin(), loaded.begin() + 4, source.begin() + 0x1004) &&
                        std::all_of(loaded.begin() + 4, loaded.end(), [](uint8_t value) { return value == 0; }) &&
                        mem::Read(section_header + 0x18, &refs, sizeof(refs)) == mem::AccessStatus::kOk && refs == 1;
    Check(valid_loaded, "loaded section bytes, zero tail, and reference count verified");
    Check(xbe::LoadSection(image, section_header, error) == xbe::SectionStatus::kOk &&
              mem::Read(section_header + 0x18, &refs, sizeof(refs)) == mem::AccessStatus::kOk && refs == 2,
          "repeated section load increments its checked reference count");
    Check(xbe::UnloadSection(image, section_header, error) == xbe::SectionStatus::kOk &&
              xbe::UnloadSection(image, section_header, error) == xbe::SectionStatus::kOk &&
              mem::Read(section_header + 0x18, &refs, sizeof(refs)) == mem::AccessStatus::kOk && refs == 0,
          "section unload decrements its checked reference count");
}

void CheckSafePreparationCheckpoint(const std::vector<uint8_t>& bytes) {
    const mem::Stats before = mem::Query();
    const auto same_resources = [&](const mem::Stats& after) {
        return after.total_pages == before.total_pages && after.available_pages == before.available_pages &&
               after.virtual_committed == before.virtual_committed && after.contiguous_used == before.contiguous_used &&
               after.system_used == before.system_used;
    };
    bool all_repeated_passes_clean = true;

    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uint8_t> imports = bytes;
        Put32(imports, 0x280, 0x80000002);
        Put32(imports, 0x284, 0x80000003);
        const std::string sha1 = Sha1Hex(imports);
        xbe::Image image;
        std::string error;
        if (!image.Parse(imports, error)) {
            Check(false, "synthetic preparation XBE with imports parses");
            return;
        }

        injected_write_count = 0;
        fail_write_at = 3;
        const bool image_rolled_back =
            !xbe::PrepareImage(image, kEntry, sha1, error, FailInjectedWrite) && !mem::IsCommitted(kBase);
        fail_write_at = 0;
        Check(image_rolled_back, "synthetic preparation rolls back an injected XBE write failure");
        if (!xbe::PrepareImage(image, kEntry, sha1, error)) {
            Check(false, "synthetic XBE prepares through checked writes after rollback");
            return;
        }

        constexpr uint32_t data_base = 0x70000;
        const kernel::KernelDataLayout layout{data_base, data_base + 0x100, data_base + 0x200, data_base + 0x300,
                                              data_base + 0x400, data_base + 0x500, data_base + 0x600,
                                              data_base + 0x700, data_base + 0x800, data_base + 0x900,
                                              data_base + 0xA00};
        std::array<uint8_t, mem::kPage> kernel_data_before{}, kernel_data_after{};
        bool kernel_data_ready = mem::Commit(data_base, mem::kPage) &&
                                 mem::Fill(data_base, 0xA5, mem::kPage) == mem::AccessStatus::kOk &&
                                 mem::Read(data_base, kernel_data_before.data(), kernel_data_before.size()) ==
                                     mem::AccessStatus::kOk;
        kernel_data_write_count = 0;
        fail_kernel_data_write_at = 4;
        const kernel::KernelDataMemory failed_data_write{nullptr, FailKernelDataWrite};
        const bool data_rolled_back = kernel_data_ready && !kernel::InitializeKernelData(layout, error, failed_data_write) &&
                                      mem::Read(data_base, kernel_data_after.data(), kernel_data_after.size()) ==
                                          mem::AccessStatus::kOk &&
                                      kernel_data_after == kernel_data_before && !kernel::KernelClockWorkerRunning();
        fail_kernel_data_write_at = 0;
        Check(data_rolled_back, "kernel-data write failure rolls back without starting the clock");
        const bool data_initialized = kernel::InitializeKernelData(layout, error) && !kernel::KernelClockWorkerRunning();
        Check(data_initialized, "kernel data initializes synchronously without starting the clock worker");

        const std::array<X86FuncMapping, 1> mappings = {{{kEntry, PreparationGeneratedStub}}};
        const std::array<uint32_t, 1> continuations = {kEntry + 1};
        dispatch::Table table;
        const dispatch::Metadata metadata{kEntry, kEntry + 4, kEntry, mappings, continuations};
        const bool dispatch_ready = table.Initialize(metadata, error) && table.Find(kEntry) == PreparationGeneratedStub &&
                                    table.IsContinuation(kEntry + 1);
        Check(dispatch_ready, "synthetic dispatch metadata initializes without invoking mapped functions");

        const std::array<uint32_t, 3> original_thunks = {0x80000002, 0x80000003, 0};
        std::array<uint32_t, 3> thunk_state{};
        const bool thunk_table_ready = mem::Read(image.kernel_thunk, thunk_state.data(), sizeof(thunk_state)) ==
                                           mem::AccessStatus::kOk &&
                                       thunk_state == original_thunks;
        thunk_write_count = 0;
        fail_thunk_write_at = 2;
        const kernel::ThunkMemory failed_thunk_write{nullptr, FailPreparationThunkWrite};
        std::vector<uint32_t> missing;
        const bool thunks_rolled_back = thunk_table_ready &&
                                        !kernel::PatchKernelThunks(image, ResolvePreparationThunk, nullptr, missing,
                                                                   error, failed_thunk_write) &&
                                        mem::Read(image.kernel_thunk, thunk_state.data(), sizeof(thunk_state)) ==
                                            mem::AccessStatus::kOk &&
                                        thunk_state == original_thunks;
        fail_thunk_write_at = 0;
        Check(thunks_rolled_back, "kernel thunk patch failure restores every synthetic import slot");
        const bool thunks_patched = kernel::PatchKernelThunks(image, ResolvePreparationThunk, nullptr, missing, error) &&
                                    mem::Read(image.kernel_thunk, thunk_state.data(), sizeof(thunk_state)) ==
                                        mem::AccessStatus::kOk &&
                                    thunk_state == std::array<uint32_t, 3>{0x81234000, 0xFE00000C, 0} &&
                                    missing == std::vector<uint32_t>{3};
        Check(thunks_patched, "synthetic imports resolve data and function ordinals without execution");

        PreparationTempRoot temp;
        const bool temp_ready = temp.Create();
        std::ofstream(temp.path / "game" / "Prepared.Xbe") << "synthetic";
        kernel::filesystem::Context filesystem;
        const std::filesystem::path data_dir = temp.path / "data";
        const bool filesystem_ready = temp_ready &&
                                      filesystem.MountDefaults((temp.path / "game").string(), data_dir.string(), error);
        kernel::filesystem::ResolvedPath resolved;
        const bool filesystem_mapped = filesystem_ready &&
                                       filesystem.ResolvePath("D:\\prepared.xbe", resolved, error) &&
                                       std::filesystem::weakly_canonical(resolved.host) ==
                                           std::filesystem::weakly_canonical(temp.path / "game" / "Prepared.Xbe");
        Check(filesystem_mapped, "temporary filesystem mappings resolve only inside the test root");

        const uint32_t kpcr = mem::AllocSystem(0x400, 64);
        const uint32_t ethread = mem::AllocSystem(0x200, 64);
        const uint32_t stack = mem::AllocStack(0x20000);
        const uint32_t tls = stack ? stack - 0x100 : 0;
        const mem::AccessStatus thread_status = kpcr && ethread && stack
                                                    ? kernel::InitializeThreadGuestMemory(kpcr, ethread, stack, 0x10000,
                                                                                         tls, 0x44)
                                                    : mem::AccessStatus::kHostError;
        std::array<uint8_t, 0x400> kpcr_bytes{};
        std::array<uint8_t, 0x200> ethread_bytes{};
        const bool thread_memory_ready = thread_status == mem::AccessStatus::kOk &&
                                         mem::Read(kpcr, kpcr_bytes.data(), kpcr_bytes.size()) == mem::AccessStatus::kOk &&
                                         mem::Read(ethread, ethread_bytes.data(), ethread_bytes.size()) ==
                                             mem::AccessStatus::kOk;
        uint32_t kpcr_self = 0, ethread_tls = 0;
        std::memcpy(&kpcr_self, kpcr_bytes.data() + 0x18, sizeof(kpcr_self));
        std::memcpy(&ethread_tls, ethread_bytes.data() + 0x28, sizeof(ethread_tls));
        const bool thread_ready = thread_memory_ready && kpcr_self == kpcr && ethread_tls == tls &&
                                  !kernel::KernelClockWorkerRunning();
        Check(thread_ready, "KPCR, ETHREAD, stack, and TLS prepare without starting a guest thread");

        filesystem.Clear();
        const bool mappings_cleared = filesystem.QuerySymbolicLink("\\??\\D:").empty() &&
                                      !filesystem.ResolvePath("D:\\prepared.xbe", resolved, error);
        if (stack) mem::FreeStack(stack);
        if (ethread) mem::FreeSystem(ethread);
        if (kpcr) mem::FreeSystem(kpcr);
        table.Reset();
        const bool image_released = mem::FreeVirtual(image.base, image.image_size, mem::kMemRelease) == 0;
        const bool data_released = mem::Decommit(data_base, mem::kPage);
        const mem::Stats after = mem::Query();
        all_repeated_passes_clean &= mappings_cleared && image_released && data_released && same_resources(after) &&
                                     !mem::IsCommitted(kBase) && !kernel::KernelClockWorkerRunning();
        Check(mappings_cleared, "synthetic preparation clears every scoped filesystem mapping");
    }

    Check(all_repeated_passes_clean, "repeated preparation releases every guest and host resource");
}

bool ReadFile(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), {});
    return bool(file) || file.eof();
}

void CheckRealGameXbe(const char* path) {
    if (!path) return;
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        Check(false, "real XBE file is readable");
        return;
    }
    Check(true, "real XBE file is readable");
    xbe::Image image;
    std::string error;
    if (!image.Parse(bytes, error)) {
        std::printf("real XBE parse failed: %s\n", error.c_str());
        Check(false, "real Black Edition XBE parses");
        return;
    }
    const size_t preloaded = size_t(std::count_if(image.sections.begin(), image.sections.end(),
                                                  [](const xbe::Section& section) { return section.flags & xbe::kPreload; }));
    Check(image.title_id == 0x4541007B && image.entry == kGameEntry && image.sections.size() == 15 && preloaded == 12,
          "real XBE identity, entry point, and 15/12 section counts verified");
    if (!xbe::PrepareImage(image, kGameEntry, kGameSha1, error)) {
        std::printf("real XBE preparation failed: %s\n", error.c_str());
        Check(false, "real XBE prepared through checked guest writes");
        return;
    }
    Check(true, "real XBE prepared through checked guest writes without execution");
    CheckPreparedImage(image, bytes);
    Check(mem::IsCommitted(image.entry), "real entry point resides in prepared guest memory");
    Check(mem::FreeVirtual(image.base, image.image_size, mem::kMemRelease) == 0,
          "real XBE reservation released after consistency checks");
}

}  // namespace

extern "C" int RunXbeImageLoaderTests(const char* real_xbe_path) {
    if (!mem::Init()) {
        Check(false, "guest memory initializes for image-loader tests");
        return 1;
    }
    Check(true, "guest memory initializes for image-loader tests");
    const std::vector<uint8_t> bytes = MakeSyntheticXbe();
    CheckSafePreparationCheckpoint(bytes);
    xbe::Image image;
    std::string error;
    Check(image.Parse(bytes, error), "synthetic XBE parses");
    CheckParserFailures(bytes);
    if (image.sections.size() == 2) {
        Check(!xbe::PrepareImage(image, kEntry, "0000000000000000000000000000000000000000", error) &&
                  !mem::IsCommitted(kBase),
              "identity mismatch rejects image before reservation");
        Check(!xbe::PrepareImage(image, kEntry + 1, kSyntheticSha1, error) && !mem::IsCommitted(kBase),
              "entry mismatch rejects image before reservation");
        Check(xbe::PrepareImage(image, kEntry, kSyntheticSha1, error), "synthetic XBE prepared through checked writes");
        CheckPreparedImage(image, bytes);
        CheckSectionLoadRollback(image, bytes);
        Check(mem::Decommit(kSecondSection, 0x1000), "synthetic non-preloaded section page decommits for cleanup");
        Check(mem::FreeVirtual(image.base, image.image_size, mem::kMemRelease) == 0 && !mem::IsCommitted(kBase),
              "successful synthetic image can be released");

        injected_write_count = 0;
        fail_write_at = 3;
        const bool image_rollback = !xbe::PrepareImage(image, kEntry, kSyntheticSha1, error, FailInjectedWrite);
        fail_write_at = 0;
        Check(image_rollback &&
                  error.find("section reference-count write") != std::string::npos && !mem::IsCommitted(kBase),
              "partial image write failure rolls back reservation and committed pages");
    }
    CheckRealGameXbe(real_xbe_path);
    mem::Shutdown();
    return failures ? 1 : 0;
}
