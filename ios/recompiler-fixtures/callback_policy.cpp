#include "callback_policy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include "xbe/xbe.h"

namespace callback_policy {
namespace {

uint32_t Fnv1aWithSeed(const uint8_t* data, size_t size, uint32_t value) {
    for (size_t i = 0; i < size; ++i) value = (value ^ data[i]) * 0x01000193u;
    return value;
}

uint32_t Fnv1a(const uint8_t* data, size_t size) { return Fnv1aWithSeed(data, size, 0x811C9DC5u); }

std::string Hex(uint32_t value) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08X", value);
    return text;
}

bool ParseHex(std::string_view text, uint32_t& value) {
    if (text.empty()) return false;
    std::string copy(text);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(copy.c_str(), &end, 16);
    if (!end || *end) return false;
    value = uint32_t(parsed);
    return true;
}

bool ParseDec(std::string_view text, uint32_t& value) {
    if (text.empty()) return false;
    std::string copy(text);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(copy.c_str(), &end, 10);
    if (!end || *end) return false;
    value = uint32_t(parsed);
    return true;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

bool ParseStatus(std::string_view text, Status& status) {
    if (text == "VERIFIED_STATIC") status = Status::kVerifiedStatic;
    else if (text == "INFERRED") status = Status::kInferred;
    else if (text == "UNRESOLVED") status = Status::kUnresolved;
    else if (text == "PROHIBITED") status = Status::kProhibited;
    else if (text == "INVALID_MAPPING") status = Status::kInvalidMapping;
    else return false;
    return true;
}

bool ParseAbi(std::string_view text, Abi& abi) {
    if (text == "NONE") abi = Abi::kNone;
    else if (text == "CDECL") abi = Abi::kCdecl;
    else if (text == "STDCALL") abi = Abi::kStdcall;
    else if (text == "UNKNOWN") abi = Abi::kUnknown;
    else return false;
    return true;
}

bool ParseFlags(std::string_view text, uint32_t& flags) {
    flags = 0;
    if (text == "-") return true;
    for (std::string_view flag : Split(text, ',')) {
        if (flag == "TRAP") flags |= kTrap;
        else if (flag == "FRAME") flags |= kFrame;
        else if (flag == "ARG_MISMATCH") flags |= kArgMismatch;
        else if (flag == "IND_CALL") flags |= kIndirectCall;
        else if (flag == "IND_WRITE") flags |= kIndirectWrite;
        else if (flag == "ABS_WRITE") flags |= kAbsoluteWrite;
        else if (flag == "IMPORT") flags |= kImport;
        else if (flag == "FS") flags |= kFs;
        else if (flag == "TAIL") flags |= kTail;
        else if (flag == "RECHECK") flags |= kRecheck;
        else return false;
    }
    return true;
}

bool ParseList(const std::string& text, std::vector<uint32_t>& values) {
    values.clear();
    if (text == "-") return true;
    for (std::string_view part : Split(text, ',')) {
        uint32_t value = 0;
        if (!ParseHex(part, value)) return false;
        values.push_back(value);
    }
    return true;
}

const Table* FindTable(const Manifest& manifest, char id) {
    for (const Table& table : manifest.tables)
        if (table.id == id) return &table;
    return nullptr;
}

uint32_t Fingerprint(const xbe::Image& image, uint32_t address) {
    const int64_t offset = image.Offset(address);
    const size_t span = image.FileSpan(address);
    if (offset < 0 || !span) return 0;
    return Fnv1a(image.file.data() + size_t(offset), std::min<size_t>(span, 16));
}

bool TableDigest(const xbe::Image& image, uint32_t base, uint32_t count, uint32_t& digest) {
    const int64_t offset = image.Offset(base);
    const size_t size = size_t(count) * 4;
    if (offset < 0 || image.FileSpan(base) < size) return false;
    digest = Fnv1a(image.file.data() + size_t(offset), size);
    return true;
}

bool ByteFingerprint(const xbe::Image& image, uint32_t address, const Signature& signature) {
    const int64_t offset = image.Offset(address);
    if (offset < 0 || image.FileSpan(address) < signature.bytes.size()) return false;
    return std::memcmp(image.file.data() + size_t(offset), signature.bytes.data(), signature.bytes.size()) == 0;
}

bool SignatureBytes(std::string_view text, std::vector<uint8_t>& bytes) {
    if (text.size() % 2) return false;
    bytes.clear();
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        uint32_t byte = 0;
        if (!ParseHex(text.substr(i, 2), byte) || byte > 0xFF) return false;
        bytes.push_back(uint8_t(byte));
    }
    return true;
}

}  // namespace

bool ParseManifest(std::string_view text, Manifest& manifest, std::string& error) {
    manifest = Manifest{};
    std::istringstream input{std::string(text)};
    std::string line;
    uint32_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        std::istringstream tokens(line.substr(start));
        std::vector<std::string> token;
        for (std::string value; tokens >> value;) token.push_back(std::move(value));
        if (token.empty()) continue;
        auto fail = [&](const char* reason) {
            error = "callback policy manifest line " + std::to_string(line_number) + ": " + reason;
            return false;
        };
        const std::string& kind = token[0];
        if (kind == "VERSION") {
            if (token.size() != 2 || !ParseDec(token[1], manifest.version)) return fail("bad version");
        } else if (kind == "IMAGE_SHA1") {
            if (token.size() != 2 || token[1].size() != 40) return fail("bad image SHA-1");
            manifest.image_sha1 = token[1];
        } else if (kind == "IMAGE_BASE" || kind == "IMAGE_END") {
            if (token.size() != 2) return fail("bad image bound");
            uint32_t value = 0;
            if (!ParseHex(token[1], value)) return fail("bad image bound");
            if (kind == "IMAGE_BASE") manifest.image_base = value;
            else manifest.image_end = value;
        } else if (kind == "MAPPING_SOURCE") {
            if (token.size() != 2) return fail("bad mapping source");
            manifest.mapping_source = token[1];
        } else if (kind == "MAPPING_COUNT") {
            if (token.size() != 2 || !ParseDec(token[1], manifest.mapping_count)) return fail("bad mapping count");
        } else if (kind == "MAPPING_DIGEST") {
            if (token.size() != 2 || !ParseHex(token[1], manifest.mapping_digest)) return fail("bad mapping digest");
        } else if (kind == "TABLE") {
            if (token.size() != 7 || token[1].size() != 1) return fail("bad table");
            Table table;
            table.id = token[1][0];
            if (!ParseHex(token[2], table.base) || !ParseDec(token[3], table.count) ||
                !ParseDec(token[4], table.live) || !ParseHex(token[5], table.digest) ||
                (token[6] != "0" && token[6] != "1"))
                return fail("bad table");
            table.mutable_table = token[6] == "1";
            if (FindTable(manifest, table.id)) return fail("duplicate table");
            manifest.tables.push_back(table);
        } else if (kind == "AUX") {
            if (token.size() != 4) return fail("bad auxiliary slot");
            AuxSlot aux;
            if (!ParseHex(token[1], aux.address) || !ParseHex(token[2], aux.target)) return fail("bad auxiliary slot");
            aux.role = token[3];
            manifest.aux.push_back(std::move(aux));
        } else if (kind == "SLOT") {
            if (token.size() != 5 || token[1].size() != 1) return fail("bad slot");
            Slot slot;
            slot.table = token[1][0];
            if (!ParseDec(token[2], slot.index) || !ParseHex(token[3], slot.address) || !ParseHex(token[4], slot.target))
                return fail("bad slot");
            manifest.slots.push_back(slot);
        } else if (kind == "TARGET") {
            if (token.size() < 18) return fail("short target record");
            Target target;
            uint32_t mapped = 0, args = 0, traps = 0, imports = 0, writes = 0, calls = 0, indirect = 0, auth = 0;
            if (!ParseHex(token[1], target.address) || !ParseDec(token[2], mapped)) return fail("bad target address");
            target.mapped = mapped != 0;
            target.mapping_kind = token[3];
            if (!ParseHex(token[4], target.section_va) || !ParseHex(token[5], target.section_flags) ||
                !ParseAbi(token[6], target.abi) || !ParseDec(token[7], args) || !ParseStatus(token[9], target.status) ||
                !ParseFlags(token[10], target.flags) || !ParseDec(token[11], traps) || !ParseDec(token[12], imports) ||
                !ParseDec(token[13], writes) || !ParseDec(token[14], calls) || !ParseDec(token[15], indirect) ||
                !ParseHex(token[16], target.fingerprint) || !ParseDec(token[17], auth))
                return fail("bad target record");
            target.args = args;
            target.cleanup = token[8];
            target.trap_count = traps;
            target.import_count = imports;
            target.absolute_write_count = writes;
            target.indirect_call_count = calls;
            target.indirect_write_count = indirect;
            target.runtime_authorized = auth != 0;
            for (size_t i = 18; i < token.size(); ++i) {
                const std::string& field = token[i];
                if (field.rfind("E=", 0) == 0) {
                    if (!ParseList(field.substr(2), target.edges)) return fail("bad edge list");
                } else if (field.rfind("W=", 0) == 0) {
                    if (!ParseList(field.substr(2), target.writes)) return fail("bad write list");
                } else if (field.rfind("T=", 0) == 0) {
                    if (!ParseList(field.substr(2), target.traps)) return fail("bad trap list");
                } else if (field.rfind("I=", 0) == 0) {
                    if (field == "I=-" || field.size() == 2) continue;
                    for (std::string_view part : Split(std::string_view(field).substr(2), ',')) {
                        uint32_t slot = 0, ordinal = 0;
                        const size_t colon = part.find(':');
                        if (colon == std::string_view::npos || !ParseHex(part.substr(0, colon), slot) ||
                            !ParseDec(part.substr(colon + 1), ordinal))
                            return fail("bad import list");
                        target.imports.emplace_back(slot, ordinal);
                    }
                } else {
                    return fail("unknown target field");
                }
            }
            if (manifest.targets.count(target.address)) return fail("duplicate target");
            manifest.targets.emplace(target.address, std::move(target));
        } else if (kind == "LIST") {
            if (token.size() != 11) return fail("bad list record");
            if (!ParseHex(token[1], manifest.list.node) || !ParseHex(token[2], manifest.list.callback) ||
                !ParseHex(token[3], manifest.list.register_site) || !ParseHex(token[4], manifest.list.helper) ||
                !ParseHex(token[5], manifest.list.walker) || !ParseHex(token[6], manifest.list.walk_one) ||
                !ParseHex(token[7], manifest.list.walk_zero) || !ParseDec(token[8], manifest.list.args))
                return fail("bad list record");
            manifest.list.cleanup = token[9];
            manifest.list.status = token[10];
        } else if (kind == "SIG") {
            if (token.size() != 3) return fail("bad signature");
            Signature signature;
            if (!ParseHex(token[1], signature.address) || !SignatureBytes(token[2], signature.bytes))
                return fail("bad signature");
            manifest.signatures.push_back(std::move(signature));
        } else {
            return fail("unknown record");
        }
    }
    if (manifest.version != 1) {
        error = "callback policy manifest version is not 1";
        return false;
    }
    if (manifest.image_sha1.size() != 40 || manifest.tables.empty() || manifest.slots.empty() ||
        manifest.targets.empty() || !manifest.list.callback) {
        error = "callback policy manifest is incomplete";
        return false;
    }
    for (const Slot& slot : manifest.slots) {
        const Table* table = FindTable(manifest, slot.table);
        if (!table || slot.index >= table->count) {
            error = "callback policy slot has no matching table";
            return false;
        }
    }
    return true;
}

bool LoadGeneratedMapping(const std::string& path, GeneratedMapping& mapping, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "generated mapping is missing: " + path;
        return false;
    }
    mapping = GeneratedMapping{};
    std::string text((std::istreambuf_iterator<char>(file)), {});
    size_t offset = 0;
    while ((offset = text.find("{0x", offset)) != std::string::npos) {
        const size_t begin = offset + 3;
        const size_t end = text.find('u', begin);
        if (end == std::string::npos) break;
        uint32_t address = 0;
        if (ParseHex(std::string_view(text).substr(begin, end - begin), address)) {
            mapping.order.push_back(address);
            mapping.index[address] = uint32_t(mapping.order.size() - 1);
        }
        offset = end + 1;
    }
    if (mapping.order.empty()) {
        error = "generated mapping is empty: " + path;
        return false;
    }
    uint32_t digest = 0x811C9DC5u;
    for (uint32_t address : mapping.order) {
        const uint8_t bytes[4] = {uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16),
                                  uint8_t(address >> 24)};
        digest = Fnv1aWithSeed(bytes, sizeof(bytes), digest);
    }
    mapping.digest = digest;
    return true;
}

bool AuditAgainstImage(const xbe::Image& image, std::string_view image_sha1, const Manifest& manifest,
                       const GeneratedMapping& mapping, AuditResult& result, std::string& error) {
    result = AuditResult{};
    result.tables = uint32_t(manifest.tables.size());
    result.slots = uint32_t(manifest.slots.size());
    result.targets = uint32_t(manifest.targets.size());
    auto fail = [&](const std::string& reason) {
        error = reason;
        result.ok = false;
        return false;
    };
    if (manifest.version != 1) return fail("callback policy version mismatch");
    if (image_sha1 != manifest.image_sha1) return fail("callback policy image SHA-1 mismatch");
    if (mapping.order.size() != manifest.mapping_count)
        return fail("callback policy mapping count mismatch: expected " + std::to_string(manifest.mapping_count) +
                    ", got " + std::to_string(mapping.order.size()));
    if (mapping.digest != manifest.mapping_digest) return fail("callback policy mapping digest mismatch");

    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const Table& table : manifest.tables) {
        if (!table.count) return fail("callback policy table " + std::string(1, table.id) + " is empty");
        if (table.base & 3) return fail("callback policy table " + std::string(1, table.id) + " is unaligned");
        const xbe::Section* first = image.SectionAt(table.base);
        const xbe::Section* last = image.SectionAt(table.base + table.count * 4 - 1);
        if (!first || first != last || !(first->flags & xbe::kExecutable))
            return fail("callback policy table " + std::string(1, table.id) + " is not in one executable section");
        uint32_t digest = 0;
        if (!TableDigest(image, table.base, table.count, digest))
            return fail("callback policy table " + std::string(1, table.id) + " is not file-backed");
        if (digest != table.digest)
            return fail("callback policy table " + std::string(1, table.id) + " digest mismatch");
        ranges.emplace_back(table.base, table.base + table.count * 4);
    }
    for (const AuxSlot& aux : manifest.aux) {
        uint32_t value = 0;
        if (!image.Read32(aux.address, value) || value != aux.target)
            return fail("callback policy auxiliary slot " + Hex(aux.address) + " does not hold " + Hex(aux.target));
        ranges.emplace_back(aux.address, aux.address + 4);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second) return fail("callback policy tables overlap");

    std::map<char, uint32_t> live;
    std::set<uint32_t> referenced;
    for (const Slot& slot : manifest.slots) {
        const Table* table = FindTable(manifest, slot.table);
        if (!table) return fail("callback policy slot has no table");
        if (slot.index >= table->count) return fail("callback policy slot index is outside its table");
        if (slot.address != table->base + slot.index * 4) return fail("callback policy slot address mismatch");
        uint32_t value = 0;
        if (!image.Read32(slot.address, value))
            return fail("callback policy slot " + Hex(slot.address) + " is not file-backed");
        if (value != slot.target)
            return fail("callback policy slot " + Hex(slot.address) + " changed: expected " + Hex(slot.target) +
                        ", got " + Hex(value));
        if (!slot.target || slot.target == 0xFFFFFFFFu) return fail("callback policy slot is not live");
        if (!manifest.targets.count(slot.target))
            return fail("callback policy slot " + Hex(slot.address) + " has no target record");
        ++live[slot.table];
        referenced.insert(slot.target);
    }
    for (const Table& table : manifest.tables) {
        if (live[table.id] != table.live)
            return fail("callback policy table " + std::string(1, table.id) + " live count mismatch");
    }

    for (const auto& [address, target] : manifest.targets) {
        const xbe::Section* section = image.SectionAt(address);
        if (!section || !(section->flags & xbe::kExecutable))
            return fail("callback policy target " + Hex(address) + " is not executable");
        if (section->va != target.section_va || section->flags != target.section_flags)
            return fail("callback policy target " + Hex(address) + " section changed");
        if (Fingerprint(image, address) != target.fingerprint)
            return fail("callback policy target " + Hex(address) + " fingerprint mismatch");
        if (target.mapped) {
            if (!mapping.index.count(address))
                return fail("callback policy target " + Hex(address) + " is not in the generated mapping");
        } else {
            if (target.mapping_kind != "INTERIOR_RET")
                return fail("callback policy target " + Hex(address) + " is unmapped");
            const int64_t offset = image.Offset(address);
            if (offset < 0 || image.FileSpan(address) < 1 || image.file[size_t(offset)] != 0xC3)
                return fail("callback policy interior target " + Hex(address) + " is not a RET");
        }
        if (target.status == Status::kInvalidMapping)
            return fail("callback policy target " + Hex(address) + " is invalid");
        if (((target.flags & kTrap) != 0) != (target.trap_count != 0) ||
            ((target.flags & kIndirectCall) != 0) != (target.indirect_call_count != 0) ||
            ((target.flags & kIndirectWrite) != 0) != (target.indirect_write_count != 0) ||
            ((target.flags & kAbsoluteWrite) != 0) != (target.absolute_write_count != 0) ||
            ((target.flags & kImport) != 0) != (target.import_count != 0))
            return fail("callback policy target " + Hex(address) + " flag/count mismatch");
        if (target.trap_count != target.traps.size() || target.import_count != target.imports.size() ||
            target.absolute_write_count != target.writes.size())
            return fail("callback policy target " + Hex(address) + " list/count mismatch");
        if (target.cleanup == "RET4") {
            if (target.args != 1 || target.abi != Abi::kStdcall)
                return fail("callback policy target " + Hex(address) + " RET4 ABI mismatch");
        } else if (target.cleanup == "RET8") {
            if (target.args != 2 || target.abi != Abi::kStdcall)
                return fail("callback policy target " + Hex(address) + " RET8 ABI mismatch");
        } else if (target.cleanup == "RET") {
            if (target.abi != Abi::kCdecl)
                return fail("callback policy target " + Hex(address) + " RET ABI mismatch");
        } else if (target.status == Status::kVerifiedStatic) {
            return fail("callback policy target " + Hex(address) + " has no terminal");
        }
        const uint32_t call_site_args = address == manifest.list.callback ? manifest.list.args : 0;
        if (target.args > call_site_args && target.status != Status::kProhibited)
            return fail("callback policy target " + Hex(address) + " argument mismatch is not prohibited");
        if ((target.flags & (kFrame | kArgMismatch)) && target.status != Status::kProhibited)
            return fail("callback policy frame-sensitive target " + Hex(address) + " is not prohibited");
        if (target.status == Status::kVerifiedStatic) {
            if (target.trap_count || target.indirect_call_count || target.indirect_write_count ||
                target.args > call_site_args || (target.flags & (kFrame | kArgMismatch)))
                return fail("callback policy target " + Hex(address) + " cannot be verified");
        }
        if (target.flags & kRecheck) {
            // the manifest records that a future call must re-read a mutable slot
        } else {
            return fail("callback policy target " + Hex(address) + " has no recheck requirement");
        }
        if (!referenced.count(address) && address != manifest.list.callback) {
            bool auxiliary = false;
            for (const AuxSlot& aux : manifest.aux) auxiliary = auxiliary || aux.target == address;
            if (!auxiliary) return fail("callback policy target " + Hex(address) + " is unreferenced");
        }
        if (target.runtime_authorized) ++result.authorized;
        switch (target.status) {
            case Status::kVerifiedStatic: ++result.verified; break;
            case Status::kInferred: ++result.inferred; break;
            case Status::kUnresolved: ++result.unresolved; break;
            case Status::kProhibited: ++result.prohibited; break;
            case Status::kInvalidMapping: ++result.invalid; break;
        }
    }

    const auto list = manifest.targets.find(manifest.list.callback);
    if (list == manifest.targets.end()) return fail("callback policy list callback has no target record");
    if (!list->second.mapped || list->second.args != manifest.list.args || list->second.args != 1 ||
        list->second.cleanup != "RET4" || manifest.list.cleanup != "RET4")
        return fail("callback policy list callback ABI mismatch");
    if (AuthorizesExecution(manifest, 0, 0, manifest.list.callback, manifest.list.args))
        return fail("callback policy list callback is authorized without runtime authorization");
    if (!manifest.list.node || !manifest.list.helper || !manifest.list.walker || !manifest.list.walk_one ||
        !manifest.list.walk_zero)
        return fail("callback policy list record is incomplete");

    for (const Signature& signature : manifest.signatures)
        if (!ByteFingerprint(image, signature.address, signature))
            return fail("callback policy signature " + Hex(signature.address) + " mismatch");

    result.ok = true;
    error.clear();
    return true;
}

bool AuthorizesExecution(const Manifest& manifest, char table, uint32_t slot_address, uint32_t target,
                         uint32_t call_site_args) {
    const auto found = manifest.targets.find(target);
    if (found == manifest.targets.end()) return false;
    const Target& candidate = found->second;
    if (!candidate.runtime_authorized || candidate.status != Status::kVerifiedStatic) return false;
    if (candidate.args != call_site_args || candidate.trap_count != 0 || candidate.indirect_call_count != 0 ||
        candidate.indirect_write_count != 0 || (candidate.flags & (kFrame | kArgMismatch)))
        return false;
    if (table == 0) return target == manifest.list.callback && call_site_args == manifest.list.args;
    for (const Slot& slot : manifest.slots)
        if (slot.table == table && slot.address == slot_address) return slot.target == target;
    return false;
}

const char* StatusName(Status status) {
    switch (status) {
        case Status::kVerifiedStatic: return "VERIFIED_STATIC";
        case Status::kInferred: return "INFERRED";
        case Status::kUnresolved: return "UNRESOLVED";
        case Status::kProhibited: return "PROHIBITED";
        case Status::kInvalidMapping: return "INVALID_MAPPING";
    }
    return "UNKNOWN";
}

}  // namespace callback_policy
