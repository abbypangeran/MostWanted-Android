#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cpu/x86_context.h"

namespace dispatch {

bool Init(std::string& error);
void Reset();
uint32_t KernelThunkAddress(uint32_t ordinal);

struct Metadata {
    uint32_t code_base;
    uint32_t code_end;
    uint32_t entry_point;
    std::span<const X86FuncMapping> functions;
    std::span<const uint32_t> continuations;
};

bool Init(const Metadata& metadata, std::string& error);
bool IsContinuation(uint32_t guest);

class Table {
public:
    bool Initialize(const Metadata& metadata, std::string& error);
    void Reset();
    X86Func* Find(uint32_t guest) const;
    bool IsContinuation(uint32_t guest) const;
    uint32_t CodeBase() const { return code_base_; }
    uint32_t CodeEnd() const { return code_end_; }

private:
    uint32_t code_base_ = 0;
    uint32_t code_end_ = 0;
    std::vector<X86Func*> functions_;
    std::vector<bool> continuations_;
};

}  // namespace dispatch
