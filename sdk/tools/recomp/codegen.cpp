#include "codegen.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdarg>

namespace fs = std::filesystem;

static std::string Fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string Fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string CodeGen::Hex(uint32_t v) const { return Fmt("0x%Xu", v); }
std::string CodeGen::FuncName(uint32_t va) const { return Fmt("sub_%08X", va); }

std::string CodeGen::CType(unsigned bits, bool s) const {
    switch (bits) {
        case 8: return s ? "int8_t" : "uint8_t";
        case 16: return s ? "int16_t" : "uint16_t";
        case 64: return s ? "int64_t" : "uint64_t";
        default: return s ? "int32_t" : "uint32_t";
    }
}

static const char* Reg32Name(ZydisRegister r) {
    switch (r) {
        case ZYDIS_REGISTER_EAX: case ZYDIS_REGISTER_AX: case ZYDIS_REGISTER_AL: case ZYDIS_REGISTER_AH: return "ctx.eax";
        case ZYDIS_REGISTER_ECX: case ZYDIS_REGISTER_CX: case ZYDIS_REGISTER_CL: case ZYDIS_REGISTER_CH: return "ctx.ecx";
        case ZYDIS_REGISTER_EDX: case ZYDIS_REGISTER_DX: case ZYDIS_REGISTER_DL: case ZYDIS_REGISTER_DH: return "ctx.edx";
        case ZYDIS_REGISTER_EBX: case ZYDIS_REGISTER_BX: case ZYDIS_REGISTER_BL: case ZYDIS_REGISTER_BH: return "ctx.ebx";
        case ZYDIS_REGISTER_ESP: case ZYDIS_REGISTER_SP: return "ctx.esp";
        case ZYDIS_REGISTER_EBP: case ZYDIS_REGISTER_BP: return "ctx.ebp";
        case ZYDIS_REGISTER_ESI: case ZYDIS_REGISTER_SI: return "ctx.esi";
        case ZYDIS_REGISTER_EDI: case ZYDIS_REGISTER_DI: return "ctx.edi";
        default: return nullptr;
    }
}

static bool IsHigh8(ZydisRegister r) {
    return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH;
}

static int RegClassIndex(ZydisRegister r, ZydisRegister first) { return int(r - first); }

std::string CodeGen::RegRead(ZydisRegister r) const {
    if (const char* n = Reg32Name(r)) {
        unsigned w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
        if (w == 32) return n;
        if (w == 16) return Fmt("((uint16_t)%s)", n);
        if (IsHigh8(r)) return Fmt("((uint8_t)(%s >> 8))", n);
        return Fmt("((uint8_t)%s)", n);
    }
    if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7) return Fmt("X87_ST(%d)", RegClassIndex(r, ZYDIS_REGISTER_ST0));
    if (r >= ZYDIS_REGISTER_MM0 && r <= ZYDIS_REGISTER_MM7) return Fmt("ctx.mm[%d]", RegClassIndex(r, ZYDIS_REGISTER_MM0));
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM7) return Fmt("ctx.xmm[%d]", RegClassIndex(r, ZYDIS_REGISTER_XMM0));
    return "0u";
}

std::string CodeGen::RegWrite(ZydisRegister r, const std::string& v) const {
    if (const char* n = Reg32Name(r)) {
        unsigned w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
        if (w == 32) return Fmt("%s = (uint32_t)(%s);", n, v.c_str());
        if (w == 16) return Fmt("%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);", n, n, v.c_str());
        if (IsHigh8(r)) return Fmt("%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);", n, n, v.c_str());
        return Fmt("%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);", n, n, v.c_str());
    }
    if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7)
        return Fmt("X87_ST(%d) = %s;", RegClassIndex(r, ZYDIS_REGISTER_ST0), v.c_str());
    if (r >= ZYDIS_REGISTER_MM0 && r <= ZYDIS_REGISTER_MM7)
        return Fmt("ctx.mm[%d] = %s;", RegClassIndex(r, ZYDIS_REGISTER_MM0), v.c_str());
    return Fmt("/* write to segment or unknown register */ (void)(%s);", v.c_str());
}

std::string CodeGen::AddrExpr(const ZydisDecodedOperand& o, bool seg) const {
    std::string e;
    if (o.mem.base != ZYDIS_REGISTER_NONE) {
        const char* n = Reg32Name(o.mem.base);
        e = n ? n : "0u";
    }
    if (o.mem.index != ZYDIS_REGISTER_NONE) {
        const char* n = Reg32Name(o.mem.index);
        if (!e.empty()) e += " + ";
        e += n ? n : "0u";
        if (o.mem.scale > 1) e += Fmt(" * %uu", o.mem.scale);
    }
    uint32_t disp = uint32_t(o.mem.disp.value);
    if (o.mem.disp.has_displacement && disp) {
        if (!e.empty()) e += " + ";
        e += Hex(disp);
    }
    if (e.empty()) e = "0u";
    if (seg && o.mem.segment == ZYDIS_REGISTER_FS) e = "ctx.fs_base + " + e;
    return "(uint32_t)(" + e + ")";
}

std::string CodeGen::Mem(unsigned bits, const std::string& ea) const {
    switch (bits) {
        case 8: return "X86_MEM8(" + ea + ")";
        case 16: return "X86_MEM16(" + ea + ")";
        case 64: return "X86_MEM64(" + ea + ")";
        default: return "X86_MEM32(" + ea + ")";
    }
}

std::string CodeGen::Read(const ZydisDecodedOperand& o, const std::string& ea) const {
    switch (o.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: return RegRead(o.reg.value);
        case ZYDIS_OPERAND_TYPE_MEMORY:
            return an_.cfg.checked_memory ? Fmt("x86_mem_read<%s>(0x%Xu, %s)", CType(o.size).c_str(), instruction_pc_, ea.c_str())
                                          : Mem(o.size, ea);
        case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
            uint64_t v = o.imm.value.u;
            return Hex(uint32_t(v));
        }
        default: return "0u";
    }
}

std::string CodeGen::Write(const ZydisDecodedOperand& o, const std::string& ea, const std::string& v) const {
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) return RegWrite(o.reg.value, v);
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (an_.cfg.checked_memory)
            return Fmt("x86_mem_write<%s>(0x%Xu, %s, (%s)(%s));", CType(o.size).c_str(), instruction_pc_, ea.c_str(),
                       CType(o.size).c_str(), v.c_str());
        return Mem(o.size, ea) + " = (" + CType(o.size) + ")(" + v + ");";
    }
    return "/* bad write */";
}

std::string CodeGen::Text(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) const {
    static ZydisFormatter fmt = [] {
        ZydisFormatter f;
        ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
        return f;
    }();
    char buf[256];
    ZydisFormatterFormatInstruction(&fmt, &in, ops, in.operand_count_visible, buf, sizeof(buf), va, nullptr);
    for (char* p = buf; *p; p++)
        if (*p == '*' && p[1] == '/') *p = 'x';
    return buf;
}

void CodeGen::Unimpl(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    std::string t = Text(va, in, ops);
    unimpl_[ZydisMnemonicGetString(in.mnemonic)]++;
    std::string esc;
    for (char c : t) esc += c == '"' ? '\'' : c;
    Line(Fmt("\t%s(ctx, 0x%X, \"unimplemented: %s\");", an_.cfg.checked_memory ? "x86_checked_trap" : "x86_trap", va,
             esc.c_str()));
}

std::string CodeGen::Target(uint32_t t, uint32_t va) {
    if (body_set_.count(t) && t != entry_) {
        labels_.insert(t);
        return Fmt("goto loc_%08X;", t);
    }
    if (t == entry_) {
        labels_.insert(t);
        return Fmt("goto loc_%08X;", t);
    }
    // A jump leaving this function is a tail transfer: the target's RET completes
    // the caller's frame, so no return address is pushed. Checked code admits it
    // exactly like a checked CALL instead of calling the host function directly.
    if (an_.cfg.checked_memory)
        return Fmt("{ X86_SAVE_FLAGS(); x86_checked_jmp(ctx, base, 0x%Xu, 0x%Xu); return; }", t, va);
    if (an_.functions.count(t)) return Fmt("{ X86_SAVE_FLAGS(); %s(ctx, base); return; }", FuncName(t).c_str());
    return Fmt("{ X86_SAVE_FLAGS(); x86_icall(ctx, base, 0x%Xu); return; }", t);
}

std::string CodeGen::Cond(ZydisMnemonic m) {
    if (pending_.valid) {
        unsigned w = pending_.bits;
        std::string U = "(" + CType(w) + ")", S = "(" + CType(w, true) + ")";
        std::string a = U + "ca", b = U + "cb", sa = S + "ca", sb = S + "cb";
        if (pending_.is_test) {
            std::string r = "(" + U + "(ca & cb))", sr = "(" + S + "(ca & cb))";
            switch (m) {
                case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return r + " == 0";
                case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return r + " != 0";
                case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS:
                case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return sr + " < 0";
                case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS:
                case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return sr + " >= 0";
                case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return sr + " <= 0";
                case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return sr + " > 0";
                case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return r + " == 0";
                case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return r + " != 0";
                default: break;
            }
        } else {
            switch (m) {
                case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return a + " == " + b;
                case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return a + " != " + b;
                case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: return a + " < " + b;
                case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: return a + " >= " + b;
                case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return a + " <= " + b;
                case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return a + " > " + b;
                case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return sa + " < " + sb;
                case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return sa + " >= " + sb;
                case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return sa + " <= " + sb;
                case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return sa + " > " + sb;
                case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return "(" + S + "(" + a + " - " + b + ")) < 0";
                case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return "(" + S + "(" + a + " - " + b + ")) >= 0";
                default: break;
            }
        }
    }
    switch (m) {
        case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO: return "of";
        case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_CMOVNO: return "!of";
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: case ZYDIS_MNEMONIC_FCMOVB: return "cf";
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: case ZYDIS_MNEMONIC_FCMOVNB: return "!cf";
        case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: case ZYDIS_MNEMONIC_FCMOVE: return "zf";
        case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_FCMOVNE: return "!zf";
        case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_FCMOVBE: return "(cf | zf)";
        case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: case ZYDIS_MNEMONIC_FCMOVNBE: return "!(cf | zf)";
        case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return "sf";
        case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return "!sf";
        case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP: case ZYDIS_MNEMONIC_FCMOVU: return "pf";
        case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP: case ZYDIS_MNEMONIC_CMOVNP: case ZYDIS_MNEMONIC_FCMOVNU: return "!pf";
        case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return "(sf != of)";
        case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return "(sf == of)";
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return "(zf | (sf != of))";
        case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return "(!zf && sf == of)";
        case ZYDIS_MNEMONIC_JCXZ: return "(uint16_t)ctx.ecx == 0";
        case ZYDIS_MNEMONIC_JECXZ: return "ctx.ecx == 0";
        default: return "false /* unknown condition */";
    }
}

void CodeGen::EmitInsn(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, uint32_t next_body) {
    insn_count_++;
    instruction_pc_ = va;
    bool keep_pending = false;
    auto m = in.mnemonic;
    uint32_t next = va + in.length;

    bool checked_call = false;
    bool checked_jmp = false;
    if (an_.cfg.checked_memory) {
        auto is_scalar_register = [](const ZydisDecodedOperand& op) {
            return op.type == ZYDIS_OPERAND_TYPE_REGISTER && Reg32Name(op.reg.value) != nullptr;
        };
        auto is_register32 = [&](const ZydisDecodedOperand& op) {
            return is_scalar_register(op) && op.size == 32;
        };
        auto is_flat_guest_memory = [&](const ZydisDecodedOperand& op) {
            if (op.type != ZYDIS_OPERAND_TYPE_MEMORY || in.address_width != 32) return false;
            auto address_register = [](ZydisRegister reg) {
                return reg == ZYDIS_REGISTER_NONE ||
                       (Reg32Name(reg) != nullptr &&
                        ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, reg) == 32);
            };
            const auto segment = op.mem.segment;
            return (segment == ZYDIS_REGISTER_NONE || segment == ZYDIS_REGISTER_DS || segment == ZYDIS_REGISTER_SS) &&
                   address_register(op.mem.base) && address_register(op.mem.index);
        };
        auto is_checked_memory = [&](const ZydisDecodedOperand& op) {
            return is_flat_guest_memory(op) ||
                   (op.type == ZYDIS_OPERAND_TYPE_MEMORY && in.address_width == 32 &&
                    op.mem.segment == ZYDIS_REGISTER_FS &&
                    (op.mem.base == ZYDIS_REGISTER_NONE ||
                     (Reg32Name(op.mem.base) && ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, op.mem.base) == 32)) &&
                    (op.mem.index == ZYDIS_REGISTER_NONE ||
                     (Reg32Name(op.mem.index) &&
                      ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, op.mem.index) == 32)));
        };
        auto is_scalar_memory = [](const ZydisDecodedOperand& op) {
            return op.type == ZYDIS_OPERAND_TYPE_MEMORY && (op.size == 8 || op.size == 16 || op.size == 32);
        };
        auto is_qword_memory = [](const ZydisDecodedOperand& op) {
            return op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.size == 64;
        };
        auto is_mmx = [](const ZydisDecodedOperand& op) {
            return op.type == ZYDIS_OPERAND_TYPE_REGISTER && op.reg.value >= ZYDIS_REGISTER_MM0 &&
                   op.reg.value <= ZYDIS_REGISTER_MM7;
        };
        auto is_scalar_source = [&](const ZydisDecodedOperand& op) {
            return is_scalar_register(op) || is_scalar_memory(op) || op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
        };
        auto is_checked_scalar_memory = [&](const ZydisDecodedOperand& op) {
            return is_scalar_memory(op) && is_checked_memory(op);
        };
        auto is_checked_scalar_source = [&](const ZydisDecodedOperand& op) {
            return is_scalar_register(op) || is_checked_scalar_memory(op) ||
                   op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
        };
        auto is_binary_rmw = [&] {
            if (in.operand_count_visible != 2 || (ops[0].size != 8 && ops[0].size != 16 && ops[0].size != 32)) return false;
            if (!is_scalar_register(ops[0]) && !is_scalar_memory(ops[0])) return false;
            return is_scalar_memory(ops[0]) ? !is_scalar_memory(ops[1]) && is_scalar_source(ops[1]) : is_scalar_source(ops[1]);
        };
        auto is_address_register = [&](ZydisRegister reg) {
            if (Reg32Name(reg) == nullptr || ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, reg) != in.address_width)
                return false;
            if (in.address_width != 16) return true;
            return reg == ZYDIS_REGISTER_BX || reg == ZYDIS_REGISTER_BP || reg == ZYDIS_REGISTER_SI ||
                   reg == ZYDIS_REGISTER_DI;
        };
        bool checked_mov = m == ZYDIS_MNEMONIC_MOV && in.operand_count_visible == 2 &&
                           (is_scalar_register(ops[0]) || is_checked_scalar_memory(ops[0])) &&
                           is_checked_scalar_source(ops[1]) &&
                           !(is_checked_scalar_memory(ops[0]) && is_checked_scalar_memory(ops[1]));
        bool checked_extend = (m == ZYDIS_MNEMONIC_MOVZX || m == ZYDIS_MNEMONIC_MOVSX) &&
                              in.operand_count_visible == 2 && is_scalar_register(ops[0]) &&
                              (is_scalar_register(ops[1]) || is_checked_scalar_memory(ops[1])) &&
                              (ops[1].size == 8 || ops[1].size == 16) && ops[0].size > ops[1].size;
        bool checked_movq = m == ZYDIS_MNEMONIC_MOVQ && in.operand_count_visible == 2 &&
                            ((is_qword_memory(ops[0]) && is_mmx(ops[1])) ||
                             (is_mmx(ops[0]) && is_qword_memory(ops[1])));
        bool checked_rmw = ((m == ZYDIS_MNEMONIC_ADD || m == ZYDIS_MNEMONIC_SUB) && is_binary_rmw()) ||
                           (m == ZYDIS_MNEMONIC_INC && in.operand_count_visible == 1 &&
                            (is_scalar_register(ops[0]) || is_scalar_memory(ops[0])));
        bool checked_sbb = m == ZYDIS_MNEMONIC_SBB && in.operand_count_visible == 2 &&
                           is_register32(ops[0]) && is_register32(ops[1]);
        bool checked_neg_al = m == ZYDIS_MNEMONIC_NEG && in.operand_count_visible == 1 &&
                              ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                              ops[0].reg.value == ZYDIS_REGISTER_AL && ops[0].size == 8;
        bool checked_or = m == ZYDIS_MNEMONIC_OR && in.operand_count_visible == 2 &&
                          (ops[0].size == 8 || ops[0].size == 16 || ops[0].size == 32) &&
                          (is_scalar_register(ops[0]) || is_checked_scalar_memory(ops[0])) &&
                          ((is_scalar_register(ops[1]) && ops[1].size == ops[0].size) ||
                           (ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[1].size <= ops[0].size));
        bool checked_cmp = m == ZYDIS_MNEMONIC_CMP && is_binary_rmw();
        bool checked_and = m == ZYDIS_MNEMONIC_AND && is_binary_rmw();
        bool checked_push_memory = m == ZYDIS_MNEMONIC_PUSH && in.operand_count_visible == 1 &&
                                   in.operand_width == 32 && in.stack_width == 32 && ops[0].size == 32 &&
                                   is_flat_guest_memory(ops[0]);
        bool checked_push = m == ZYDIS_MNEMONIC_PUSH && in.operand_count_visible == 1 && in.operand_width == 32 &&
                            in.stack_width == 32 &&
                            ((is_scalar_register(ops[0]) && ops[0].size == 32) ||
                             (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                              (in.raw.imm[0].size == 8 || in.raw.imm[0].size == 32)) || checked_push_memory);
        bool checked_pop = m == ZYDIS_MNEMONIC_POP && in.operand_count_visible == 1 && in.operand_width == 32 &&
                           in.stack_width == 32 && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                           (ops[0].reg.value == ZYDIS_REGISTER_ECX || ops[0].reg.value == ZYDIS_REGISTER_EBX ||
                            ops[0].reg.value == ZYDIS_REGISTER_EBP ||
                            ops[0].reg.value == ZYDIS_REGISTER_EDI || ops[0].reg.value == ZYDIS_REGISTER_ESI) &&
                           ops[0].size == 32;
        bool checked_cdq = m == ZYDIS_MNEMONIC_CDQ && in.operand_count_visible == 0 && in.operand_width == 32;
        bool checked_idiv = m == ZYDIS_MNEMONIC_IDIV && in.operand_count_visible == 1 && in.operand_width == 32 &&
                            is_register32(ops[0]) && ops[0].reg.value == ZYDIS_REGISTER_ECX;
        bool checked_xor = m == ZYDIS_MNEMONIC_XOR && in.operand_count_visible == 2 && is_register32(ops[0]) &&
                           is_register32(ops[1]);
        bool checked_test = m == ZYDIS_MNEMONIC_TEST && in.operand_count_visible == 2 && is_register32(ops[0]) &&
                            is_register32(ops[1]) && ops[0].reg.value == ops[1].reg.value;
        bool checked_test_byte_memory_imm = m == ZYDIS_MNEMONIC_TEST && in.operand_count_visible == 2 &&
                                            ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].size == 8 &&
                                            is_flat_guest_memory(ops[0]) &&
                                            ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[1].size <= 8;
        bool checked_shr = m == ZYDIS_MNEMONIC_SHR && in.operand_count_visible == 2 && in.operand_width == 32 &&
                           is_register32(ops[0]) && ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[1].size == 8;
        bool checked_shl = m == ZYDIS_MNEMONIC_SHL && in.operand_count_visible == 2 && in.operand_width == 32 &&
                           is_register32(ops[0]) && ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[1].size == 8;
        bool checked_leave = m == ZYDIS_MNEMONIC_LEAVE && in.operand_count_visible == 0 &&
                             in.operand_width == 32 && in.stack_width == 32;
        bool checked_not_al = m == ZYDIS_MNEMONIC_NOT && in.operand_count_visible == 1 &&
                              in.raw.prefix_count == 0 && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                              ops[0].reg.value == ZYDIS_REGISTER_AL && ops[0].size == 8;
        const bool rep_prefix_only = (in.attributes & ZYDIS_ATTRIB_HAS_REP) &&
                                     !(in.attributes & (ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) &&
                                     in.raw.prefix_count == 1 && in.raw.prefixes[0].value == 0xF3;
        bool checked_rep_string = rep_prefix_only && in.address_width == 32 &&
                                  ((m == ZYDIS_MNEMONIC_MOVSB && in.operand_width == 8) ||
                                   (m == ZYDIS_MNEMONIC_MOVSD && in.operand_width == 32) ||
                                   (m == ZYDIS_MNEMONIC_STOSB && in.operand_width == 8) ||
                                   (m == ZYDIS_MNEMONIC_STOSD && in.operand_width == 32));
        bool checked_lea = m == ZYDIS_MNEMONIC_LEA && in.operand_count_visible == 2 &&
                           is_scalar_register(ops[0]) && (ops[0].size == 16 || ops[0].size == 32) &&
                           ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                           (in.address_width == 16 || in.address_width == 32) &&
                           (ops[1].mem.base == ZYDIS_REGISTER_NONE || is_address_register(ops[1].mem.base)) &&
                           (ops[1].mem.index == ZYDIS_REGISTER_NONE || is_address_register(ops[1].mem.index));
        bool checked_branch = in.meta.category == ZYDIS_CATEGORY_COND_BR && in.operand_count_visible == 1 &&
                              ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
        checked_jmp = m == ZYDIS_MNEMONIC_JMP && in.operand_count_visible == 1 &&
                      ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && in.operand_width == 32 &&
                      (in.raw.imm[0].size == 8 || in.raw.imm[0].size == 32);
        bool checked_indirect_jmp = m == ZYDIS_MNEMONIC_JMP && in.operand_count_visible == 1 &&
                                    in.operand_width == 32 && in.address_width == 32 &&
                                    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].size == 32 &&
                                    is_flat_guest_memory(ops[0]);
        bool checked_indirect_register_jmp =
            m == ZYDIS_MNEMONIC_JMP && in.operand_count_visible == 1 && in.operand_width == 32 && is_register32(ops[0]);
        uint32_t call_target = m == ZYDIS_MNEMONIC_CALL && in.operand_count_visible == 1 &&
                                       ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
                                   ? uint32_t(next + ops[0].imm.value.s)
                                   : 0;
        checked_call = m == ZYDIS_MNEMONIC_CALL && in.operand_count_visible == 1 && in.operand_width == 32 &&
                       in.stack_width == 32 && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[0].size == 32 &&
                       call_target != next && an_.functions.count(call_target) && !an_.continuations.count(call_target);
        bool checked_indirect_call = m == ZYDIS_MNEMONIC_CALL && in.operand_count_visible == 1 &&
                                     in.operand_width == 32 && in.stack_width == 32 && in.address_width == 32 &&
                                     ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].size == 32 &&
                                     is_flat_guest_memory(ops[0]);
        bool checked_indirect_register_call = m == ZYDIS_MNEMONIC_CALL && in.operand_count_visible == 1 &&
                                              in.operand_width == 32 && in.stack_width == 32 &&
                                              is_register32(ops[0]);
        bool checked_ret = m == ZYDIS_MNEMONIC_RET && in.operand_count_visible <= 1 &&
                           in.operand_width == 32 && in.stack_width == 32 &&
                           (!in.operand_count_visible || (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                                                          ops[0].imm.value.u <= 0xFFFF));
        bool locked = in.attributes & ZYDIS_ATTRIB_HAS_LOCK;
        if (!(checked_mov || checked_extend || checked_movq || checked_rmw || checked_sbb || checked_neg_al || checked_cmp ||
              checked_and || checked_or || checked_push || checked_pop || checked_cdq || checked_idiv || checked_xor ||
              checked_test || checked_test_byte_memory_imm || checked_shl || checked_shr || checked_leave ||
              checked_lea || checked_not_al ||
              checked_rep_string || checked_branch || checked_call || checked_indirect_call ||
              checked_indirect_register_call || checked_jmp || checked_indirect_jmp ||
              checked_indirect_register_jmp || checked_ret) || locked) {
            Unimpl(va, in, ops);
            pending_.valid = false;
            return;
        }
    }

    // control flow first
    switch (m) {
        case ZYDIS_MNEMONIC_CALL: {
            if (an_.cfg.checked_memory) {
                if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    const uint32_t target = uint32_t(next + ops[0].imm.value.s);
                    Line(Fmt("\t{ X86_SAVE_FLAGS(); x86_checked_call(ctx, base, 0x%Xu, 0x%Xu, 0x%Xu); ", target,
                             next, va));
                } else if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    Line(Fmt("\t{ X86_SAVE_FLAGS(); x86_checked_register_call(ctx, base, %s, 0x%Xu, 0x%Xu); ",
                             Read(ops[0], "").c_str(), next, va));
                } else if (ops[0].mem.base == ZYDIS_REGISTER_NONE && ops[0].mem.index == ZYDIS_REGISTER_NONE) {
                    const uint32_t operand_address = uint32_t(ops[0].mem.disp.value);
                    Line(Fmt("\t{ X86_SAVE_FLAGS(); x86_checked_indirect_call(ctx, base, 0x%Xu, 0x%Xu, 0x%Xu); ",
                             operand_address, next, va));
                } else {
                    Line(Fmt("\t{ X86_SAVE_FLAGS(); x86_checked_indirect_call(ctx, base, %s, 0x%Xu, 0x%Xu); ",
                             AddrExpr(ops[0]).c_str(), next, va));
                }
                Line("\t  cf = ctx.cf; zf = ctx.zf; sf = ctx.sf; of = ctx.of; pf = ctx.pf; af = ctx.af; }");
            } else if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uint32_t t = uint32_t(next + ops[0].imm.value.s);
                if (t == next) {  // call $+5 only pushes eip
                    Line(Fmt("\tX86_PUSH32(0x%Xu);", next));
                } else if (an_.functions.count(t)) {
                    Line(Fmt("\tX86_PUSH32(0x%Xu); %s(ctx, base);", next, FuncName(t).c_str()));
                } else {
                    Line(Fmt("\tX86_PUSH32(0x%Xu); x86_icall(ctx, base, 0x%Xu);", next, t));
                }
            } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_NONE &&
                       ops[0].mem.index == ZYDIS_REGISTER_NONE && ops[0].mem.segment != ZYDIS_REGISTER_FS &&
                       uint32_t(ops[0].mem.disp.value) >= an_.img.kernel_thunk &&
                       uint32_t(ops[0].mem.disp.value) < an_.img.kernel_thunk + 0x400) {
                uint32_t slot = uint32_t(ops[0].mem.disp.value), ord = 0;
                an_.img.Read32(slot, ord);
                Line(Fmt("\tX86_PUSH32(0x%Xu); x86_kernel(ctx, base, %u);", next, ord & 0x7FFFFFFF));
            } else {
                std::string ea = ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? AddrExpr(ops[0]) : "";
                Line(Fmt("\t{ uint32_t t = %s; X86_PUSH32(0x%Xu); x86_icall(ctx, base, t); }", Read(ops[0], ea).c_str(), next));
            }
            pending_.valid = false;
            return;
        }
        case ZYDIS_MNEMONIC_JMP: {
            if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uint32_t t = uint32_t(next + ops[0].imm.value.s);
                // backward jumps get a compiler barrier so spin loops reread memory other threads write
                if (t <= va) Line("\tX86_BARRIER();");
                Line("\t" + Target(t, va));
            } else {
                auto jt = an_.jump_tables.find(va);
                std::string ea = ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? AddrExpr(ops[0]) : "";
                if (jt != an_.jump_tables.end()) {
                    Line(Fmt("\t{ uint32_t t = %s;", Read(ops[0], ea).c_str()));
                    Line("\tswitch (t) {");
                    std::set<uint32_t> done;
                    for (uint32_t t : jt->second.targets) {
                        if (!done.insert(t).second) continue;
                        Line(Fmt("\tcase 0x%Xu: %s", t, Target(t, va).c_str()));
                    }
                    if (an_.cfg.checked_memory)
                        Line(Fmt("\tdefault: { X86_SAVE_FLAGS(); x86_checked_jmp(ctx, base, t, 0x%Xu); return; }", va));
                    else
                        Line("\tdefault: { X86_SAVE_FLAGS(); x86_icall(ctx, base, t); return; }");
                    Line("\t} }");
                } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_NONE &&
                           ops[0].mem.index == ZYDIS_REGISTER_NONE &&
                           uint32_t(ops[0].mem.disp.value) >= an_.img.kernel_thunk &&
                           uint32_t(ops[0].mem.disp.value) < an_.img.kernel_thunk + 0x400) {
                    uint32_t ord = 0;
                    an_.img.Read32(uint32_t(ops[0].mem.disp.value), ord);
                    Line(Fmt("\tx86_kernel(ctx, base, %u); return;", ord & 0x7FFFFFFF));
                } else if (an_.cfg.checked_memory) {
                    if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
                        Line(Fmt("\t{ uint32_t t = %s; X86_SAVE_FLAGS(); x86_checked_register_jmp(ctx, base, t, 0x%Xu); return; }",
                                 Read(ops[0], ea).c_str(), va));
                    else
                        Line(Fmt("\t{ X86_SAVE_FLAGS(); x86_checked_indirect_jmp(ctx, base, %s, 0x%Xu); return; }",
                                 ea.c_str(), va));
                } else {
                    Line(Fmt("\t{ uint32_t t = %s; X86_SAVE_FLAGS(); x86_icall(ctx, base, t); return; }", Read(ops[0], ea).c_str()));
                }
            }
            pending_.valid = false;
            return;
        }
        case ZYDIS_MNEMONIC_RET: {
            uint32_t n = in.operand_count_visible && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? uint32_t(ops[0].imm.value.u) : 0;
            if (an_.cfg.checked_memory) {
                Line("\tX86_SAVE_FLAGS();");
                Line(Fmt("\tx86_checked_ret(ctx, 0x%Xu, %u); return;", va, n));
            } else {
                Line(Fmt("\tctx.esp += %u; return;", 4 + n));
            }
            pending_.valid = false;
            return;
        }
        case ZYDIS_MNEMONIC_LOOP:
        case ZYDIS_MNEMONIC_LOOPE:
        case ZYDIS_MNEMONIC_LOOPNE: {
            uint32_t t = uint32_t(next + ops[0].imm.value.s);
            const char* extra = m == ZYDIS_MNEMONIC_LOOPE ? " && zf" : m == ZYDIS_MNEMONIC_LOOPNE ? " && !zf" : "";
            Line(Fmt("\tif (--ctx.ecx != 0%s) { X86_BARRIER(); %s }", extra, Target(t, va).c_str()));
            pending_.valid = false;
            return;
        }
        default:
            break;
    }
    if (in.meta.category == ZYDIS_CATEGORY_COND_BR) {
        uint32_t t = uint32_t(next + ops[0].imm.value.s);
        std::string c = Cond(m);
        Line(Fmt("\tif (%s) { %s%s }", c.c_str(), t <= va ? "X86_BARRIER(); " : "", Target(t, va).c_str()));
        pending_.valid = false;
        return;
    }

    bool ok = EmitInteger(va, in, ops) || EmitString(va, in, ops) || EmitX87(va, in, ops) || EmitSse(va, in, ops) ||
              EmitMmx(va, in, ops);
    if (!ok) Unimpl(va, in, ops);
    if (m == ZYDIS_MNEMONIC_CMP || m == ZYDIS_MNEMONIC_TEST) keep_pending = true;
    else if (in.cpu_flags && !(in.cpu_flags->modified | in.cpu_flags->set_0 | in.cpu_flags->set_1 | in.cpu_flags->undefined))
        keep_pending = pending_.valid;
    if (!keep_pending) pending_.valid = false;
}
