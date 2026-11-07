#include "xbe/image_loader.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "kernel/crypto.h"

namespace xbe {
namespace {

std::string Sha1Hex(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t context[128];
    uint8_t digest[20];
    crypto::ShaInit(context);
    crypto::ShaUpdate(context, data.data(), data.size());
    crypto::ShaFinal(context, digest);
    std::string hex;
    hex.reserve(40);
    for (uint8_t byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}

bool IsSha1(std::string_view value) {
    if (value.size() != 40) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool Release(uint32_t base) {
    uint32_t size = 0;
    return mem::FreeVirtual(base, size, mem::kMemRelease) == 0;
}

const Section* SectionAt(const Image& image, uint32_t header) {
    const uint32_t table = image.U32(0x120);
    if (header < table) return nullptr;
    const uint32_t offset = header - table;
    if (offset % 56 || offset / 56 >= image.sections.size()) return nullptr;
    if (uint64_t(header) + 0x1C > uint64_t(image.base) + image.header_size) return nullptr;
    return &image.sections[offset / 56];
}

std::vector<uint32_t> UncommittedPages(const Section& section) {
    std::vector<uint32_t> pages;
    if (!section.vsize) return pages;
    const uint32_t first = section.va & ~(mem::kPage - 1);
    const uint64_t end = (uint64_t(section.va) + section.vsize + mem::kPage - 1) & ~(uint64_t(mem::kPage) - 1);
    for (uint64_t page = first; page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) pages.push_back(uint32_t(page));
    return pages;
}

bool RollbackSection(const Section& section, const std::vector<uint32_t>& new_pages, std::string& error) {
    bool okay = mem::Fill(section.va, 0, section.vsize) == mem::AccessStatus::kOk;
    for (uint32_t page : new_pages)
        if (!mem::Decommit(page, mem::kPage)) okay = false;
    if (!okay) error += "; section rollback incomplete";
    return okay;
}

std::string AccessError(const char* operation, uint32_t va, mem::AccessStatus status) {
    char message[128];
    std::snprintf(message, sizeof(message), "XBE %s at %08X failed: %s", operation, va, mem::StatusName(status));
    return message;
}

}  // namespace

bool PrepareImage(const Image& image, uint32_t expected_entry, std::string_view expected_sha1, std::string& error,
                  CheckedWrite checked_write) {
    if (expected_entry != image.entry) {
        char message[96];
        std::snprintf(message, sizeof(message), "XBE entry %08X does not match expected entry %08X", image.entry,
                      expected_entry);
        error = message;
        return false;
    }
    if (!IsSha1(expected_sha1)) {
        error = "expected XBE SHA-1 must be 40 lowercase hexadecimal characters";
        return false;
    }
    const std::string actual_sha1 = Sha1Hex(image.file);
    if (actual_sha1 != expected_sha1) {
        error = "XBE SHA-1 mismatch: expected " + std::string(expected_sha1) + ", got " + actual_sha1;
        return false;
    }
    mem::ScopedVirtualRangeLock range_lock;
    mem::ScopedLock memory_lock;
    if (!mem::ReserveImage(image.base, image.image_size)) {
        char message[96];
        std::snprintf(message, sizeof(message), "could not reserve XBE image at %08X+%X", image.base, image.image_size);
        error = message;
        return false;
    }

    auto write = checked_write ? checked_write : &mem::Write;
    auto fail = [&](const char* operation, uint32_t va, mem::AccessStatus status) {
        error = AccessError(operation, va, status);
        memory_lock.Unlock();
        if (!Release(image.base)) error += "; XBE image rollback failed";
        memory_lock.Lock();
        return false;
    };
    mem::AccessStatus status = write(image.base, image.file.data(), image.header_size);
    if (status != mem::AccessStatus::kOk) return fail("header write", image.base, status);

    const uint32_t section_table = image.U32(0x120);
    for (size_t i = 0; i < image.sections.size(); ++i) {
        const Section& section = image.sections[i];
        if (!(section.flags & kPreload)) continue;
        if (section.rsize) {
            status = write(section.va, image.file.data() + section.raw, section.rsize);
            if (status != mem::AccessStatus::kOk) return fail("section write", section.va, status);
        }
        if (section.vsize > section.rsize) {
            status = mem::Fill(section.va + section.rsize, 0, section.vsize - section.rsize);
            if (status != mem::AccessStatus::kOk) return fail("section zero-fill", section.va + section.rsize, status);
        }
        const uint64_t reference_va = uint64_t(section_table) + i * 56 + 0x18;
        const uint32_t reference_count = 1;
        status = write(uint32_t(reference_va), &reference_count, sizeof(reference_count));
        if (status != mem::AccessStatus::kOk)
            return fail("section reference-count write", uint32_t(reference_va), status);
    }
    error.clear();
    return true;
}

SectionStatus LoadSection(const Image& image, uint32_t section_header, std::string& error, CheckedWrite checked_write) {
    mem::ScopedVirtualRangeLock range_lock;
    const Section* section = SectionAt(image, section_header);
    if (!section) {
        error = "invalid XBE section header";
        return SectionStatus::kInvalidSection;
    }

    uint32_t refs = 0;
    auto write = checked_write ? checked_write : &mem::Write;
    mem::AccessStatus access = mem::Read(section_header + 0x18, &refs, sizeof(refs));
    if (access != mem::AccessStatus::kOk) {
        error = AccessError("section reference-count read", section_header + 0x18, access);
        return SectionStatus::kAccessFailure;
    }
    if (refs == UINT32_MAX) {
        error = "XBE section reference count overflow";
        return SectionStatus::kInvalidSection;
    }

    if (refs) {
        const uint32_t next_refs = refs + 1;
        access = write(section_header + 0x18, &next_refs, sizeof(next_refs));
        if (access != mem::AccessStatus::kOk) {
            error = AccessError("section reference-count write", section_header + 0x18, access);
            return SectionStatus::kAccessFailure;
        }
        error.clear();
        return SectionStatus::kOk;
    }

    const std::vector<uint32_t> new_pages = UncommittedPages(*section);
    if (!mem::Commit(section->va, section->vsize)) {
        error = "could not commit XBE section at " + std::to_string(section->va);
        for (uint32_t page : new_pages)
            if (!mem::Decommit(page, mem::kPage)) error += "; section rollback incomplete";
        return SectionStatus::kNoMemory;
    }

    access = mem::Fill(section->va, 0, section->vsize);
    if (access != mem::AccessStatus::kOk) {
        error = AccessError("section zero-fill", section->va, access);
        RollbackSection(*section, new_pages, error);
        return SectionStatus::kAccessFailure;
    }
    if (section->rsize) {
        access = write(section->va, image.file.data() + section->raw, section->rsize);
        if (access != mem::AccessStatus::kOk) {
            error = AccessError("section write", section->va, access);
            RollbackSection(*section, new_pages, error);
            return SectionStatus::kAccessFailure;
        }
    }

    const uint32_t next_refs = 1;
    access = write(section_header + 0x18, &next_refs, sizeof(next_refs));
    if (access != mem::AccessStatus::kOk) {
        error = AccessError("section reference-count write", section_header + 0x18, access);
        RollbackSection(*section, new_pages, error);
        return SectionStatus::kAccessFailure;
    }
    error.clear();
    return SectionStatus::kOk;
}

SectionStatus UnloadSection(const Image& image, uint32_t section_header, std::string& error, CheckedWrite checked_write) {
    mem::ScopedVirtualRangeLock range_lock;
    if (!SectionAt(image, section_header)) {
        error = "invalid XBE section header";
        return SectionStatus::kInvalidSection;
    }

    uint32_t refs = 0;
    auto write = checked_write ? checked_write : &mem::Write;
    mem::AccessStatus access = mem::Read(section_header + 0x18, &refs, sizeof(refs));
    if (access != mem::AccessStatus::kOk) {
        error = AccessError("section reference-count read", section_header + 0x18, access);
        return SectionStatus::kAccessFailure;
    }
    if (!refs) {
        error = "XBE section reference count underflow";
        return SectionStatus::kInvalidSection;
    }
    const uint32_t next_refs = refs - 1;
    access = write(section_header + 0x18, &next_refs, sizeof(next_refs));
    if (access != mem::AccessStatus::kOk) {
        error = AccessError("section reference-count write", section_header + 0x18, access);
        return SectionStatus::kAccessFailure;
    }
    error.clear();
    return SectionStatus::kOk;
}

}  // namespace xbe
