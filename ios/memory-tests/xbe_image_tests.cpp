#include "kernel/crypto.h"
#include "kernel/memory.h"
#include "kernel/thread_memory.h"
#include "xbe/image_loader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    xbe::Image image;
    std::string error;
    if (!image.Parse(bytes, error) || !xbe::PrepareImage(image, kEntry, kSyntheticSha1, error)) {
        Check(false, "synthetic non-executing preparation parses and prepares the XBE");
        return;
    }
    Check(mem::IsCommitted(image.entry), "synthetic preparation leaves the entry mapped without executing it");

    const uint32_t kpcr = mem::AllocSystem(0x400, 64);
    const uint32_t ethread = mem::AllocSystem(0x200, 64);
    const uint32_t stack = mem::AllocStack(0x20000);
    const uint32_t tls = stack ? stack - 0x100 : 0;
    const auto status = kpcr && ethread && stack
                            ? kernel::InitializeThreadGuestMemory(kpcr, ethread, stack, 0x10000, tls, 0x44)
                            : mem::AccessStatus::kHostError;
    std::array<uint8_t, 0x400> kpcr_bytes{};
    std::array<uint8_t, 0x200> ethread_bytes{};
    bool initialized = status == mem::AccessStatus::kOk &&
                       mem::Read(kpcr, kpcr_bytes.data(), kpcr_bytes.size()) == mem::AccessStatus::kOk &&
                       mem::Read(ethread, ethread_bytes.data(), ethread_bytes.size()) == mem::AccessStatus::kOk;
    uint32_t kpcr_self = 0, kpcr_thread = 0, ethread_stack = 0, ethread_tls = 0, ethread_id = 0;
    std::memcpy(&kpcr_self, kpcr_bytes.data() + 0x18, sizeof(kpcr_self));
    std::memcpy(&kpcr_thread, kpcr_bytes.data() + 0x28, sizeof(kpcr_thread));
    std::memcpy(&ethread_stack, ethread_bytes.data() + 0x1C, sizeof(ethread_stack));
    std::memcpy(&ethread_tls, ethread_bytes.data() + 0x28, sizeof(ethread_tls));
    std::memcpy(&ethread_id, ethread_bytes.data() + 0x12C, sizeof(ethread_id));
    Check(initialized && kpcr_self == kpcr && kpcr_thread == ethread && ethread_bytes[0] == 6 &&
              ethread_bytes[2] == 0x80 && ethread_stack == stack && ethread_tls == tls && ethread_id == 0x44,
          "synthetic preparation initializes KPCR and ETHREAD through checked writes");

    if (stack) mem::FreeStack(stack);
    if (kpcr) mem::FreeSystem(kpcr);
    if (ethread) mem::FreeSystem(ethread);
    const bool image_released = mem::FreeVirtual(image.base, image.image_size, mem::kMemRelease) == 0;
    const mem::Stats after = mem::Query();
    Check(image_released && after.system_used == before.system_used && !mem::IsCommitted(kBase),
          "synthetic preparation releases its image and allocator ownership");
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
