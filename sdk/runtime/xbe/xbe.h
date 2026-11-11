#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace xbe {

// the header XORs the entry point and kernel thunk address with these
constexpr uint32_t kRetailEntryKey = 0xA8FC57AB;
constexpr uint32_t kDebugEntryKey = 0x94859D4B;
constexpr uint32_t kRetailThunkKey = 0x5B6D40B6;
constexpr uint32_t kDebugThunkKey = 0xEFB1F152;

enum SectionFlags : uint32_t {
    kWritable = 1,
    kPreload = 2,
    kExecutable = 4,
    kInsertedFile = 8,
};

struct Section {
    std::string name;
    uint32_t flags = 0;
    uint32_t va = 0, vsize = 0;
    uint32_t raw = 0, rsize = 0;
};

struct Library {
    std::string name;
    uint16_t major = 0, minor = 0, build = 0, flags = 0;
};

struct Tls {
    uint32_t data_start = 0, data_end = 0, index_addr = 0, callbacks = 0, zero_fill = 0, characteristics = 0;
};

// One dword per module index in front of the initialisation data.
inline constexpr uint32_t kTlsSlotBytes = 4;

// The CRT's own computation (002B3916): align16(zero_fill + data_end - data_start)
// plus the index slot. The loader must agree with it, or the negative index the
// CRT stores at the directory's index address will not address the block.
inline uint32_t TlsBlockSize(const Tls& tls) {
    if (tls.data_end < tls.data_start) return 0;
    const uint64_t body = uint64_t(tls.data_end - tls.data_start) + tls.zero_fill;
    if (body > UINT32_MAX - 15 - kTlsSlotBytes) return 0;
    return uint32_t((body + 15) & ~uint64_t(15)) + kTlsSlotBytes;
}

struct Image {
    std::vector<uint8_t> file;
    uint32_t base = 0, header_size = 0, image_size = 0;
    uint32_t entry = 0, kernel_thunk = 0, tls_addr = 0;
    uint32_t stack_commit = 0, heap_reserve = 0, heap_commit = 0;
    uint32_t cert = 0, title_id = 0, init_flags = 0;
    bool debug = false;
    std::vector<Section> sections;
    std::vector<Library> libraries;
    Tls tls;

    uint32_t U32(uint32_t off) const {
        uint32_t value = 0;
        if (uint64_t(off) + sizeof(value) <= file.size()) std::memcpy(&value, file.data() + off, sizeof(value));
        return value;
    }

    // file offset for a virtual address, -1 if unmapped or in the zero-filled tail
    int64_t Offset(uint32_t va) const {
        if (va >= base && uint64_t(va) < uint64_t(base) + header_size) return int64_t(va - base);
        for (const auto& section : sections)
            if (va >= section.va && uint64_t(va) < uint64_t(section.va) + section.rsize)
                return int64_t(section.raw) + (va - section.va);
        return -1;
    }

    size_t FileSpan(uint32_t va) const {
        if (va >= base && uint64_t(va) < uint64_t(base) + header_size)
            return size_t(uint64_t(base) + header_size - va);
        for (const auto& section : sections)
            if (va >= section.va && uint64_t(va) < uint64_t(section.va) + section.rsize)
                return section.rsize - (va - section.va);
        return 0;
    }

    const Section* SectionAt(uint32_t va) const {
        for (const auto& section : sections)
            if (va >= section.va && uint64_t(va) < uint64_t(section.va) + section.vsize) return &section;
        return nullptr;
    }

    bool Read32(uint32_t va, uint32_t& out) const {
        int64_t offset = Offset(va);
        if (offset < 0 || FileSpan(va) < sizeof(out)) return false;
        std::memcpy(&out, file.data() + size_t(offset), sizeof(out));
        return true;
    }

    std::string CStr(uint32_t va) const {
        int64_t offset = Offset(va);
        if (offset < 0) return {};
        size_t span = FileSpan(va);
        const auto* start = file.data() + size_t(offset);
        const auto* end = static_cast<const uint8_t*>(std::memchr(start, 0, span));
        return end ? std::string(reinterpret_cast<const char*>(start), size_t(end - start)) : std::string{};
    }

    bool Parse(std::vector<uint8_t> data, std::string& error) {
        *this = Image{};
        file = std::move(data);
        auto fail = [&](const char* message) {
            error = message;
            return false;
        };
        auto read32 = [&](uint64_t offset, uint32_t& value) {
            if (offset + sizeof(value) > file.size()) return false;
            std::memcpy(&value, file.data() + offset, sizeof(value));
            return true;
        };
        auto read16 = [&](uint64_t offset, uint16_t& value) {
            if (offset + sizeof(value) > file.size()) return false;
            std::memcpy(&value, file.data() + offset, sizeof(value));
            return true;
        };
        if (file.size() < 0x180 || std::memcmp(file.data(), "XBEH", 4)) return fail("not an XBE");

        uint32_t ep = 0, kt = 0, nsec = 0, sec_addr = 0;
        if (!read32(0x104, base) || !read32(0x108, header_size) || !read32(0x10C, image_size) ||
            !read32(0x118, cert) || !read32(0x11C, nsec) || !read32(0x120, sec_addr) ||
            !read32(0x124, init_flags) || !read32(0x128, ep) || !read32(0x12C, tls_addr) ||
            !read32(0x130, stack_commit) || !read32(0x134, heap_reserve) || !read32(0x138, heap_commit) ||
            !read32(0x158, kt))
            return fail("truncated XBE header");

        const uint64_t image_end = uint64_t(base) + image_size;
        const uint64_t header_end = uint64_t(base) + header_size;
        if (!base || !image_size || image_end > (uint64_t(UINT32_MAX) + 1) || header_size < 0x180 ||
            header_size > file.size() || header_size > image_size)
            return fail("invalid XBE image bounds");
        auto inside_image = [&](uint32_t value) { return value >= base && uint64_t(value) < image_end; };
        auto decode = [&](uint32_t encoded, uint32_t entry_key, uint32_t thunk_key) {
            uint32_t decoded_entry = encoded ^ entry_key;
            uint32_t decoded_thunk = kt ^ thunk_key;
            return inside_image(decoded_entry) && inside_image(decoded_thunk);
        };
        if (decode(ep, kRetailEntryKey, kRetailThunkKey)) {
            entry = ep ^ kRetailEntryKey;
            kernel_thunk = kt ^ kRetailThunkKey;
        } else if (decode(ep, kDebugEntryKey, kDebugThunkKey)) {
            entry = ep ^ kDebugEntryKey;
            kernel_thunk = kt ^ kDebugThunkKey;
            debug = true;
        } else {
            return fail("entry point key not recognised or address outside image");
        }
        if (kernel_thunk & 3u) return fail("unaligned kernel thunk address");

        const uint64_t section_table_size = uint64_t(nsec) * 56;
        if (!nsec || sec_addr < base || uint64_t(sec_addr) + section_table_size > header_end ||
            uint64_t(sec_addr - base) + section_table_size > file.size())
            return fail("section table outside XBE header");

        std::vector<std::pair<uint64_t, uint64_t>> virtual_ranges;
        virtual_ranges.reserve(nsec);
        sections.reserve(nsec);
        const uint32_t section_offset = sec_addr - base;
        for (uint32_t i = 0; i < nsec; ++i) {
            const uint64_t offset = uint64_t(section_offset) + uint64_t(i) * 56;
            Section section;
            uint32_t name = 0;
            if (!read32(offset, section.flags) || !read32(offset + 4, section.va) ||
                !read32(offset + 8, section.vsize) || !read32(offset + 12, section.raw) ||
                !read32(offset + 16, section.rsize) || !read32(offset + 20, name))
                return fail("truncated XBE section table");
            const uint64_t section_end = uint64_t(section.va) + section.vsize;
            const uint64_t raw_end = uint64_t(section.raw) + section.rsize;
            if ((section.vsize && (section.va < header_end || section_end > image_end || section_end > (uint64_t(UINT32_MAX) + 1))) ||
                section.rsize > section.vsize || raw_end > file.size())
                return fail("invalid XBE section bounds");
            if (section.vsize) virtual_ranges.emplace_back(section.va, section_end);
            if (name) {
                if (name < base || uint64_t(name) >= header_end)
                    return fail("XBE section name outside header");
                size_t name_offset = name - base;
                const auto* begin = file.data() + name_offset;
                const size_t span = header_size - name_offset;
                const auto* end = static_cast<const uint8_t*>(std::memchr(begin, 0, span));
                if (!end) return fail("unterminated XBE section name");
                section.name.assign(reinterpret_cast<const char*>(begin), size_t(end - begin));
            }
            sections.push_back(std::move(section));
        }
        std::sort(virtual_ranges.begin(), virtual_ranges.end());
        for (size_t i = 1; i < virtual_ranges.size(); ++i)
            if (virtual_ranges[i].first < virtual_ranges[i - 1].second) return fail("overlapping XBE sections");

        const Section* entry_section = SectionAt(entry);
        if (!entry_section || !(entry_section->flags & kExecutable) || !(entry_section->flags & kPreload))
            return fail("entry point is not in a preloaded executable section");
        const Section* thunk_section = SectionAt(kernel_thunk);
        const bool thunk_in_header = kernel_thunk >= base && uint64_t(kernel_thunk) < header_end;
        if (!thunk_in_header && (!thunk_section || !(thunk_section->flags & kPreload)))
            return fail("kernel thunk table is not in a preloaded region");
        bool thunk_terminated = false;
        const size_t thunk_span = FileSpan(kernel_thunk);
        for (size_t offset = 0; offset + sizeof(uint32_t) <= thunk_span; offset += sizeof(uint32_t)) {
            uint32_t thunk = 0;
            if (!Read32(uint32_t(uint64_t(kernel_thunk) + offset), thunk)) break;
            if (!thunk) {
                thunk_terminated = true;
                break;
            }
        }
        if (!thunk_terminated) return fail("kernel thunk table is not terminated in file-backed data");

        uint32_t nlib = 0, lib_addr = 0;
        if (!read32(0x160, nlib) || !read32(0x164, lib_addr)) return fail("truncated XBE library table header");
        const uint64_t library_bytes = uint64_t(nlib) * 16;
        if (nlib && (lib_addr < base || uint64_t(lib_addr) + library_bytes > header_end ||
                     uint64_t(lib_addr - base) + library_bytes > file.size()))
            return fail("library table outside XBE header");
        libraries.reserve(nlib);
        for (uint32_t i = 0; i < nlib; ++i) {
            const uint64_t offset = uint64_t(lib_addr - base) + uint64_t(i) * 16;
            Library library;
            const char* name = reinterpret_cast<const char*>(file.data() + offset);
            size_t name_size = 0;
            while (name_size < 8 && name[name_size]) ++name_size;
            library.name.assign(name, name_size);
            if (!read16(offset + 8, library.major) || !read16(offset + 10, library.minor) ||
                !read16(offset + 12, library.build) || !read16(offset + 14, library.flags))
                return fail("truncated XBE library table");
            libraries.push_back(std::move(library));
        }

        if (cert) {
            uint32_t title = 0;
            if (cert < base || uint64_t(cert) + 12 > header_end || !read32(uint64_t(cert - base) + 8, title))
                return fail("XBE certificate outside header");
            title_id = title;
        }
        if (tls_addr) {
            int64_t offset = Offset(tls_addr);
            if (offset < 0 || FileSpan(tls_addr) < 24) return fail("XBE TLS directory is not file-backed");
            if (!read32(uint64_t(offset), tls.data_start) || !read32(uint64_t(offset) + 4, tls.data_end) ||
                !read32(uint64_t(offset) + 8, tls.index_addr) || !read32(uint64_t(offset) + 12, tls.callbacks) ||
                !read32(uint64_t(offset) + 16, tls.zero_fill) || !read32(uint64_t(offset) + 20, tls.characteristics))
                return fail("truncated XBE TLS directory");
            if (tls.data_end < tls.data_start) return fail("invalid XBE TLS bounds");
            const uint64_t tls_data_size = uint64_t(tls.data_end) - tls.data_start;
            if (tls_data_size + tls.zero_fill > uint64_t(UINT32_MAX) - 19 ||
                (tls.data_end > tls.data_start &&
                 (!inside_image(tls.data_start) || uint64_t(tls.data_end) > image_end)) ||
                (tls.index_addr && !inside_image(tls.index_addr)) || (tls.callbacks && !inside_image(tls.callbacks)))
                return fail("invalid XBE TLS bounds");
        }
        error.clear();
        return true;
    }
};

}  // namespace xbe
