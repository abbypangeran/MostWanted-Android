// Phase 2D-19 synthetic rehearsal fixture builder.
// Emits a synthetic XBE whose reviewed routes occupy known addresses, the
// generated full-route mirror used by the 211-instruction rehearsal, and a
// recompiler config that gives every reviewed instruction its own generated
// function. No authentic game bytes are used.
//
// The full-route mirror is derived from the reviewed manifest at build time:
// the step classes, kernel ordinals, arguments, conditional outcomes, SEH/TLS
// shape and the 71/140 counts are the authentic route's, while every address
// and instruction encoding is synthetic.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "kernel/authentic_run_route.h"
#include "xbe/xbe.h"

namespace route = kernel::authentic::route;

namespace {

constexpr uint32_t kBase = 0x10000;
constexpr uint32_t kEntry = 0x11000;
constexpr uint32_t kSectionTable = 0x180;
constexpr uint32_t kSectionName = 0x240;
constexpr uint32_t kRawCode = 0x1000;
constexpr uint32_t kCompactSize = 0x1000;  // 0x11000..0x12000
constexpr uint32_t kFullRaw = 0x2000;
constexpr uint32_t kFullSize = 0x4000;      // 0x20000..0x24000
constexpr uint32_t kImageSize = 0x14000;    // 0x10000..0x24000

// Full-route synthetic layout. The route mirrors are laid out contiguously in
// authentic-address order, so every natural fallthrough is pc + encoded size.
constexpr uint32_t kFullParentBase = 0x00020000;
constexpr uint32_t kFullChildBase = 0x00021000;
constexpr uint32_t kFullHandler = 0x00023000;
constexpr uint32_t kFullHandlerEnd = 0x00023010;
constexpr uint32_t kFullTrapBase = 0x00023100;

constexpr uint32_t kFullDataPage = 0x00040000;
constexpr uint32_t kFullDataPages = 4;
constexpr uint32_t kFullScopeTable = 0x00040000;
constexpr uint32_t kFullCriticalSection = 0x00040100;
constexpr uint32_t kFullShutdownObject = 0x00040200;
constexpr uint32_t kFullDpcObject = 0x00040240;
constexpr uint32_t kFullTimerObject = 0x00040280;
constexpr uint32_t kFullDpcContext = 0x00040300;
constexpr uint32_t kFullHardwareSlot = 0x00040080;
constexpr uint32_t kFullVersionSlot = 0x00040084;
constexpr uint32_t kFullHandlerMarker = 0x00040090;
constexpr uint32_t kFullDebe8 = 0x000400A0;
constexpr uint32_t kFullDebec = 0x000400A4;
constexpr uint32_t kFullHandleOut = 0x00040400;
constexpr uint32_t kFullIdOut = 0x00040404;
constexpr uint32_t kFullStatusOut = 0x00040408;
constexpr uint32_t kFullCloseStatusOut = 0x0004040C;
constexpr uint32_t kFullSlotBase = 0x00041000;

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
    bytes[offset + 2] = uint8_t(value >> 16);
    bytes[offset + 3] = uint8_t(value >> 24);
}

void Emit32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(uint8_t(value));
    bytes.push_back(uint8_t(value >> 8));
    bytes.push_back(uint8_t(value >> 16));
    bytes.push_back(uint8_t(value >> 24));
}

// NOP is not implemented by the checked recompiler; mov eax, eax is a
// harmless, implemented two-byte filler.
void EmitNop(std::vector<uint8_t>& bytes) {
    bytes.push_back(0x89);
    bytes.push_back(0xC0);
}
void EmitJmp(std::vector<uint8_t>& bytes, uint32_t pc, uint32_t target) {
    bytes.push_back(0xE9);
    Emit32(bytes, target - (pc + 5));
}
void EmitCallRel(std::vector<uint8_t>& bytes, uint32_t pc, uint32_t target) {
    bytes.push_back(0xE8);
    Emit32(bytes, target - (pc + 5));
}
void EmitCallAbs(std::vector<uint8_t>& bytes, uint32_t slot) {
    bytes.push_back(0xFF);
    bytes.push_back(0x15);
    Emit32(bytes, slot);
}
void EmitPushImm32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(0x68);
    Emit32(bytes, value);
}
void EmitPushImm8(std::vector<uint8_t>& bytes, uint8_t value) {
    bytes.push_back(0x6A);
    bytes.push_back(value);
}
void EmitPushReg(std::vector<uint8_t>& bytes, uint8_t reg) { bytes.push_back(uint8_t(0x50 + reg)); }
void EmitPopReg(std::vector<uint8_t>& bytes, uint8_t reg) { bytes.push_back(uint8_t(0x58 + reg)); }
void EmitRet(std::vector<uint8_t>& bytes) { bytes.push_back(0xC3); }
void EmitRetImm(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(0xC2);
    bytes.push_back(uint8_t(value));
    bytes.push_back(uint8_t(value >> 8));
}

void Append(std::vector<uint8_t>& bytes, std::initializer_list<uint8_t> values) {
    bytes.insert(bytes.end(), values.begin(), values.end());
}

struct Chunk {
    uint32_t address;
    std::vector<uint8_t> bytes;
};

struct SynthStep {
    uint32_t pc = 0;
    uint32_t next = 0;
    uint32_t target = 0;
    uint32_t slot = 0;
    uint32_t encoding = 0;
    uint16_t ordinal = 0;
    uint8_t arguments = 0;
    bool taken = false;
    route::Segment segment = route::Segment::kParentRoot;
    route::StepKind kind = route::StepKind::kSequential;
    std::string text;
    uint8_t ret_pop_bytes = 0;
    std::vector<uint8_t> code;
};

struct SynthRoute {
    std::vector<SynthStep> parent;
    std::vector<SynthStep> child;
    uint32_t parent_stop = 0;
    uint32_t child_stop = 0;
    uint32_t child_failure = 0;
    uint32_t handler = kFullHandler;
    uint32_t seh_call = 0;
    uint32_t seh_entry = 0;
    uint32_t child_start_routine = 0;
    uint32_t child_entry = 0;
};

struct AddressMap {
    std::map<uint32_t, uint32_t> route_steps;
    std::map<uint32_t, uint32_t> traps;
    uint32_t next_unused = 0x00024000;

    uint32_t operator()(uint32_t authentic) {
        if (const auto it = route_steps.find(authentic); it != route_steps.end()) return it->second;
        if (const auto it = traps.find(authentic); it != traps.end()) return it->second;
        const uint32_t synthetic = next_unused;
        next_unused += 0x10;
        traps.emplace(authentic, synthetic);
        return synthetic;
    }
};

uint32_t SlotFor(uint32_t ordinal) { return kFullSlotBase + ordinal * 8; }

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

void EncodeStep(bool parent, size_t index, const route::Step& step, const SynthStep& synth, uint32_t pc,
                AddressMap& map, std::vector<uint8_t>& bytes) {
    switch (step.kind) {
        case route::StepKind::kCall:
            EmitCallRel(bytes, pc, map(step.target));
            return;
        case route::StepKind::kConditional: {
            if (!parent && index == 168) {
                bytes.push_back(0x0F);
                bytes.push_back(0x85);
                Emit32(bytes, map(step.target) - (pc + 6));
                return;
            }
            if (!parent && index == 144) {
                bytes.push_back(0x0F);
                bytes.push_back(0x85);
                Emit32(bytes, map(step.target) - (pc + 6));
                return;
            }
            const uint32_t expected = step.taken ? map(step.target) : pc + uint32_t(bytes.size() + 5);
            EmitJmp(bytes, pc, expected);
            return;
        }
        case route::StepKind::kKernelCall:
            if (StartsWith(step.text, "jmp [")) {
                bytes.push_back(0xFF);
                bytes.push_back(0x25);
                Emit32(bytes, SlotFor(step.ordinal));
            } else {
                EmitCallAbs(bytes, SlotFor(step.ordinal));
            }
            return;
        case route::StepKind::kRet:
            if (step.ret_pop_bytes)
                EmitRetImm(bytes, step.ret_pop_bytes);
            else
                EmitRet(bytes);
            return;
        case route::StepKind::kSequential:
            break;
    }
    if (StartsWith(step.text, "jmp ")) {
        EmitJmp(bytes, pc, map(step.next));
        return;
    }
    if (parent) {
        switch (index) {
            case 9: EmitPushReg(bytes, 6); return;   // esi
            case 11: EmitPushReg(bytes, 7); return;  // edi
            case 21: case 22: case 23: case 24: case 25: case 26:
                EmitPushImm8(bytes, 0);
                return;
            case 29: bytes.push_back(0x55); return;  // push ebp
            case 30: Append(bytes, {0x8B, 0xEC}); return;  // mov ebp, esp
            case 36: EmitPushImm32(bytes, synth.target); return;  // child system routine
            case 37: case 38: case 39: case 41: case 44: EmitPushImm8(bytes, 0); return;
            case 40: EmitPushImm32(bytes, synth.target); return;  // child start routine
            case 42: EmitPushImm32(bytes, 0x94); return;
            case 43: EmitPushImm32(bytes, 0x10000); return;
            case 45: EmitPushImm32(bytes, kFullHandleOut); return;  // arg0 handle out
            case 46: EmitNop(bytes); return;
            case 53: Append(bytes, {0xA1}); Emit32(bytes, kFullHandleOut); return;  // mov eax, [handle]
            case 54: bytes.push_back(0x5D); return;              // pop ebp
            case 56: Append(bytes, {0x8B, 0xF8}); return;        // mov edi, eax
            case 59: EmitPushReg(bytes, 7); return;              // push edi
            case 61: Append(bytes, {0xFF, 0x74, 0x24, 0x04}); return;  // push [esp+4]
            case 69: EmitPopReg(bytes, 7); return;
            case 70: EmitPopReg(bytes, 6); return;
            default: EmitNop(bytes); return;
        }
    }
    switch (index) {
        case 0: EmitPushImm8(bytes, 0x18); return;
        case 1: EmitPushImm32(bytes, kFullScopeTable); return;
        case 3: EmitPushImm32(bytes, kFullHandler); return;
        case 4: Append(bytes, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}); return;
        case 5: EmitPushReg(bytes, 0); return;
        case 6: Append(bytes, {0x8B, 0x44, 0x24, 0x10}); return;
        case 7: Append(bytes, {0x89, 0x6C, 0x24, 0x10}); return;
        case 8: Append(bytes, {0x8D, 0x6C, 0x24, 0x10}); return;
        case 9: Append(bytes, {0x29, 0xC4}); return;
        case 10: EmitPushReg(bytes, 3); return;
        case 11: EmitPushReg(bytes, 6); return;
        case 12: EmitPushReg(bytes, 7); return;
        case 13: Append(bytes, {0x8B, 0x45, 0xF8}); return;
        case 14: Append(bytes, {0x89, 0x65, 0xE8}); return;
        case 15: EmitPushReg(bytes, 0); return;
        case 16: Append(bytes, {0x8B, 0x45, 0xFC}); return;
        case 17: Append(bytes, {0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF}); return;
        case 18: Append(bytes, {0x89, 0x45, 0xF8}); return;
        case 19: Append(bytes, {0x8D, 0x45, 0xF0}); return;
        case 20: Append(bytes, {0x64, 0xA3, 0x00, 0x00, 0x00, 0x00}); return;
        case 22: Append(bytes, {0x83, 0x65, 0xFC, 0x00}); return;
        case 23: Append(bytes, {0x64, 0xA1, 0x28, 0x00, 0x00, 0x00}); return;
        case 25: Append(bytes, {0x8B, 0x50, 0x28}); return;
        case 26: Append(bytes, {0x83, 0xC2, 0x04}); return;
        case 28: Append(bytes, {0x89, 0x52, 0xFC}); return;
        case 53: EmitPushImm8(bytes, 1); return;
        case 58: case 65: EmitPushImm32(bytes, kFullCriticalSection); return;
        case 71: Append(bytes, {0xFF, 0x75, 0x0C}); return;
        case 72: Append(bytes, {0xFF, 0x55, 0x08}); return;
        case 74: Append(bytes, {0xA1}); Emit32(bytes, kFullHardwareSlot); return;
        case 75: Append(bytes, {0x8B, 0x00}); return;
        case 76: Append(bytes, {0x8B, 0x0D}); Emit32(bytes, kFullVersionSlot); return;
        case 77: Append(bytes, {0x0F, 0xB7, 0x49, 0x04}); return;
        case 92: Append(bytes, {0x64, 0xA1, 0x20, 0x00, 0x00, 0x00}); return;
        case 93: Append(bytes, {0x83, 0xB8, 0x50, 0x02, 0x00, 0x00, 0x00}); return;
        case 97: bytes.push_back(0x55); return;
        case 98: Append(bytes, {0x8B, 0xEC}); return;
        case 99: case 100: EmitPushReg(bytes, 1); return;
        case 101: Append(bytes, {0xA1, 0x3C, 0x00, 0x01, 0x80}); return;
        case 102: bytes.push_back(0xBA); Emit32(bytes, 0x7FFF0000u); return;
        case 103: Append(bytes, {0x2B, 0xC2}); return;
        case 104: Append(bytes, {0x0F, 0xB7, 0x48, 0x06}); return;
        case 105: EmitPushReg(bytes, 6); return;
        case 106: Append(bytes, {0x0F, 0xB7, 0x70, 0x14}); return;
        case 107: Append(bytes, {0x03, 0xF0}); return;
        case 108: Append(bytes, {0x8D, 0x0C, 0x89}); return;
        case 109: Append(bytes, {0x8D, 0x44, 0xC8, 0xF0}); return;
        case 110: Append(bytes, {0x81, 0x38, 0x49, 0x4E, 0x49, 0x54}); return;
        case 111: EmitPopReg(bytes, 6); return;
        case 113: bytes.push_back(0xC9); return;
        case 116: bytes.push_back(0x55); return;
        case 117: Append(bytes, {0x8B, 0xEC}); return;
        case 118: Append(bytes, {0x83, 0xEC, 0x34}); return;
        case 124: EmitPushImm8(bytes, 1); return;
        case 126: EmitPushImm32(bytes, kFullShutdownObject); return;
        case 130: bytes.push_back(0x55); return;
        case 131: Append(bytes, {0x8B, 0xEC}); return;
        case 132: Append(bytes, {0x83, 0xEC, 0x0C}); return;
        case 133: EmitPushImm32(bytes, kFullDpcContext); return;
        case 134: EmitPushImm32(bytes, kFullHandler); return;
        case 135: EmitPushImm32(bytes, kFullDpcObject); return;
        case 140: Append(bytes, {0xA1}); Emit32(bytes, kFullHardwareSlot); return;
        case 141: Append(bytes, {0x83, 0x25}); Emit32(bytes, kFullDebe8); bytes.push_back(0x00); return;
        case 142: Append(bytes, {0x83, 0x25}); Emit32(bytes, kFullDebec); bytes.push_back(0x00); return;
        case 143: Append(bytes, {0xF6, 0x00, 0x08}); return;
        case 145: Append(bytes, {0x8D, 0x45, 0xF8}); return;
        case 146: EmitPushReg(bytes, 0); return;
        case 147: EmitPushImm8(bytes, 4); return;
        case 148: Append(bytes, {0x8D, 0x45, 0xFC}); return;
        case 149: EmitPushReg(bytes, 0); return;
        case 150: Append(bytes, {0x8D, 0x45, 0xF4}); return;
        case 151: EmitPushReg(bytes, 0); return;
        case 152: EmitPushImm8(bytes, 0x11); return;
        case 154: bytes.push_back(0x55); return;
        case 155: Append(bytes, {0x8B, 0xEC}); return;
        case 156: Append(bytes, {0xFF, 0x75, 0x18}); return;
        case 157: Append(bytes, {0xFF, 0x75, 0x14}); return;
        case 158: Append(bytes, {0xFF, 0x75, 0x10}); return;
        case 159: Append(bytes, {0xFF, 0x75, 0x0C}); return;
        case 160: Append(bytes, {0xFF, 0x75, 0x08}); return;
        case 163: bytes.push_back(0x50); return;
        case 165: bytes.push_back(0x5D); return;
        case 167: Append(bytes, {0x85, 0xC0}); return;
        case 169: Append(bytes, {0x8B, 0x45, 0xFC}); return;
        case 170: Append(bytes, {0x83, 0xE0, 0x01}); return;
        case 171: bytes.push_back(0xA3); Emit32(bytes, kFullDebe8); return;
        case 137: EmitPushImm8(bytes, 0); return;
        case 138: EmitPushImm32(bytes, kFullTimerObject); return;
        default: EmitNop(bytes); return;
    }
}

uint32_t EncodedSize(bool parent, size_t index, const route::Step& step) {
    AddressMap empty;
    SynthStep dummy;
    std::vector<uint8_t> bytes;
    EncodeStep(parent, index, step, dummy, 0, empty, bytes);
    return uint32_t(bytes.size());
}

SynthRoute BuildFullRoute() {
    SynthRoute result;

    struct Entry {
        size_t index;
        const route::Step* step;
    };
    std::vector<Entry> parent_order;
    for (size_t i = 0; i < route::kParentRouteCount; ++i) parent_order.push_back({i, &route::kParentRoute[i]});
    std::sort(parent_order.begin(), parent_order.end(),
              [](const Entry& a, const Entry& b) { return a.step->pc < b.step->pc; });
    std::vector<Entry> child_order;
    for (size_t i = 0; i < route::kChildRouteCount; ++i) child_order.push_back({i, &route::kChildRoute[i]});
    std::sort(child_order.begin(), child_order.end(),
              [](const Entry& a, const Entry& b) { return a.step->pc < b.step->pc; });

    std::map<size_t, uint32_t> parent_sizes;
    for (const Entry& entry : parent_order) parent_sizes[entry.index] = EncodedSize(true, entry.index, *entry.step);
    std::map<size_t, uint32_t> child_sizes;
    for (const Entry& entry : child_order) child_sizes[entry.index] = EncodedSize(false, entry.index, *entry.step);

    AddressMap map;
    // Lay out each contiguous authentic code region separately. The region
    // holding the step whose fallthrough is the mandatory stop is placed first
    // so the stop sits exactly at that step's natural next address. Other
    // regions follow after the stop; calls, jumps and returns cross regions
    // through the reviewed map.
    auto assign = [&](const route::Step* steps, size_t count, const std::map<size_t, uint32_t>& sizes,
                      uint32_t base, uint32_t stop_pc, uint32_t& stop, uint32_t& failure,
                      bool reserve_failure) {
        std::vector<size_t> sorted(count);
        for (size_t i = 0; i < count; ++i) sorted[i] = i;
        std::sort(sorted.begin(), sorted.end(), [steps](size_t a, size_t b) { return steps[a].pc < steps[b].pc; });
        std::vector<std::vector<size_t>> regions;
        for (size_t index : sorted) {
            if (regions.empty()) {
                regions.emplace_back();
            } else {
                const route::Step& previous = steps[regions.back().back()];
                if (steps[index].pc != previous.pc + previous.length) regions.emplace_back();
            }
            regions.back().push_back(index);
        }
        size_t stop_region = regions.size();
        for (size_t r = 0; r < regions.size(); ++r)
            for (size_t index : regions[r])
                if (steps[index].next == stop_pc) stop_region = r;
        if (stop_region >= regions.size()) stop_region = regions.size() - 1;
        std::vector<size_t> region_order;
        region_order.push_back(stop_region);
        for (size_t r = 0; r < regions.size(); ++r)
            if (r != stop_region) region_order.push_back(r);
        uint32_t pc = base;
        for (size_t r : region_order) {
            for (size_t index : regions[r]) {
                map.route_steps[steps[index].pc] = pc;
                pc += sizes.at(index);
            }
            if (r == stop_region) {
                if (reserve_failure) {
                    failure = pc;
                    stop = pc + 10;
                } else {
                    stop = pc;
                }
                pc = stop + 0x10;
            } else {
                pc += 0x10;
            }
        }
    };
    uint32_t parent_failure = 0;
    assign(route::kParentRoute, route::kParentRouteCount, parent_sizes, kFullParentBase, route::kParentStop,
           result.parent_stop, parent_failure, false);
    assign(route::kChildRoute, route::kChildRouteCount, child_sizes, kFullChildBase, route::kChildStop,
           result.child_stop, result.child_failure, true);
    map.traps[route::kParentStop] = result.parent_stop;
    map.traps[route::kChildStop] = result.child_stop;
    map.traps[0x002B5E75u] = result.child_failure;
    map.traps[route::kSehHandler] = kFullHandler;
    for (uint32_t trap : route::kParentTrapEntries)
        if (trap != route::kParentStop) map(trap);
    for (uint32_t trap : route::kChildTrapEntries)
        if (trap != route::kChildStop && trap != route::kSehHandler) map(trap);

    result.child_entry = map.route_steps[route::kChildEntry];
    result.seh_call = map.route_steps[route::kSehCall];
    result.seh_entry = map.route_steps[route::kSehEntry];
    result.child_start_routine = map.route_steps[route::kChildStartRoutine];

    // Emit the manifest in reviewed execution order (the entry is index 0).
    auto build = [&](const route::Step* steps, size_t count, const std::map<size_t, uint32_t>& sizes,
                     bool parent, std::vector<SynthStep>& output) {
        for (size_t i = 0; i < count; ++i) {
            const route::Step& step = steps[i];
            const uint32_t current = map.route_steps[step.pc];
            const uint32_t size = sizes.at(i);
            SynthStep synth;
            synth.pc = current;
            synth.kind = step.kind;
            synth.ordinal = step.ordinal;
            synth.arguments = step.arguments;
            synth.taken = step.taken;
            synth.segment = step.segment;
            synth.text = step.text;
            synth.ret_pop_bytes = step.ret_pop_bytes;
            synth.encoding = step.encoding;
            synth.slot = step.kind == route::StepKind::kKernelCall ? SlotFor(step.ordinal) : 0;
            const bool natural_next = step.next == step.pc + step.length;
            if (step.kind == route::StepKind::kRet) {
                synth.next = map(step.next);
            } else if (step.kind == route::StepKind::kSequential && StartsWith(step.text, "jmp ")) {
                synth.next = map(step.next);
            } else if (step.kind == route::StepKind::kConditional && !step.taken) {
                synth.next = map(step.next);
            } else if (natural_next) {
                synth.next = current + size;
            } else {
                synth.next = map(step.next);
            }
            if (step.kind == route::StepKind::kCall) {
                synth.next = natural_next ? current + size : map(step.next);
                synth.target = map(step.target);
            } else if (step.kind == route::StepKind::kConditional) {
                synth.target = map(step.target);
            } else {
                synth.target = step.target ? map(step.target) : 0;
            }
            if (parent && i == 36) synth.target = result.child_entry;
            if (parent && i == 40) synth.target = result.child_start_routine;
            EncodeStep(parent, i, step, synth, current, map, synth.code);
            if (synth.code.size() != size) {
                std::fprintf(stderr, "fixture encoding size mismatch at %08X: %zu != %u\n", step.pc,
                             synth.code.size(), size);
                std::exit(2);
            }
            output.push_back(std::move(synth));
        }
    };
    build(route::kParentRoute, route::kParentRouteCount, parent_sizes, true, result.parent);
    build(route::kChildRoute, route::kChildRouteCount, child_sizes, false, result.child);
    return result;
}

std::string Escape(const std::string& text) {
    std::string escaped;
    for (char c : text) {
        if (c == '"' || c == '\\') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

const char* KindName(route::StepKind kind) {
    switch (kind) {
        case route::StepKind::kSequential: return "kSequential";
        case route::StepKind::kConditional: return "kConditional";
        case route::StepKind::kCall: return "kCall";
        case route::StepKind::kKernelCall: return "kKernelCall";
        case route::StepKind::kRet: return "kRet";
    }
    return "kSequential";
}

const char* SegmentName(route::Segment segment) {
    switch (segment) {
        case route::Segment::kParentRoot: return "kParentRoot";
        case route::Segment::kSystemRoutine: return "kSystemRoutine";
        case route::Segment::kSehPrologue: return "kSehPrologue";
        case route::Segment::kPreStartWalk: return "kPreStartWalk";
        case route::Segment::kStartCallSetup: return "kStartCallSetup";
        case route::Segment::kStartRoutine: return "kStartRoutine";
        case route::Segment::kHardwareProbe: return "kHardwareProbe";
        case route::Segment::kKernelInit: return "kKernelInit";
    }
    return "kParentRoot";
}

void EmitHeaderRoute(std::ofstream& header, const char* name, const std::vector<SynthStep>& steps) {
    header << "inline constexpr Step k" << name << "Route[] = {\n";
    for (const SynthStep& step : steps) {
        header << "    {0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.pc
               << "u, " << std::dec << unsigned(step.code.size()) << ", StepKind::" << KindName(step.kind)
               << ", 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.next
               << "u, 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.target
               << "u, 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.slot
               << "u, 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << step.encoding
               << "u, " << std::dec << step.ordinal << ", " << unsigned(step.arguments) << ", "
               << (step.taken ? "true" : "false") << ", Segment::" << SegmentName(step.segment) << ", \""
               << Escape(step.text) << "\"";
        if (step.ret_pop_bytes) header << ", " << unsigned(step.ret_pop_bytes);
        header << std::dec << "},\n";
    }
    header << "};\n";
    header << "inline constexpr size_t k" << name << "RouteCount = sizeof(k" << name
           << "Route) / sizeof(k" << name << "Route[0]);\n\n";
}

bool WriteFullRoute(std::vector<Chunk>& chunks, const std::filesystem::path& header_path,
                    std::ostringstream& config) {
    const SynthRoute route = BuildFullRoute();
    for (const SynthStep& step : route.parent) chunks.push_back({step.pc, step.code});
    for (const SynthStep& step : route.child) chunks.push_back({step.pc, step.code});
    chunks.push_back({kFullHandler,
                      {0xC7, 0x05, 0x90, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0xC0, 0xC3}});
    chunks.push_back({route.parent_stop, {0xC3}});
    chunks.push_back({route.child_failure,
                      {0xC7, 0x05, uint8_t(kFullDebec & 0xFF), uint8_t((kFullDebec >> 8) & 0xFF),
                       uint8_t((kFullDebec >> 16) & 0xFF), uint8_t((kFullDebec >> 24) & 0xFF),
                       0x01, 0x00, 0x00, 0x00}});
    chunks.push_back({route.child_stop, {0xC3}});

    std::ofstream header(header_path);
    if (!header) return false;
    header << "// Generated by authentic_run_fixture_builder. Do not edit.\n"
              "// Synthetic 244-instruction mirror of the reviewed route; no authentic bytes.\n"
              "#pragma once\n\n"
              "#include <cstddef>\n"
              "#include <cstdint>\n\n"
              "#include \"kernel/authentic_run_route.h\"\n\n"
              "namespace authentic_run_full_fixture {\n\n"
              "using kernel::authentic::route::Segment;\n"
              "using kernel::authentic::route::Step;\n"
              "using kernel::authentic::route::StepKind;\n\n";
    EmitHeaderRoute(header, "Parent", route.parent);
    EmitHeaderRoute(header, "Child", route.child);
    header << "inline constexpr uint32_t kParentEntry = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.parent.front().pc << "u;\n";
    header << "inline constexpr uint32_t kParentStop = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.parent_stop << "u;\n";
    header << "inline constexpr uint32_t kChildEntry = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.child_entry << "u;\n";
    header << "inline constexpr uint32_t kChildStop = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.child_stop << "u;\n";
    header << "inline constexpr uint32_t kChildStartRoutine = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.child_start_routine << "u;\n";
    header << "inline constexpr uint32_t kSehCall = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.seh_call << "u;\n";
    header << "inline constexpr uint32_t kSehEntry = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.seh_entry << "u;\n";
    header << "inline constexpr uint32_t kSehHandler = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << route.handler << "u;\n";
    header << "inline constexpr uint32_t kSehScopeTable = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullScopeTable << "u;\n";
    header << "inline constexpr uint32_t kSehScopeBytes = 0x18u;\n";
    header << "inline constexpr uint32_t kSehScopeFlag = 0u;\n";
    header << "inline constexpr uint32_t kTlsBlockSize = 0x94u;\n";
    header << "inline constexpr uint32_t kCriticalSectionObject = 0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill('0') << kFullCriticalSection << "u;\n";
    header << "inline constexpr uint32_t kParentExecutedInstructions = " << std::dec << route.parent.size()
           << "u;\n";
    header << "inline constexpr uint32_t kChildFailure = 0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill('0') << route.child_failure << "u;\n";
    header << "inline constexpr uint32_t kChildExecutedInstructions = " << std::dec << route.child.size()
           << "u;\n";
    header << "inline constexpr uint32_t kTotalExecutedInstructions = " << std::dec
           << route.parent.size() + route.child.size() << "u;\n";
    header << "inline constexpr uint32_t kParentBoundarySteps = " << std::dec << route.parent.size() + 1
           << "u;\n";
    header << "inline constexpr uint32_t kChildBoundarySteps = " << std::dec << route.child.size() + 1
           << "u;\n";
    header << "inline constexpr uint32_t kTotalBoundarySteps = " << std::dec
           << route.parent.size() + route.child.size() + 2 << "u;\n";
    header << "inline constexpr uint32_t kParentRequiredTransfers = "
              "kernel::authentic::route::ComputeRequiredTransfers(kParentRoute, kParentRouteCount);\n";
    header << "inline constexpr uint32_t kChildRequiredTransfers = "
              "kernel::authentic::route::ComputeRequiredTransfers(kChildRoute, kChildRouteCount);\n";
    header << "inline constexpr uint32_t kParentTrapEntries[] = {kParentStop};\n";
    header << "inline constexpr uint32_t kChildTrapEntries[] = {kChildStop, kSehHandler};\n\n";
    header << "inline constexpr uint32_t kDataPage = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullDataPage << "u;\n";
    header << "inline constexpr uint32_t kDataPages = " << std::dec << kFullDataPages << "u;\n";
    header << "inline constexpr uint32_t kScopeTable = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullScopeTable << "u;\n";
    header << "inline constexpr uint32_t kCriticalSectionAddress = 0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill('0') << kFullCriticalSection << "u;\n";
    header << "inline constexpr uint32_t kShutdownObject = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullShutdownObject << "u;\n";
    header << "inline constexpr uint32_t kDpcObject = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullDpcObject << "u;\n";
    header << "inline constexpr uint32_t kTimerObject = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullTimerObject << "u;\n";
    header << "inline constexpr uint32_t kDpcContext = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullDpcContext << "u;\n";
    header << "inline constexpr uint32_t kHardwareSlot = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullHardwareSlot << "u;\n";
    header << "inline constexpr uint32_t kVersionSlot = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullVersionSlot << "u;\n";
    header << "inline constexpr uint32_t kHandlerMarker = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullHandlerMarker << "u;\n";
    header << "inline constexpr uint32_t kDebe8 = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullDebe8 << "u;\n";
    header << "inline constexpr uint32_t kDebec = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullDebec << "u;\n";
    header << "inline constexpr uint32_t kHandleOut = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullHandleOut << "u;\n";
    header << "inline constexpr uint32_t kIdOut = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullIdOut << "u;\n";
    header << "inline constexpr uint32_t kStatusOut = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullStatusOut << "u;\n";
    header << "inline constexpr uint32_t kCloseStatusOut = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullCloseStatusOut << "u;\n";
    header << "inline constexpr uint32_t kSlotBase = 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << kFullSlotBase << "u;\n";
    header << "inline constexpr uint32_t SlotFor(uint32_t ordinal) { return kSlotBase + ordinal * 8; }\n\n";
    header << "}  // namespace authentic_run_full_fixture\n";
    if (!header) return false;

    for (const SynthStep& step : route.parent) {
        config << "\n[[function]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << step.pc << "\nend = 0x" << std::setw(8) << step.pc + step.code.size() << "\n";
    }
    for (const SynthStep& step : route.child) {
        config << "\n[[function]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << step.pc << "\nend = 0x" << std::setw(8) << step.pc + step.code.size() << "\n";
    }
    config << "\n[[function]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << kFullHandler << "\nend = 0x" << std::setw(8) << kFullHandlerEnd << "\n";
    config << "\n[[function]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << route.parent_stop << "\nend = 0x" << std::setw(8) << route.parent_stop + 1 << "\n";
    config << "\n[[function]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << route.child_stop << "\nend = 0x" << std::setw(8) << route.child_stop + 1 << "\n";
    config << "\n[[function]]\nat = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << route.child_failure << "\nend = 0x" << std::setw(8) << route.child_failure + 10 << "\n";
    config << std::dec;
    return true;
}

bool Write(const std::filesystem::path& xbe_path, const std::filesystem::path& config_path,
           const std::filesystem::path& header_path) {
    std::vector<Chunk> chunks = {
        {0x00011000u, {0x68, 0x34, 0x12, 0x00, 0x00}},
        {0x00011005u, {0x59}},
        {0x00011006u, {0x81, 0xF9, 0x34, 0x12, 0x00, 0x00}},
        {0x0001100Cu, {0x74, 0x06}},
        {0x00011014u, {0x68, 0x00, 0x11, 0x01, 0x00}},
        {0x00011019u, {0x6A, 0x00}},
        {0x0001101Bu, {0x6A, 0x00}},
        {0x0001101Du, {0x6A, 0x00}},
        {0x0001101Fu, {0x68, 0x80, 0x12, 0x01, 0x00}},
        {0x00011024u, {0x68, 0x04, 0x30, 0x01, 0x00}},
        {0x00011029u, {0x68, 0x94, 0x00, 0x00, 0x00}},
        {0x0001102Eu, {0x68, 0x00, 0x00, 0x01, 0x00}},
        {0x00011033u, {0x6A, 0x00}},
        {0x00011035u, {0x68, 0x00, 0x30, 0x01, 0x00}},
        {0x0001103Au, {0xFF, 0x15, 0x00, 0x40, 0x01, 0x00}},
        {0x00011040u, {0xA3, 0x08, 0x30, 0x01, 0x00}},
        {0x00011045u, {0xFF, 0x35, 0x00, 0x30, 0x01, 0x00}},
        {0x0001104Bu, {0xFF, 0x15, 0x04, 0x40, 0x01, 0x00}},
        {0x00011051u, {0xA3, 0x0C, 0x30, 0x01, 0x00}},
        {0x00011200u, {0x68, 0x90, 0x12, 0x01, 0x00}},
        {0x00011205u, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}},
        {0x0001120Bu, {0x50}},
        {0x0001120Cu, {0x8B, 0x44, 0x24, 0x10}},
        {0x00011210u, {0x89, 0x6C, 0x24, 0x10}},
        {0x00011214u, {0x8D, 0x6C, 0x24, 0x10}},
        {0x00011218u, {0x29, 0xC4}},
        {0x0001121Au, {0x53}},
        {0x0001121Bu, {0x56}},
        {0x0001121Cu, {0x57}},
        {0x0001121Du, {0x8B, 0x45, 0xF8}},
        {0x00011220u, {0x89, 0x65, 0xE8}},
        {0x00011223u, {0x50}},
        {0x00011224u, {0x8B, 0x45, 0xFC}},
        {0x00011227u, {0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF}},
        {0x0001122Eu, {0x89, 0x45, 0xF8}},
        {0x00011231u, {0x8D, 0x45, 0xF0}},
        {0x00011234u, {0x64, 0xA3, 0x00, 0x00, 0x00, 0x00}},
        {0x0001123Au, {0xC3}},
        {0x00011100u, {0x68, 0x18, 0x00, 0x00, 0x00}},
        {0x00011105u, {0x68, 0x10, 0x30, 0x01, 0x00}},
        {0x0001110Au, {0xE8, 0xF1, 0x00, 0x00, 0x00}},
        {0x0001110Fu, {0x83, 0x65, 0xFC, 0x00}},
        {0x00011113u, {0x64, 0xA1, 0x28, 0x00, 0x00, 0x00}},
        {0x00011119u, {0x8B, 0x50, 0x28}},
        {0x0001111Cu, {0x8D, 0x4A, 0x04}},
        {0x0001111Fu, {0x89, 0x0A}},
        {0x00011121u, {0x68, 0x20, 0x30, 0x01, 0x00}},
        {0x00011126u, {0xFF, 0x15, 0x08, 0x40, 0x01, 0x00}},
        {0x0001112Cu, {0x68, 0x20, 0x30, 0x01, 0x00}},
        {0x00011131u, {0xFF, 0x15, 0x0C, 0x40, 0x01, 0x00}},
        {0x00011137u, {0x8B, 0x55, 0x08}},
        {0x0001113Au, {0xFF, 0x75, 0x0C}},
        {0x0001113Du, {0xFF, 0xD2}},
        {0x00011056u, {0xC3}},
        {0x00011280u, {0xB8, 0xEF, 0xBE, 0xAD, 0xDE}},
        {0x00011285u, {0xC3}},
        {0x00011500u, {0xE8, 0x1B, 0x00, 0x00, 0x00}},
        {0x00011505u, {0xC3}},
        {0x00011520u, {0xA1, 0x30, 0x30, 0x01, 0x00}},
        {0x00011525u, {0x0F, 0xB7, 0x48, 0x04}},
        {0x00011529u, {0x81, 0xF9, 0xCE, 0x16, 0x00, 0x00}},
        {0x0001152Fu, {0x75, 0x23}},
        {0x00011531u, {0x64, 0xA1, 0x20, 0x00, 0x00, 0x00}},
        {0x00011537u, {0x83, 0xB8, 0x50, 0x02, 0x00, 0x00, 0x00}},
        {0x0001153Eu, {0x75, 0x14}},
        {0x00011540u, {0xA1, 0x3C, 0x00, 0x01, 0x80}},
        {0x00011545u, {0xA3, 0x3C, 0x30, 0x01, 0x00}},
        {0x0001154Au, {0xA1, 0xF0, 0xFF, 0x00, 0x80}},
        {0x0001154Fu, {0xA3, 0x4C, 0x30, 0x01, 0x00}},
        {0x00011554u, {0xC3}},
        {0x00011580u, {0xFA}},
        {0x00011581u, {0xC3}},
        {0x00011582u, {0xFB}},
        {0x00011583u, {0xC3}},
        {0x00011290u, {0xC7, 0x05, 0x14, 0x30, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0xC0, 0xC3}},
        {0x0001100Eu, {0xBB, 0xAD, 0x0B, 0x00, 0x00}},
        {0x00011300u, {0x87, 0x07}},
        {0x00011302u, {0xC3}},
        {0x00011310u, {0xA1, 0x00, 0x00, 0x08, 0x00}},
        {0x00011315u, {0xC3}},
        {0x00011400u, {0x68, 0x34, 0x12, 0x00, 0x00}},
        {0x00011405u, {0xE8, 0x06, 0x00, 0x00, 0x00}},
        {0x00011410u, {0x89, 0xC0}},
        {0x00011412u, {0xC2, 0x04, 0x00}},
        {0x0001140Au, {0xC3}},
        {0x00011600u, {0xE9, 0xFF, 0xFF, 0xFF, 0xFF}},
        {0x00011604u, {0xE9, 0xF7, 0xFF, 0xFF, 0xFF}},
        {0x00011610u, {0xE9, 0x0B, 0x00, 0x00, 0x00}},
    };
    std::ostringstream full_config;
    if (!WriteFullRoute(chunks, header_path, full_config)) return false;
    std::vector<uint8_t> compact(kCompactSize, 0xCC);
    std::vector<uint8_t> full(kFullSize, 0xCC);
    for (const Chunk& chunk : chunks) {
        if (chunk.address < kFullParentBase) {
            if (chunk.address < kEntry || chunk.address + chunk.bytes.size() > kEntry + kCompactSize) return false;
            std::copy(chunk.bytes.begin(), chunk.bytes.end(), compact.begin() + (chunk.address - kEntry));
        } else {
            if (chunk.address < kFullParentBase || chunk.address + chunk.bytes.size() > kFullParentBase + kFullSize)
                return false;
            std::copy(chunk.bytes.begin(), chunk.bytes.end(), full.begin() + (chunk.address - kFullParentBase));
        }
    }
    constexpr size_t header_size = 0x1000;
    std::vector<uint8_t> image(kRawCode + compact.size() + full.size(), 0);
    std::copy_n("XBEH", 4, image.begin());
    Put32(image, 0x104, kBase);
    Put32(image, 0x108, header_size);
    Put32(image, 0x10C, kImageSize);
    Put32(image, 0x11C, 2);
    Put32(image, 0x120, kBase + kSectionTable);
    Put32(image, 0x128, kEntry ^ xbe::kRetailEntryKey);
    Put32(image, 0x158, (kBase + 0x280) ^ xbe::kRetailThunkKey);
    Put32(image, kSectionTable + 0, xbe::kExecutable | xbe::kPreload);
    Put32(image, kSectionTable + 4, kEntry);
    Put32(image, kSectionTable + 8, kCompactSize);
    Put32(image, kSectionTable + 12, kRawCode);
    Put32(image, kSectionTable + 16, uint32_t(compact.size()));
    Put32(image, kSectionTable + 20, kBase + kSectionName);
    Put32(image, kSectionTable + 56 + 0, xbe::kExecutable | xbe::kPreload);
    Put32(image, kSectionTable + 56 + 4, kFullParentBase);
    Put32(image, kSectionTable + 56 + 8, kFullSize);
    Put32(image, kSectionTable + 56 + 12, kFullRaw);
    Put32(image, kSectionTable + 56 + 16, uint32_t(full.size()));
    Put32(image, kSectionTable + 56 + 20, kBase + kSectionName);
    std::copy_n(".text", 6, image.begin() + kSectionName);
    std::copy(compact.begin(), compact.end(), image.begin() + kRawCode);
    std::copy(full.begin(), full.end(), image.begin() + kFullRaw);
    std::ofstream xbe(xbe_path, std::ios::binary);
    xbe.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    if (!xbe) return false;
    std::ofstream config(config_path);
    config << "[xbe]\n"
              "path = \"authentic_run_fixture.xbe\"\n"
              "entry = 0x11000\n"
              "out = \"generated\"\n"
              "partitions = 1\n"
              "code_sections = [\".text\"]\n"
              "pointer_seeds = false\n"
              "gap_fill = false\n"
              "callback_seeds = false\n"
              "entry_seeds = false\n"
              "checked_memory = true\n";
    config << "\n[[function]]\nat = 0x00011000\nend = 0x00011005\n";
    config << "\n[[function]]\nat = 0x00011005\nend = 0x00011006\n";
    config << "\n[[function]]\nat = 0x00011006\nend = 0x0001100C\n";
    config << "\n[[function]]\nat = 0x0001100C\nend = 0x0001100E\n";
    config << "\n[[function]]\nat = 0x0001100E\nend = 0x00011013\n";
    config << "\n[[function]]\nat = 0x00011014\nend = 0x00011019\n";
    config << "\n[[function]]\nat = 0x00011019\nend = 0x0001101B\n";
    config << "\n[[function]]\nat = 0x0001101B\nend = 0x0001101D\n";
    config << "\n[[function]]\nat = 0x0001101D\nend = 0x0001101F\n";
    config << "\n[[function]]\nat = 0x0001101F\nend = 0x00011024\n";
    config << "\n[[function]]\nat = 0x00011024\nend = 0x00011029\n";
    config << "\n[[function]]\nat = 0x00011029\nend = 0x0001102E\n";
    config << "\n[[function]]\nat = 0x0001102E\nend = 0x00011033\n";
    config << "\n[[function]]\nat = 0x00011033\nend = 0x00011035\n";
    config << "\n[[function]]\nat = 0x00011035\nend = 0x0001103A\n";
    config << "\n[[function]]\nat = 0x0001103A\nend = 0x00011040\n";
    config << "\n[[function]]\nat = 0x00011040\nend = 0x00011045\n";
    config << "\n[[function]]\nat = 0x00011045\nend = 0x0001104B\n";
    config << "\n[[function]]\nat = 0x0001104B\nend = 0x00011051\n";
    config << "\n[[function]]\nat = 0x00011051\nend = 0x00011056\n";
    config << "\n[[function]]\nat = 0x00011056\nend = 0x00011057\n";
    config << "\n[[function]]\nat = 0x00011100\nend = 0x00011105\n";
    config << "\n[[function]]\nat = 0x00011105\nend = 0x0001110A\n";
    config << "\n[[function]]\nat = 0x0001110A\nend = 0x0001110F\n";
    config << "\n[[function]]\nat = 0x0001110F\nend = 0x00011113\n";
    config << "\n[[function]]\nat = 0x00011113\nend = 0x00011119\n";
    config << "\n[[function]]\nat = 0x00011119\nend = 0x0001111C\n";
    config << "\n[[function]]\nat = 0x0001111C\nend = 0x0001111F\n";
    config << "\n[[function]]\nat = 0x0001111F\nend = 0x00011121\n";
    config << "\n[[function]]\nat = 0x00011121\nend = 0x00011126\n";
    config << "\n[[function]]\nat = 0x00011126\nend = 0x0001112C\n";
    config << "\n[[function]]\nat = 0x0001112C\nend = 0x00011131\n";
    config << "\n[[function]]\nat = 0x00011131\nend = 0x00011137\n";
    config << "\n[[function]]\nat = 0x00011137\nend = 0x0001113A\n";
    config << "\n[[function]]\nat = 0x0001113A\nend = 0x0001113D\n";
    config << "\n[[function]]\nat = 0x0001113D\nend = 0x0001113F\n";
    config << "\n[[function]]\nat = 0x00011200\nend = 0x00011205\n";
    config << "\n[[function]]\nat = 0x00011205\nend = 0x0001120B\n";
    config << "\n[[function]]\nat = 0x0001120B\nend = 0x0001120C\n";
    config << "\n[[function]]\nat = 0x0001120C\nend = 0x00011210\n";
    config << "\n[[function]]\nat = 0x00011210\nend = 0x00011214\n";
    config << "\n[[function]]\nat = 0x00011214\nend = 0x00011218\n";
    config << "\n[[function]]\nat = 0x00011218\nend = 0x0001121A\n";
    config << "\n[[function]]\nat = 0x0001121A\nend = 0x0001121B\n";
    config << "\n[[function]]\nat = 0x0001121B\nend = 0x0001121C\n";
    config << "\n[[function]]\nat = 0x0001121C\nend = 0x0001121D\n";
    config << "\n[[function]]\nat = 0x0001121D\nend = 0x00011220\n";
    config << "\n[[function]]\nat = 0x00011220\nend = 0x00011223\n";
    config << "\n[[function]]\nat = 0x00011223\nend = 0x00011224\n";
    config << "\n[[function]]\nat = 0x00011224\nend = 0x00011227\n";
    config << "\n[[function]]\nat = 0x00011227\nend = 0x0001122E\n";
    config << "\n[[function]]\nat = 0x0001122E\nend = 0x00011231\n";
    config << "\n[[function]]\nat = 0x00011231\nend = 0x00011234\n";
    config << "\n[[function]]\nat = 0x00011234\nend = 0x0001123A\n";
    config << "\n[[function]]\nat = 0x0001123A\nend = 0x0001123B\n";
    config << "\n[[function]]\nat = 0x0001113F\nend = 0x00011140\n";
    config << "\n[[function]]\nat = 0x00011280\nend = 0x00011285\n";
    config << "\n[[function]]\nat = 0x00011285\nend = 0x00011286\n";
    config << "\n[[function]]\nat = 0x00011500\nend = 0x00011505\n";
    config << "\n[[function]]\nat = 0x00011505\nend = 0x00011506\n";
    config << "\n[[function]]\nat = 0x00011520\nend = 0x00011525\n";
    config << "\n[[function]]\nat = 0x00011525\nend = 0x00011529\n";
    config << "\n[[function]]\nat = 0x00011529\nend = 0x0001152F\n";
    config << "\n[[function]]\nat = 0x0001152F\nend = 0x00011531\n";
    config << "\n[[function]]\nat = 0x00011531\nend = 0x00011537\n";
    config << "\n[[function]]\nat = 0x00011537\nend = 0x0001153E\n";
    config << "\n[[function]]\nat = 0x0001153E\nend = 0x00011540\n";
    config << "\n[[function]]\nat = 0x00011540\nend = 0x00011545\n";
    config << "\n[[function]]\nat = 0x00011545\nend = 0x0001154A\n";
    config << "\n[[function]]\nat = 0x0001154A\nend = 0x0001154F\n";
    config << "\n[[function]]\nat = 0x0001154F\nend = 0x00011554\n";
    config << "\n[[function]]\nat = 0x00011554\nend = 0x00011555\n";
    config << "\n[[function]]\nat = 0x00011580\nend = 0x00011581\n";
    config << "\n[[function]]\nat = 0x00011581\nend = 0x00011582\n";
    config << "\n[[function]]\nat = 0x00011582\nend = 0x00011583\n";
    config << "\n[[function]]\nat = 0x00011583\nend = 0x00011584\n";
    config << "\n[[function]]\nat = 0x00011290\nend = 0x0001129D\n";
    config << "\n[[function]]\nat = 0x00011300\nend = 0x00011302\n";
    config << "\n[[function]]\nat = 0x00011302\nend = 0x00011303\n";
    config << "\n[[function]]\nat = 0x00011310\nend = 0x00011315\n";
    config << "\n[[function]]\nat = 0x00011315\nend = 0x00011316\n";
    config << "\n[[function]]\nat = 0x00011400\nend = 0x00011405\n";
    config << "\n[[function]]\nat = 0x00011405\nend = 0x0001140A\n";
    config << "\n[[function]]\nat = 0x0001140A\nend = 0x0001140B\n";
    config << "\n[[function]]\nat = 0x00011410\nend = 0x00011412\n";
    config << "\n[[function]]\nat = 0x00011412\nend = 0x00011415\n";
    config << "\n[[function]]\nat = 0x00011600\nend = 0x00011605\n";
    config << "\n[[function]]\nat = 0x00011604\nend = 0x00011609\n";
    config << "\n[[function]]\nat = 0x00011610\nend = 0x00011615\n";
    config << full_config.str();
    return bool(config);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: authentic_run_fixture_builder <xbe> <config> <route-header>\n");
        return 2;
    }
    return Write(argv[1], argv[2], argv[3]) ? 0 : 1;
}
