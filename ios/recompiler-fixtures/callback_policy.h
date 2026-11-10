#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xbe {
struct Image;
}

namespace callback_policy {

enum class Status : uint8_t { kVerifiedStatic, kInferred, kUnresolved, kProhibited, kInvalidMapping };
enum class Abi : uint8_t { kNone, kCdecl, kStdcall, kUnknown };

enum TargetFlags : uint32_t {
    kTrap = 1u << 0,
    kFrame = 1u << 1,
    kArgMismatch = 1u << 2,
    kIndirectCall = 1u << 3,
    kIndirectWrite = 1u << 4,
    kAbsoluteWrite = 1u << 5,
    kImport = 1u << 6,
    kFs = 1u << 7,
    kTail = 1u << 8,
    kRecheck = 1u << 9,
};

struct Table {
    char id = 0;
    uint32_t base = 0, count = 0, live = 0, digest = 0;
    bool mutable_table = false;
};

struct Slot {
    char table = 0;
    uint32_t index = 0, address = 0, target = 0;
};

struct AuxSlot {
    uint32_t address = 0, target = 0;
    std::string role;
};

struct Target {
    uint32_t address = 0;
    bool mapped = false;
    std::string mapping_kind;  // ENTRY, INTERIOR_RET
    uint32_t section_va = 0, section_flags = 0;
    Abi abi = Abi::kUnknown;
    uint32_t args = 0;
    std::string cleanup;  // RET, RET4, RET8, NONE, VARIES
    Status status = Status::kUnresolved;
    uint32_t flags = 0;
    uint32_t trap_count = 0, import_count = 0, absolute_write_count = 0;
    uint32_t indirect_call_count = 0, indirect_write_count = 0;
    uint32_t fingerprint = 0;
    bool runtime_authorized = false;
    std::vector<uint32_t> edges, writes, traps;
    std::vector<std::pair<uint32_t, uint32_t>> imports;
};

struct ListRecord {
    uint32_t node = 0, callback = 0, register_site = 0, helper = 0, walker = 0;
    uint32_t walk_one = 0, walk_zero = 0;
    uint32_t args = 0;
    std::string cleanup, status;
};

struct Signature {
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
};

struct Manifest {
    uint32_t version = 0;
    std::string image_sha1, mapping_source;
    uint32_t image_base = 0, image_end = 0;
    uint32_t mapping_count = 0, mapping_digest = 0;
    std::vector<Table> tables;
    std::vector<Slot> slots;
    std::vector<AuxSlot> aux;
    std::map<uint32_t, Target> targets;
    ListRecord list;
    std::vector<Signature> signatures;
};

struct GeneratedMapping {
    std::vector<uint32_t> order;
    std::map<uint32_t, uint32_t> index;
    uint32_t digest = 0;
};

struct AuditResult {
    uint32_t tables = 0, slots = 0, targets = 0;
    uint32_t verified = 0, inferred = 0, unresolved = 0, prohibited = 0, invalid = 0, authorized = 0;
    bool ok = false;
};

bool ParseManifest(std::string_view text, Manifest& manifest, std::string& error);
bool LoadGeneratedMapping(const std::string& path, GeneratedMapping& mapping, std::string& error);
bool AuditAgainstImage(const xbe::Image& image, std::string_view image_sha1, const Manifest& manifest,
                       const GeneratedMapping& mapping, AuditResult& result, std::string& error);

// Runtime authorisation is deliberately separate from static classification.
// A target present in the manifest is not authorised unless the manifest also
// carries a reviewed runtime authorisation and the caller supplies the right
// table slot and argument count.
bool AuthorizesExecution(const Manifest& manifest, char table, uint32_t slot_address, uint32_t target,
                         uint32_t call_site_args);

const char* StatusName(Status status);

}  // namespace callback_policy
